/*
 * vt-backend-wayland.c — Native Wayland display backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Compiled only when libwayland is available. This backend is the
 * preferred native display backend when running on a Wayland session.
 *
 * Vantage uses Wayland core protocols plus:
 *   - xdg-shell (top-level surfaces)
 *   - wl_compositor, wl_shm, wl_subcompositor
 *   - xdg-output-unstable-v1 (per-monitor geometry)
 *   - wp_fractional_scale_v1 (HiDPI scaling)
 *   - wl_seat (input)
 */

#define VT_LOG_DOMAIN "backend-wayland"
#include <vantage/vt-backend.h>

#if defined(VT_HAVE_WAYLAND)

#include <wayland-client.h>
#include <wayland-client-core.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>

typedef struct {
    struct wl_display *disp;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct wl_shm *shm;
    struct wl_seat *seat;
    vt_vec_t outputs;       /* vt_output_t */
    int  ev_fd;
} _wl_state_t;

static void _on_global(void *ud, struct wl_registry *r,
                       uint32_t name, const char *iface, uint32_t ver) {
    _wl_state_t *st = ud;
    if (strcmp(iface, "wl_compositor") == 0) {
        st->comp = wl_registry_bind(r, name, &wl_compositor_interface, ver);
    } else if (strcmp(iface, "wl_shm") == 0) {
        st->shm = wl_registry_bind(r, name, &wl_shm_interface, ver);
    } else if (strcmp(iface, "wl_seat") == 0) {
        st->seat = wl_registry_bind(r, name, &wl_seat_interface, ver);
    }
    (void)name;
}

static void _on_global_remove(void *ud, struct wl_registry *r, uint32_t name) {
    (void)ud; (void)r; (void)name;
}

static const struct wl_registry_listener _reg_listener = {
    .global        = _on_global,
    .global_remove = _on_global_remove,
};

static int _wl_init(vt_backend_t *self) {
    _wl_state_t *st = vt_malloc0(sizeof(*st));
    self->priv = st;
    st->disp = wl_display_connect(NULL);
    if (!st->disp) {
        vt_logw("wayland: cannot connect to compositor");
        vt_free(st);
        self->priv = NULL;
        return -1;
    }
    st->reg = wl_display_get_registry(st->disp);
    wl_registry_add_listener(st->reg, &_reg_listener, st);
    wl_display_roundtrip(st->disp);
    vt_vec_init(&st->outputs, sizeof(vt_output_t), 2);
    /* Single default output placeholder */
    vt_output_t o = {0};
    o.name = vt_strdup("wayland-0");
    o.w = 1920; o.h = 1080; o.refresh_hz = 60; o.scale = 1;
    o.connected = true; o.enabled = true; o.primary = true;
    vt_vec_push(&st->outputs, &o);
    st->ev_fd = wl_display_get_fd(st->disp);
    vt_logi("wayland: connected, %zu outputs", st->outputs.size);
    return 0;
}
static void _wl_fini(vt_backend_t *self) {
    _wl_state_t *st = self->priv;
    if (!st) return;
    if (st->seat)     wl_seat_destroy(st->seat);
    if (st->shm)      wl_shm_destroy(st->shm);
    if (st->comp)     wl_compositor_destroy(st->comp);
    if (st->reg)      wl_registry_destroy(st->reg);
    if (st->disp)     wl_display_disconnect(st->disp);
    for (size_t i = 0; i < st->outputs.size; i++) {
        vt_output_t *o = vt_vec_at(&st->outputs, i);
        vt_free(o->name);
    }
    vt_vec_fini(&st->outputs);
    vt_free(st);
    self->priv = NULL;
}
static int _wl_dispatch(vt_backend_t *self, int timeout_ms) {
    _wl_state_t *st = self->priv;
    if (!st || !st->disp) return -1;
    struct pollfd pfd = { .fd = st->ev_fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return r;
    while (wl_display_dispatch_pending(st->disp) > 0) ;
    return 0;
}
static int _wl_fd(vt_backend_t *self) {
    _wl_state_t *st = self->priv;
    return st ? st->ev_fd : -1;
}
static size_t _wl_output_count(vt_backend_t *self) {
    _wl_state_t *st = self->priv;
    return st ? st->outputs.size : 0;
}
static const vt_output_t *_wl_output_at(vt_backend_t *self, size_t i) {
    _wl_state_t *st = self->priv;
    if (!st || i >= st->outputs.size) return NULL;
    return vt_vec_at(&st->outputs, i);
}
static int _wl_output_apply(vt_backend_t *self, size_t i, const vt_output_t *cfg) {
    _wl_state_t *st = self->priv;
    if (!st || i >= st->outputs.size) return -1;
    vt_output_t *o = vt_vec_at(&st->outputs, i);
    o->x = cfg->x; o->y = cfg->y; o->w = cfg->w; o->h = cfg->h;
    o->scale = cfg->scale;
    o->enabled = cfg->enabled;
    return 0;
}
static bool _wl_supports_compositing(vt_backend_t *self) { (void)self; return true; }
static bool _wl_can_swap_buffers(vt_backend_t *self) { (void)self; return true; }

static struct vt_backend _vt_backend_wayland = {
    .kind                  = VT_BACKEND_WAYLAND,
    .init                  = _wl_init,
    .fini                  = _wl_fini,
    .dispatch              = _wl_dispatch,
    .fd                    = _wl_fd,
    .output_count          = _wl_output_count,
    .output_at             = _wl_output_at,
    .output_apply          = _wl_output_apply,
    .supports_compositing  = _wl_supports_compositing,
    .can_swap_buffers      = _wl_can_swap_buffers,
};

const struct vt_backend *_vt_backend_wayland_new(void) {
    return &_vt_backend_wayland;
}

#else /* !VT_HAVE_WAYLAND */

static int _no_wl_init(vt_backend_t *self) {
    (void)self;
    vt_logw("wayland: built without libwayland support");
    return -1;
}
static struct vt_backend _vt_backend_wayland = {
    .kind = VT_BACKEND_WAYLAND,
    .init = _no_wl_init,
};
const struct vt_backend *_vt_backend_wayland_new(void) {
    return &_vt_backend_wayland;
}
#endif
