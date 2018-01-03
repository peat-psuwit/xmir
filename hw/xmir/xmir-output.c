/*
 * Copyright © 2015-2017 Canonical Ltd
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "xmir.h"
#include <randrstr.h>
#include "glamor_priv.h"
#include "mipointer.h"

static Rotation
to_rr_rotation(MirOrientation orient)
{
    switch (orient) {
    default: return RR_Rotate_0;
    case mir_orientation_left: return RR_Rotate_90;
    case mir_orientation_inverted: return RR_Rotate_180;
    case mir_orientation_right: return RR_Rotate_270;
    }
}

Bool
xmir_output_dpms(struct xmir_screen *xmir_screen, int mode)
{
    MirDisplayConfig *display_config = xmir_screen->display;
    MirPowerMode mir_mode = mir_power_mode_on;
    Bool unchanged = TRUE;
    int num_outputs;

    if (xmir_screen->rootless || xmir_screen->windowed)
        return FALSE;

    switch (mode) {
    case DPMSModeOn:      mir_mode = mir_power_mode_on; break;
    case DPMSModeStandby: mir_mode = mir_power_mode_standby; break;
    case DPMSModeSuspend: mir_mode = mir_power_mode_suspend; break;
    case DPMSModeOff:     mir_mode = mir_power_mode_off; break;
    }

    DebugF("Setting DPMS mode to %d\n", mode);

    num_outputs = mir_display_config_get_num_outputs(display_config);
    for (int i = 0; i < num_outputs; i++) {
        MirOutput *output = mir_display_config_get_mutable_output(display_config, i);
        MirPowerMode power_mode = mir_output_get_power_mode(output);
        if (power_mode != mir_mode) {
            mir_output_set_power_mode(output, mir_mode);
            unchanged = FALSE;
        }
    }

    if (!unchanged)
        mir_connection_apply_session_display_config(xmir_screen->conn,
                                                    xmir_screen->display);

    return TRUE;
}

static void
xmir_output_update(struct xmir_output *xmir_output, MirOutput const *mir_output)
{
    MirOutputConnectionState connection_state;
    bool output_is_connected;

    connection_state = mir_output_get_connection_state(mir_output);
    output_is_connected = !(connection_state == mir_output_connection_state_disconnected);

    RROutputSetConnection(xmir_output->randr_output,
                          output_is_connected ? RR_Connected : RR_Disconnected);
    RROutputSetSubpixelOrder(xmir_output->randr_output, SubPixelUnknown);

    if (output_is_connected) {
        MirOutputMode const *mode = mir_output_get_current_mode(mir_output);
        RRModePtr randr_mode;
        double refresh_rate;

        xmir_output->width = mir_output_mode_get_width(mode);
        xmir_output->height = mir_output_mode_get_height(mode);
        xmir_output->x = mir_output_get_position_x(mir_output);
        xmir_output->y = mir_output_get_position_y(mir_output);

        refresh_rate = mir_output_mode_get_refresh_rate(mode);
        randr_mode = xmir_cvt(xmir_output->width, xmir_output->height,
                              refresh_rate, 0, 0);
        /* Odd resolutions like 1366x768 don't show correctly otherwise */
        randr_mode->mode.width = mir_output_mode_get_width(mode);
        randr_mode->mode.height = mir_output_mode_get_height(mode);
        sprintf(randr_mode->name, "%dx%d",
                randr_mode->mode.width,
                randr_mode->mode.height);

        RROutputSetPhysicalSize(xmir_output->randr_output,
                                mir_output_get_physical_width_mm(mir_output),
                                mir_output_get_physical_height_mm(mir_output));
        RROutputSetModes(xmir_output->randr_output, &randr_mode, 1, 1);

        /* Yes, Mir and XrandR's (XRender's) subpixel enums match up */
        RROutputSetSubpixelOrder(xmir_output->randr_output,
            mir_output_get_subpixel_arrangement(mir_output));

        RRCrtcNotify(xmir_output->randr_crtc, randr_mode,
                     xmir_output->x, xmir_output->y,
                     to_rr_rotation(mir_output_get_orientation(mir_output)),
                     NULL, 1, &xmir_output->randr_output);
    }
    else {
        xmir_output->width = 0;
        xmir_output->height = 0;
        xmir_output->x = 0;
        xmir_output->y = 0;

        RROutputSetPhysicalSize(xmir_output->randr_output, 0, 0);
        RROutputSetModes(xmir_output->randr_output, NULL, 0, 0);

        RRCrtcNotify(xmir_output->randr_crtc, NULL,
                     0, 0, RR_Rotate_0, NULL, 1, &xmir_output->randr_output);
    }
}

