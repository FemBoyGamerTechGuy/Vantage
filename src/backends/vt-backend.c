/*
 * vt-backend.c — Display backend factory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Selects and instantiates the appropriate backend based on user config
 * and what is available at runtime. The backend implementations live in
 * vt-backend-wayland.c, vt-backend-x11.c, vt-backend-xlibre.c.
 */

#define VT_LOG_DOMAIN "backend"
#include <vantage/vt-backend.h>
#include <string.h>
#include <stdlib.h>

extern const struct vt_backend *_vt_backend_wayland_new(void);
extern const struct vt_backend *_vt_backend_x11_new(void);
extern const struct vt_backend *_vt_backend_xlibre_new(void);

static const char *const _names[] = {
    [VT_BACKEND_AUTO]    = "auto",
    [VT_BACKEND_WAYLAND] = "wayland",
    [VT_BACKEND_XORG]    = "xorg",
    [VT_BACKEND_XLIBRE]  = "xlibre",
    [VT_BACKEND_HEADLESS]= "headless",
    [VT_BACKEND_INVALID] = "invalid",
};

vt_backend_kind_t vt_backend_kind_from_str(const char *s) {
    if (!s) return VT_BACKEND_AUTO;
    for (int i = 0; i < (int)VT_ARRAY_SIZE(_names); i++)
        if (vt_strcaseeq(_names[i], s)) return (vt_backend_kind_t)i;
    return VT_BACKEND_AUTO;
}
const char *vt_backend_kind_str(vt_backend_kind_t k) {
    return (k >= 0 && k < (int)VT_ARRAY_SIZE(_names)) ? _names[k] : "invalid";
}
const char *vt_backend_name(const vt_backend_t *b) {
    return b ? vt_backend_kind_str(b->kind) : "null";
}

vt_backend_t *vt_backend_new(vt_backend_kind_t preferred) {
    /* Probe order: user-specified first, then Wayland (native), then X11.
     * XLibre shares X11 code path. */
    const struct vt_backend *candidates[] = { NULL, NULL, NULL, };
    int n = 0;
    switch (preferred) {
    case VT_BACKEND_WAYLAND:
        candidates[n++] = _vt_backend_wayland_new();
        break;
    case VT_BACKEND_XORG:
        candidates[n++] = _vt_backend_x11_new();
        break;
    case VT_BACKEND_XLIBRE:
        candidates[n++] = _vt_backend_xlibre_new();
        break;
    default:
        candidates[n++] = _vt_backend_wayland_new();
        candidates[n++] = _vt_backend_x11_new();
        candidates[n++] = _vt_backend_xlibre_new();
        break;
    }
    for (int i = 0; i < n; i++) {
        const struct vt_backend *b = candidates[i];
        if (!b) continue;
        if (b->init && b->init((vt_backend_t *)b) == 0)
            return (vt_backend_t *)b;
        if (b->fini) b->fini((vt_backend_t *)b);
    }
    /* Fallback: headless — just so the session can still start. */
    vt_backend_t *hb = vt_malloc0(sizeof(*hb));
    hb->kind = VT_BACKEND_HEADLESS;
    return hb;
}

void vt_backend_free(vt_backend_t *b) {
    if (!b) return;
    if (b->fini) b->fini(b);
    vt_free(b);
}

int vt_backend_init(vt_backend_t *b) {
    return (b && b->init) ? b->init(b) : 0;
}
int vt_backend_dispatch(vt_backend_t *b, int timeout_ms) {
    return (b && b->dispatch) ? b->dispatch(b, timeout_ms) : 0;
}
int vt_backend_fd(vt_backend_t *b) {
    return (b && b->fd) ? b->fd(b) : -1;
}
size_t vt_backend_output_count(const vt_backend_t *b) {
    return (b && b->output_count) ? b->output_count((vt_backend_t *)b) : 0;
}
const vt_output_t *vt_backend_output_at(const vt_backend_t *b, size_t i) {
    return (b && b->output_at) ? b->output_at((vt_backend_t *)b, i) : NULL;
}
int vt_backend_output_apply(vt_backend_t *b, size_t i, const vt_output_t *cfg) {
    return (b && b->output_apply) ? b->output_apply(b, i, cfg) : 0;
}
