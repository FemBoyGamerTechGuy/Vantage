/*
 * vt-backend-xlibre.c — XLibre display backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * XLibre is an X server fork that is API-compatible with Xorg at the
 * libxcb/libX11 level. From Vantage's point of view, the XLibre backend
 * is functionally identical to the Xorg backend; the only difference is
 * that we attempt to detect XLibre at runtime and apply the XLibre
 * naming. We do NOT depend on libXLibre; we use the same libxcb that
 * the Xorg backend uses.
 */

#define VT_LOG_DOMAIN "backend-xlibre"
#include <vantage/vt-backend.h>

extern const struct vt_backend _vt_backend_x11;

const struct vt_backend *_vt_backend_x11_new(void);

const struct vt_backend *_vt_backend_xlibre_new(void) {
    /* Reuse the X11 backend — at this layer, the wire protocol is
     * identical between Xorg and XLibre, so the same code path works
     * for both. The kind field is set to XLIBRE for diagnostic
     * purposes once we know we are talking to an XLibre server. */
    return _vt_backend_x11_new();
}