static void
xmir_screen_update_windowed_output(struct xmir_screen *xmir_screen)
{
    struct xmir_output *xmir_output = xmir_screen->windowed;
    RRModePtr randr_mode;

    RROutputSetConnection(xmir_output->randr_output, RR_Connected);
    RROutputSetSubpixelOrder(xmir_output->randr_output, SubPixelUnknown);

    xmir_output->width = xmir_screen->screen->width;
    xmir_output->height = xmir_screen->screen->height;
    xmir_output->x = 0;
    xmir_output->y = 0;

    randr_mode = xmir_cvt(xmir_output->width, xmir_output->height, 60, 0, 0);
    randr_mode->mode.width = xmir_output->width;
    randr_mode->mode.height = xmir_output->height;
    sprintf(randr_mode->name, "%dx%d",
            randr_mode->mode.width, randr_mode->mode.height);

    RROutputSetPhysicalSize(xmir_output->randr_output, 0, 0);
    RROutputSetModes(xmir_output->randr_output, &randr_mode, 1, 1);
    RRCrtcNotify(xmir_output->randr_crtc, randr_mode,
                 xmir_output->x, xmir_output->y,
                 RR_Rotate_0, NULL, 1, &xmir_output->randr_output);
}

static void
xmir_output_screen_resized(struct xmir_screen *xmir_screen)
{
    ScreenPtr screen = xmir_screen->screen;
    struct xmir_output *xmir_output;
    int width, height;

    width = 0;
    height = 0;
    xorg_list_for_each_entry(xmir_output, &xmir_screen->output_list, link) {
        if (width < xmir_output->x + xmir_output->width)
            width = xmir_output->x + xmir_output->width;
        if (height < xmir_output->y + xmir_output->height)
            height = xmir_output->y + xmir_output->height;
    }

    screen->width = width;
    screen->height = height;
    if (ConnectionInfo)
        RRScreenSizeNotify(xmir_screen->screen);
    update_desktop_dimensions();
}

static struct xmir_output*
xmir_output_create(struct xmir_screen *xmir_screen, const char *name)
{
    struct xmir_output *xmir_output;

    xmir_output = calloc(sizeof *xmir_output, 1);
    if (xmir_output == NULL) {
        FatalError("No memory for creating output\n");
        return NULL;
    }

    xmir_output->xmir_screen = xmir_screen;
    xmir_output->randr_crtc = RRCrtcCreate(xmir_screen->screen, xmir_output);
    xmir_output->randr_output = RROutputCreate(xmir_screen->screen,
                                               name, strlen(name),
                                               xmir_output);

    RRCrtcGammaSetSize(xmir_output->randr_crtc, 256);
    RROutputSetCrtcs(xmir_output->randr_output, &xmir_output->randr_crtc, 1);
    xorg_list_append(&xmir_output->link, &xmir_screen->output_list);
    return xmir_output;
}

void
xmir_output_destroy(struct xmir_output *xmir_output)
{
    xorg_list_del(&xmir_output->link);
    free(xmir_output);
}

static Bool
xmir_randr_get_info(ScreenPtr pScreen, Rotation * rotations)
{
    *rotations = 0;

    return TRUE;
}

static Bool
xmir_randr_set_config(ScreenPtr pScreen,
                     Rotation rotation, int rate, RRScreenSizePtr pSize)
{
    return FALSE;
}

static void
xmir_update_config(struct xmir_screen *xmir_screen)
{
    MirDisplayConfig *new_config;
    struct xmir_output *xmir_output;
    int old_num_outputs, new_num_outputs;
    MirOutput const *mir_output;
    int i;

    if (xmir_screen->windowed)
        return;

    new_config = mir_connection_create_display_configuration(xmir_screen->conn);
    new_num_outputs = mir_display_config_get_num_outputs(new_config);
    old_num_outputs = mir_display_config_get_num_outputs(xmir_screen->display);
    if (new_num_outputs != old_num_outputs)
        FatalError("Number of outputs changed on update.\n");

    mir_display_config_release(xmir_screen->display);
    xmir_screen->display = new_config;

    i = 0;
    xorg_list_for_each_entry(xmir_output, &xmir_screen->output_list, link) {
        mir_output = mir_display_config_get_output(new_config, i);
        xmir_output_update(xmir_output, mir_output);
        ++i;
    }

    xmir_output_screen_resized(xmir_screen);
}

