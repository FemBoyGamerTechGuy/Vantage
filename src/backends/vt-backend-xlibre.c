/*
 * vt-backend-xlibre.c — XLibre display backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * XLibre is an X server fork that is API-compatible with Xorg at the
 * libX11 wire-protocol level. From Vantage's point of view the XLibre
 * backend is functionally identical to the Xorg backend: the same code
 * path is used, with the backend kind set to XLIBRE when the server
 * identifies itself as XLibre (via the server vendor string), which is
 * purely informational for diagnostics.
 */

#define VT_LOG_DOMAIN "backend-xlibre"
#include <vantage/vt-backend.h>
#include <vantage/vt-x11.h>
#include <string.h>

vt_backend_t *_vt_backend_x11_new(void);

vt_backend_t *_vt_backend_xlibre_new(void) {
    vt_backend_t *b = _vt_backend_x11_new();
    if (!b) return NULL;
#if defined(VT_HAVE_X11)
    /* After a successful init the backend can inspect the server vendor.
     * vt-backend-x11.c leaves kind as XORG; refine it here. */
    Display *dpy = vt_x11_display();
    if (dpy) {
        const char *vendor = XServerVendor(dpy);
        if (vendor && (strstr(vendor, "XLibre") || strstr(vendor, "Xlibre"))) {
            b->kind = VT_BACKEND_XLIBRE;
            vt_logi("xlibre: server identified as XLibre (%s)", vendor);
        }
    }
#endif
    return b;
}