void
xmir_output_handle_orientation(struct xmir_window *xmir_window,
                               MirOrientation dir)
{
    XMIR_DEBUG(("Orientation: %i\n", dir));

    xmir_output_handle_resize(xmir_window, -1, -1);
}

void
xmir_output_handle_resize(struct xmir_window *xmir_window,
                          int width, int height)
{
    WindowPtr window = xmir_window->window;
    ScreenPtr screen = window->drawable.pScreen;
    struct xmir_screen *xmir_screen = xmir_screen_get(screen);
    PixmapPtr pixmap;
    DrawablePtr oldroot = &screen->root->drawable;
    BoxRec box;
    BoxRec copy_box;

    int window_width, window_height;
    DeviceIntPtr pDev;

    MirOrientation old = xmir_window->orientation;
    xmir_window->orientation = mir_window_get_orientation(xmir_window->surface);

    if (width < 0 && height < 0) {
        if (old % 180 == xmir_window->orientation % 180) {
            window_width = window->drawable.width;
            window_height = window->drawable.height;
        }
        else {
            window_width = window->drawable.height;
            window_height = window->drawable.width;
        }
    }
    else if (xmir_window->orientation == 0 || xmir_window->orientation == 180) {
        window_width = width * (1 + xmir_screen->doubled);
        window_height = height * (1 + xmir_screen->doubled);
    }
    else {
        window_width = height * (1 + xmir_screen->doubled);
        window_height = width * (1 + xmir_screen->doubled);
    }

    if (window_width == window->drawable.width &&
        window_height == window->drawable.height) {
        /* Damage window if rotated */
        if (old != xmir_window->orientation)
            DamageDamageRegion(&window->drawable, &xmir_window->region);
        return;
    }

    /* In case of async EGL, destroy the image after swap has finished */
    if (xmir_window->image) {
        if (!xmir_window->has_free_buffer) {
            while (1) {
                xmir_process_from_eventloop();
                if (xmir_window->has_free_buffer)
                    break;
                usleep(1000);
            }
        }

        eglDestroyImageKHR(xmir_screen->egl_display, xmir_window->image);
        xmir_window->image = NULL;
    }

    if (xmir_screen->rootless)
        return;

    if (!xmir_screen->windowed) {
        XMIR_DEBUG(("Root resized, removing all outputs and inserting fake output\n"));

        while (!xorg_list_is_empty(&xmir_screen->output_list)) {
            struct xmir_output *xmir_output;

            xmir_output = xorg_list_first_entry(&xmir_screen->output_list,
                                                typeof(*xmir_output),
                                                link);

            RRCrtcDestroy(xmir_output->randr_crtc);
            RROutputDestroy(xmir_output->randr_output);
            xmir_output_destroy(xmir_output);
        }

        xmir_screen->windowed = xmir_output_create(xmir_screen, "Windowed");
        xmir_disable_screensaver(xmir_screen);
    }

    XMIR_DEBUG(("Output resized %ix%i with rotation %i\n",
                width, height, xmir_window->orientation));

    pixmap = screen->CreatePixmap(screen,
                                  window_width, window_height,
                                  screen->rootDepth,
                                  CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

    copy_box.x1 = copy_box.y1 = 0;
    copy_box.x2 = min(window_width, oldroot->width);
    copy_box.y2 = min(window_height, oldroot->height);

    if (xmir_screen->glamor) {
        glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(pixmap);
        glBindFramebuffer(GL_FRAMEBUFFER, pixmap_priv->fbo->fb);
        glClearColor(0., 0., 0., 1.);
        glClear(GL_COLOR_BUFFER_BIT);
        glamor_copy(&screen->root->drawable, &pixmap->drawable,
                              NULL, &copy_box, 1, 0, 0, FALSE, FALSE, 0, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    else {
        PixmapPtr old_pixmap = screen->GetWindowPixmap(window);
        int src_stride = old_pixmap->devKind;
        int dst_stride = pixmap->devKind;
        int bpp = oldroot->bitsPerPixel >> 3;
        const char *src = (char*)old_pixmap->devPrivate.ptr +
                          src_stride * copy_box.y1 +
                          copy_box.x1 * bpp;
        char *dst = (char*)pixmap->devPrivate.ptr +
                    dst_stride * copy_box.y1 +
                    copy_box.x1 * bpp;
        int line_len = (copy_box.x2 - copy_box.x1) * bpp;
        int y;
        for (y = copy_box.y1; y < copy_box.y2; ++y) {
            memcpy(dst, src, line_len);
            memset(dst+line_len, 0, dst_stride-line_len);
            src += src_stride;
            dst += dst_stride;
        }
        if (y < window_height)
            memset(dst, 0, (window_height - y) * dst_stride);
    }

    screen->width = window_width;
    screen->height = window_height;
    screen->mmWidth = screen->width * 254 / (10 * xmir_screen->dpi);
    screen->mmHeight = screen->height * 254 / (10 * xmir_screen->dpi);

    screen->SetScreenPixmap(pixmap);

    SetRootClip(screen, ROOT_CLIP_FULL);

    box.x1 = box.y1 = 0;
    box.x2 = window_width;
    box.y2 = window_height;
    RegionReset(&xmir_window->region, &box);
    DamageDamageRegion(&window->drawable, &xmir_window->region);

    /* Update cursor info too */
    for (pDev = inputInfo.devices; pDev; pDev = pDev->next) {
        int x, y;

        if (!IsPointerDevice(pDev))
            continue;

        miPointerGetPosition(pDev, &x, &y);
        UpdateSpriteForScreen(pDev, screen);
        miPointerSetScreen(pDev, 0, x, y);
    }

    xmir_screen_update_windowed_output(xmir_screen);
    if (ConnectionInfo)
        RRScreenSizeNotify(xmir_screen->screen);
    update_desktop_dimensions();
}

static void
xmir_handle_hotplug(struct xmir_screen *xmir_screen,
                    struct xmir_window *unused1,
                    void *unused2)
{
    xmir_update_config(xmir_screen);

    /* Trigger RANDR refresh */
    RRGetInfo(screenInfo.screens[0], TRUE);
}

static void
xmir_display_config_callback(MirConnection *conn, void *ctx)
{
    struct xmir_screen *xmir_screen = ctx;
    xmir_post_to_eventloop(xmir_handle_hotplug, xmir_screen, 0, 0);
}

Bool
xmir_screen_init_output(struct xmir_screen *xmir_screen)
{
    rrScrPrivPtr rp;
    int i;
    MirDisplayConfig *display_config = xmir_screen->display;
    int num_outputs;
    int output_type_count[mir_display_output_type_edp + 1] = {};

    if (!RRScreenInit(xmir_screen->screen))
        return FALSE;

    mir_connection_set_display_config_change_callback(xmir_screen->conn,
                                                      &xmir_display_config_callback,
                                                      xmir_screen);

    num_outputs = mir_display_config_get_num_outputs(display_config);
    for (i = 0; i < num_outputs; i++) {
        char name[32];
        int type_count;
        MirOutput const *mir_output;
        MirOutputType output_type;
        const char* output_type_str;
        struct xmir_output *xmir_output;

        mir_output = mir_display_config_get_output(display_config, i);
        output_type = mir_output_get_type(mir_output);
        output_type_str = mir_output_type_name(output_type);
        if (output_type_str)
            type_count = output_type_count[output_type]++;
        snprintf(name, sizeof name, "%s-%d", output_type_str, type_count);

        xmir_output = xmir_output_create(xmir_screen, name);
        if (!xmir_output)
            return FALSE;
        xmir_output_update(xmir_output, mir_output);
    }

    RRScreenSetSizeRange(xmir_screen->screen, 320, 200, INT16_MAX, INT16_MAX);

    xmir_output_screen_resized(xmir_screen);

    rp = rrGetScrPriv(xmir_screen->screen);
    rp->rrGetInfo = xmir_randr_get_info;
    rp->rrSetConfig = xmir_randr_set_config;
    // TODO: rp->rrCrtcSet = xmir_randr_set_crtc;

    return TRUE;
}
