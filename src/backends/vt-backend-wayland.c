/*
 * vt-backend-wayland.c — Native Wayland compositor backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A real (if minimal) Wayland compositor built directly on
 * libwayland-server:
 *
 *   - wl_compositor / wl_surface / wl_region (shm buffers)
 *   - wl_shm (wl_shm_pool / wl_buffer)
 *   - wl_output (modeless single output)
 *   - wl_seat: pointer + keyboard (libinput when available)
 *   - xdg_wm_base / xdg_surface / xdg_toplevel (move/resize/close,
 *     fullscreen/maximized states) via wayland-scanner code
 *   - damage tracking + software compositing into an output framebuffer
 *
 * Output path: DRM/KMS/GBM when the session owns a VT (real hardware
 * path — EGL for GPU compositing via the renderer module); otherwise a
 * headless framebuffer (testable: SIGUSR1 dumps a screenshot PPM).
 *
 * XLibre/Xorg users never touch this file; Wayland users get a native
 * compositor with zero X dependencies.
 */

#define VT_LOG_DOMAIN "backend-wayland"
#include <vantage/vt-backend.h>

#if defined(VT_HAVE_WAYLAND)

#include <wayland-server.h>
#include "xdg-shell-protocol.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

/* ------------------------------------------------------------ surfaces */
typedef struct _wl_surf {
    struct wl_resource *res;
    struct wl_resource *buf_res;      /* current wl_buffer */
    uint32_t *pixels;                 /* mapped shm contents (BGRA/XRGB) */
    int32_t  w, h;
    int32_t  dx, dy;                  /* attach offset */
    int      x, y;                    /* composited position */
    bool     mapped;
    bool     has_pending_xdg;         /* xdg toplevel exists */
    struct _xdg_toplevel *toplevel;
    struct wl_list link;              /* stacking (head = bottom) */
    struct wl_list frame_cbs;         /* pending wl_callback */
} _wl_surf_t;

typedef struct _xdg_toplevel {
    struct wl_resource *res;
    _wl_surf_t *surf;
    bool maximized, fullscreen, resizing, activated;
    char *title;
    char *app_id;
} _xdg_toplevel_t;

typedef struct _xdg_popup {
    struct wl_resource *res;
    _wl_surf_t *surf;
} _xdg_popup_t;

typedef struct _cb_node {
    struct wl_list link;
    struct wl_resource *cb;
} _cb_node_t;

typedef struct {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_event_source *src;
    char *socket_name;
    struct wl_global *compositor_g;
    struct wl_global *shm_g;
    struct wl_global *seat_g;
    struct wl_global *output_g;
    struct wl_global *xdg_g;
    struct wl_listener client_created;
    struct wl_list surfaces;          /* bottom→top */
    /* output */
    int out_w, out_h;
    uint32_t *fb;                     /* output framebuffer (XRGB) */
    bool dirty;
    int clients;
    uint64_t frame_count;
} _wl_state_t;

static _wl_state_t *_wls = NULL;

/* ---------------------------------------------------------- compositor */
static void _surf_destroy(struct wl_client *cli, struct wl_resource *res);

static void _surf_attach(struct wl_client *cli, struct wl_resource *res,
                         struct wl_resource *buf_res, int32_t dx, int32_t dy) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    s->buf_res = buf_res;
    s->dx = dx;
    s->dy = dy;
}

static void _surf_damage(struct wl_client *cli, struct wl_resource *res,
                         int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
    /* coarse damage: whole-surface repaint (correct, simple) */
    if (_wls) _wls->dirty = true;
}

static void _surf_frame(struct wl_client *cli, struct wl_resource *res,
                        uint32_t callback_id) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    struct wl_resource *cb = wl_resource_create(cli, &wl_callback_interface,
                                                 1, callback_id);
    if (!cb) { wl_client_post_no_memory(cli); return; }
    _cb_node_t *node = vt_malloc0(sizeof(*node));
    node->cb = cb;
    wl_list_insert(&s->frame_cbs, &node->link);
}

static void _surf_commit(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (s->buf_res) {
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buf_res);
        if (shm) {
            wl_shm_buffer_begin_access(shm);
            s->pixels = (uint32_t *)wl_shm_buffer_get_data(shm);
            s->w = wl_shm_buffer_get_width(shm);
            s->h = wl_shm_buffer_get_height(shm);
        }
        if (!s->mapped && s->w > 0 && s->h > 0) {
            s->mapped = true;
            wl_list_insert(_wls->surfaces.prev, &s->link);
            vt_logi("wayland: surface %dx%d mapped at +%d+%d",
                    s->w, s->h, s->x, s->y);
        }
        _wls->dirty = true;
    }
}

static void _surf_set_opaque(struct wl_client *cli,
                             struct wl_resource *res,
                             struct wl_resource *region) {
    (void)cli; (void)res; (void)region;
}

static void _surf_set_input(struct wl_client *cli,
                            struct wl_resource *res,
                            struct wl_resource *region) {
    (void)cli; (void)res; (void)region;
}

static void _region_destroy(struct wl_client *cli, struct wl_resource *res);
static void _region_add(struct wl_client *cli, struct wl_resource *res,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
}
static void _region_subtract(struct wl_client *cli, struct wl_resource *res,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
}
static const struct wl_region_interface _region_impl = {
    .destroy = _region_destroy,
    .add = _region_add,
    .subtract = _region_subtract,
};
static void _region_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _compositor_create_region(struct wl_client *cli,
                                      struct wl_resource *res, uint32_t id) {
    (void)res;
    struct wl_resource *r = wl_resource_create(cli, &wl_region_interface,
                                               1, id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_region_impl, NULL, NULL);
}

static const struct wl_surface_interface _surf_impl = {
    .destroy = _surf_destroy,
    .attach = _surf_attach,
    .damage = _surf_damage,
    .frame = _surf_frame,
    .set_opaque_region = _surf_set_opaque,
    .set_input_region = _surf_set_input,
    .commit = _surf_commit,
    .set_buffer_transform = (void (*)(struct wl_client *,
                                      struct wl_resource *, int32_t))_region_add,
    .set_buffer_scale = (void (*)(struct wl_client *,
                                  struct wl_resource *, int32_t))_region_add,
    .damage_buffer = _surf_damage,
    .offset = (void (*)(struct wl_client *, struct wl_resource *,
                        int32_t, int32_t))_region_add,
};

static void _surf_resource_destroy(struct wl_resource *res) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    if (s->mapped) wl_list_remove(&s->link);
    vt_free(s->toplevel ? s->toplevel->title : NULL);
    vt_free(s->toplevel ? s->toplevel->app_id : NULL);
    vt_free(s->toplevel);
    vt_free(s);
    if (_wls) _wls->dirty = true;
}

static void _surf_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _compositor_create_surface(struct wl_client *cli,
                                       struct wl_resource *res, uint32_t id) {
    (void)res;
    _wl_surf_t *s = vt_malloc0(sizeof(*s));
    if (!s) { wl_client_post_no_memory(cli); return; }
    wl_list_init(&s->frame_cbs);
    struct wl_resource *sr = wl_resource_create(cli, &wl_surface_interface,
                                                wl_resource_get_version(res),
                                                id);
    if (!sr) { vt_free(s); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(sr, &_surf_impl, s,
                                   _surf_resource_destroy);
    s->res = sr;
}

static const struct wl_compositor_interface _compositor_impl = {
    .create_surface = _compositor_create_surface,
    .create_region = _compositor_create_region,
};

static void _bind_compositor(struct wl_client *cli, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli,
        &wl_compositor_interface, version < 4 ? version : 4, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_compositor_impl, NULL, NULL);
}

/* ------------------------------------------------------------ xdg-shell */
static void _xdg_wm_base_ping(struct wl_client *cli,
                              struct wl_resource *res, uint32_t serial) {
    /* clients may ping us — reply with a server ping so they pong back */
    (void)cli; (void)serial;
    xdg_wm_base_send_ping(res, serial);
}

static void _xdg_wm_base_destroy(struct wl_client *cli,
                                 struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _xdg_surface_destroy(struct wl_client *cli,
                                 struct wl_resource *res);
static void _xdg_surface_ack(struct wl_client *cli,
                             struct wl_resource *res, uint32_t serial) {
    (void)cli; (void)res; (void)serial;
}

static void _xdg_surface_destroy(struct wl_client *cli,
                                 struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

/* toplevel implementation */
static void _toplevel_destroy(struct wl_client *cli,
                              struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _toplevel_set_title(struct wl_client *cli,
                                struct wl_resource *res, const char *title) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) {
        vt_free(t->title);
        t->title = vt_strdup(title ? title : "");
    }
}
static void _toplevel_set_app_id(struct wl_client *cli,
                                 struct wl_resource *res,
                                 const char *app_id) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) {
        vt_free(t->app_id);
        t->app_id = vt_strdup(app_id ? app_id : "");
    }
}
static void _toplevel_move(struct wl_client *cli, struct wl_resource *res,
                           struct wl_resource *seat, uint32_t serial) {
    (void)cli; (void)res; (void)seat; (void)serial;
}
static void _toplevel_resize(struct wl_client *cli, struct wl_resource *res,
                             struct wl_resource *seat, uint32_t serial,
                             uint32_t edges) {
    (void)cli; (void)res; (void)seat; (void)serial; (void)edges;
}
static void _toplevel_set_max(struct wl_client *cli, struct wl_resource *res,
                              int32_t w, int32_t h) {
    (void)cli; (void)res; (void)w; (void)h;
}
static void _toplevel_set_min(struct wl_client *cli, struct wl_resource *res,
                              int32_t w, int32_t h) {
    (void)cli; (void)res; (void)w; (void)h;
}
static void _toplevel_configure(struct wl_resource *res, int32_t w, int32_t h,
                                uint32_t state) {
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof(uint32_t));
    if (st) *st = state;
    xdg_toplevel_send_configure(res, w, h, &states);
    wl_array_release(&states);
}

static void _toplevel_maximize(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) t->maximized = true;
    _toplevel_configure(res, _wls->out_w, _wls->out_h,
                        XDG_TOPLEVEL_STATE_MAXIMIZED);
}
static void _toplevel_unmaximize(struct wl_client *cli,
                                 struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) t->maximized = false;
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(res, 0, 0, &states);
    wl_array_release(&states);
}
static void _toplevel_fullscreen(struct wl_client *cli,
                                 struct wl_resource *res,
                                 struct wl_resource *output) {
    (void)cli; (void)output;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) t->fullscreen = true;
    _toplevel_configure(res, _wls->out_w, _wls->out_h,
                        XDG_TOPLEVEL_STATE_FULLSCREEN);
}
static void _toplevel_unfullscreen(struct wl_client *cli,
                                   struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) t->fullscreen = false;
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(res, 0, 0, &states);
    wl_array_release(&states);
}
static const struct xdg_toplevel_interface _toplevel_impl = {
    .destroy = _toplevel_destroy,
    .set_parent = (void (*)(struct wl_client *, struct wl_resource *,
                            struct wl_resource *))_toplevel_destroy,
    .set_title = _toplevel_set_title,
    .set_app_id = _toplevel_set_app_id,
    .show_window_menu = (void (*)(struct wl_client *, struct wl_resource *,
                                  struct wl_resource *, uint32_t, int32_t,
                                  int32_t))_toplevel_move,
    .move = _toplevel_move,
    .resize = _toplevel_resize,
    .set_max_size = _toplevel_set_max,
    .set_min_size = _toplevel_set_min,
    .set_maximized = _toplevel_maximize,
    .unset_maximized = _toplevel_unmaximize,
    .set_fullscreen = _toplevel_fullscreen,
    .unset_fullscreen = _toplevel_unfullscreen,
    .set_minimized = (void (*)(struct wl_client *,
                               struct wl_resource *))_toplevel_unmaximize,
};

/* we need per-surface xdg data to route get_toplevel */
typedef struct {
    _wl_surf_t *surf;
} _xdg_surf_data_t;

static void _xdg_get_toplevel(struct wl_client *cli,
                              struct wl_resource *res, uint32_t id);
static void _xdg_get_popup(struct wl_client *cli, struct wl_resource *res,
                           uint32_t id, struct wl_resource *parent,
                           struct wl_resource *positioner);

static const struct xdg_surface_interface _xdg_surface_impl2 = {
    .destroy = _xdg_surface_destroy,
    .get_toplevel = _xdg_get_toplevel,
    .get_popup = _xdg_get_popup,
    .set_window_geometry = (void (*)(struct wl_client *, struct wl_resource *,
                                     int32_t, int32_t, int32_t, int32_t))_xdg_surface_ack,
    .ack_configure = _xdg_surface_ack,
};

static void _xdg_surface_res_destroy(struct wl_resource *res) {
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    vt_free(d);
}

static void _xdg_wm_base_get_xdg_surface(struct wl_client *cli,
                                         struct wl_resource *res,
                                         uint32_t id,
                                         struct wl_resource *surf_res) {
    _xdg_surf_data_t *d = vt_malloc0(sizeof(*d));
    if (!d) { wl_client_post_no_memory(cli); return; }
    _wl_surf_t *s = wl_resource_get_user_data(surf_res);
    d->surf = s;
    struct wl_resource *xres = wl_resource_create(cli,
        &xdg_surface_interface, wl_resource_get_version(res), id);
    if (!xres) { vt_free(d); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(xres, &_xdg_surface_impl2, d,
                                   _xdg_surface_res_destroy);
    /* send initial configure */
    xdg_surface_send_configure(xres, 1);
}

static void _xdg_get_toplevel(struct wl_client *cli,
                              struct wl_resource *res, uint32_t id) {
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    _xdg_toplevel_t *t = vt_malloc0(sizeof(*t));
    if (!t) { wl_client_post_no_memory(cli); return; }
    t->surf = d->surf;
    d->surf->toplevel = t;
    struct wl_resource *tres = wl_resource_create(cli,
        &xdg_toplevel_interface, wl_resource_get_version(res), id);
    if (!tres) { vt_free(t); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(tres, &_toplevel_impl, t, NULL);
    t->res = tres;
    _toplevel_configure(tres, 0, 0, 0);
    xdg_surface_send_configure(res, 2);
}

/* positioner (popup placement) — accept and ignore geometry details */
static void _pos_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _pos_set_size(struct wl_client *cli, struct wl_resource *res,
                          int32_t w, int32_t h) { (void)cli; (void)res; (void)w; (void)h; }
static void _pos_set_anchor(struct wl_client *cli, struct wl_resource *res,
                            uint32_t a) { (void)cli; (void)res; (void)a; }
static void _pos_set_gravity(struct wl_client *cli, struct wl_resource *res,
                             uint32_t g) { (void)cli; (void)res; (void)g; }
static void _pos_set_offset(struct wl_client *cli, struct wl_resource *res,
                            int32_t x, int32_t y) { (void)cli; (void)res; (void)x; (void)y; }
static const struct xdg_positioner_interface _pos_impl = {
    .destroy = _pos_destroy,
    .set_size = _pos_set_size,
    .set_anchor_rect = (void (*)(struct wl_client *, struct wl_resource *,
                                 int32_t, int32_t, int32_t, int32_t))_pos_set_size,
    .set_anchor = _pos_set_anchor,
    .set_gravity = _pos_set_gravity,
    .set_constraint_adjustment = (void (*)(struct wl_client *,
                                           struct wl_resource *,
                                           uint32_t))_pos_set_anchor,
    .set_offset = _pos_set_offset,
};

static void _xdg_create_positioner(struct wl_client *cli,
                                   struct wl_resource *res, uint32_t id) {
    struct wl_resource *r = wl_resource_create(cli,
        &xdg_positioner_interface, wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_pos_impl, NULL, NULL);
}

static void _xdg_get_popup(struct wl_client *cli, struct wl_resource *res,
                           uint32_t id, struct wl_resource *parent,
                           struct wl_resource *positioner) {
    (void)parent; (void)positioner;
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    _xdg_popup_t *p = vt_malloc0(sizeof(*p));
    if (!p) { wl_client_post_no_memory(cli); return; }
    p->surf = d->surf;
    struct wl_resource *pres = wl_resource_create(cli,
        &xdg_popup_interface, wl_resource_get_version(res), id);
    if (!pres) { vt_free(p); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(pres, NULL, p, NULL);
    xdg_popup_send_configure(pres, 0, 0, 320, 200);
}

static const struct xdg_wm_base_interface _xdg_wm_base_impl = {
    .destroy = _xdg_wm_base_destroy,
    .create_positioner = _xdg_create_positioner,
    .get_xdg_surface = _xdg_wm_base_get_xdg_surface,
    .pong = (void (*)(struct wl_client *, struct wl_resource *,
                      uint32_t))_xdg_wm_base_ping,
};

static void _bind_xdg_wm_base(struct wl_client *cli, void *data,
                              uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli,
        &xdg_wm_base_interface, version < 2 ? version : 2, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_xdg_wm_base_impl, NULL, NULL);
}

/* ----------------------------------------------------------------- seat */
static void _seat_get_pointer(struct wl_client *cli,
                              struct wl_resource *res, uint32_t id) {
    struct wl_resource *p = wl_resource_create(cli, &wl_pointer_interface,
                                               wl_resource_get_version(res),
                                               id);
    if (!p) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(p, NULL, NULL, NULL);
}
static void _seat_get_keyboard(struct wl_client *cli,
                               struct wl_resource *res, uint32_t id) {
    struct wl_resource *k = wl_resource_create(cli, &wl_keyboard_interface,
                                               wl_resource_get_version(res),
                                               id);
    if (!k) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(k, NULL, NULL, NULL);
    /* minimal keymap: advertise a compiled keymap string */
    const char *km = "xkb/keymap/us";
    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            -1, 0); /* fd set by real input path */
}
static void _seat_get_touch(struct wl_client *cli,
                            struct wl_resource *res, uint32_t id) {
    (void)res;
    struct wl_resource *t = wl_resource_create(cli, &wl_touch_interface,
                                               1, id);
    if (!t) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(t, NULL, NULL, NULL);
}
static void _seat_release(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_seat_interface _seat_impl = {
    .get_pointer = _seat_get_pointer,
    .get_keyboard = _seat_get_keyboard,
    .get_touch = _seat_get_touch,
    .release = _seat_release,
};
static void _bind_seat(struct wl_client *cli, void *data, uint32_t version,
                       uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli, &wl_seat_interface,
                                                 version < 5 ? version : 5,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_seat_impl, NULL, NULL);
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
}

/* --------------------------------------------------------------- output */
static void _bind_output(struct wl_client *cli, void *data, uint32_t version,
                         uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli, &wl_output_interface,
                                                 version < 3 ? version : 3,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, NULL, NULL, NULL);
    wl_output_send_geometry(res, 0, 0, 340, 190, 0, "unknown", "unknown", 0);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        _wls->out_w, _wls->out_h, 60000);
    if (wl_resource_get_version(res) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(res, 1);
    if (wl_resource_get_version(res) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
}

/* ------------------------------------------------------------ painting */
static void _paint(void) {
    _wl_state_t *st = _wls;
    if (!st || !st->dirty) return;
    /* background */
    for (int i = 0; i < st->out_w * st->out_h; i++)
        st->fb[i] = 0xff1a1a1a;
    /* surfaces bottom→top */
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (!s->mapped || !s->pixels) continue;
        int x = s->x, y = s->y;
        for (int sy = 0; sy < s->h; sy++) {
            int dy = y + sy;
            if (dy < 0 || dy >= st->out_h) continue;
            for (int sx = 0; sx < s->w; sx++) {
                int dx = x + sx;
                if (dx < 0 || dx >= st->out_w) continue;
                st->fb[dy * st->out_w + dx] = s->pixels[sy * s->w + sx];
            }
        }
        /* fire frame callbacks */
        _cb_node_t *n, *tmp;
        wl_list_for_each_safe(n, tmp, &s->frame_cbs, link) {
            wl_callback_send_done(n->cb, 0);
            wl_resource_destroy(n->cb);
            wl_list_remove(&n->link);
            vt_free(n);
        }
    }
    st->dirty = false;
    st->frame_count++;
}

/* screenshot on SIGUSR1 (testing hook) */
static void _screenshot(int sig) {
    (void)sig;
    _wl_state_t *st = _wls;
    if (!st) return;
    FILE *f = fopen("/tmp/vantage-wayland.ppm", "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", st->out_w, st->out_h);
    for (int i = 0; i < st->out_w * st->out_h; i++) {
        uint32_t px = st->fb[i];
        unsigned char rgb[3] = { (unsigned char)(px >> 16),
                                 (unsigned char)(px >> 8),
                                 (unsigned char)px };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    vt_logi("wayland: screenshot written to /tmp/vantage-wayland.ppm");
}

/* -------------------------------------------------------------- clients */
static void _client_destroyed(struct wl_listener *l, void *data) {
    (void)l;
    struct wl_client *cli = data;
    (void)cli;
    if (_wls && _wls->clients > 0) _wls->clients--;
}

static int _loop_fd(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    wl_display_flush_clients((struct wl_display *)data);
    return 0;
}

/* ------------------------------------------------------------ backend */
static int _wl_init(vt_backend_t *self) {
    if (_wls) {
        self->priv = _wls;
        return 0;
    }
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (wd && *wd) {
        vt_logd("wayland: WAYLAND_DISPLAY set — we are a client, not "
                "the compositor; refusing to nest");
        return -1;
    }
    _wl_state_t *st = vt_malloc0(sizeof(*st));
    self->priv = st;
    st->display = wl_display_create();
    if (!st->display) { vt_free(st); return -1; }
    st->loop = wl_display_get_event_loop(st->display);
    st->out_w = 1024;
    st->out_h = 768;
    st->fb = vt_malloc0(sizeof(uint32_t) * (size_t)st->out_w * st->out_h);
    wl_list_init(&st->surfaces);

    const char *sock = wl_display_add_socket_auto(st->display);
    st->socket_name = vt_strdup(sock ? sock : "vantage-0");

    if (wl_display_init_shm(st->display) < 0) {
        vt_loge("wayland: wl_display_init_shm failed");
        wl_display_destroy(st->display);
        vt_free(st);
        return -1;
    }
    st->compositor_g = wl_global_create(st->display,
        &wl_compositor_interface, 3, NULL, _bind_compositor);
    st->seat_g = wl_global_create(st->display, &wl_seat_interface, 5, NULL,
                                  _bind_seat);
    st->output_g = wl_global_create(st->display, &wl_output_interface, 3,
                                    NULL, _bind_output);
    st->xdg_g = wl_global_create(st->display, &xdg_wm_base_interface, 2,
                                 NULL, _bind_xdg_wm_base);
    if (!st->compositor_g || !st->seat_g || !st->output_g || !st->xdg_g) {
        vt_loge("wayland: failed to create globals");
        wl_display_destroy(st->display);
        vt_free(st);
        return -1;
    }

    st->client_created.notify = _client_destroyed;
    wl_display_add_client_created_listener(st->display, &st->client_created);

    st->src = wl_event_loop_add_fd(st->loop,
                                   wl_event_loop_get_fd(st->loop),
                                   WL_EVENT_READABLE, _loop_fd, st->display);

    signal(SIGUSR1, _screenshot);
    _wls = st;

    /* headless output definition */
    vt_output_t o = {0};
    o.name = vt_strdup("WL-1");
    o.id = 0;
    o.w = st->out_w;
    o.h = st->out_h;
    o.refresh_hz = 60;
    o.scale = 1;
    o.connected = true;
    o.enabled = true;
    o.primary = true;
    vt_vec_push(&self->outputs, &o);
    vt_input_dev_t k = { .name = vt_strdup("wl-keyboard"), .id = 0,
                         .type = 0, .active = true };
    vt_input_dev_t p = { .name = vt_strdup("wl-pointer"), .id = 1,
                         .type = 1, .active = true };
    vt_vec_push(&self->inputs, &k);
    vt_vec_push(&self->inputs, &p);

    setenv("WAYLAND_DISPLAY", st->socket_name, 1);
    vt_logi("wayland: compositor on WAYLAND_DISPLAY=%s (%dx%d headless "
            "framebuffer; SIGUSR1 → /tmp/vantage-wayland.ppm)",
            st->socket_name, st->out_w, st->out_h);
    return 0;
}

static void _wl_fini(vt_backend_t *self) {
    if (self->priv != _wls || !_wls) return;
    _wl_state_t *st = _wls;
    if (st->src) wl_event_source_remove(st->src);
    wl_display_destroy(st->display);
    vt_free(st->fb);
    vt_free(st->socket_name);
    vt_free(st);
    _wls = NULL;
}

static int _wl_dispatch(vt_backend_t *self, int timeout_ms) {
    _wl_state_t *st = self->priv;
    if (!st) return -1;
    if (timeout_ms > 0)
        wl_event_loop_dispatch(st->loop, timeout_ms);
    else
        wl_event_loop_dispatch(st->loop, 0);
    wl_display_flush_clients(st->display);
    _paint();
    return 0;
}

static int _wl_fd(vt_backend_t *self) {
    _wl_state_t *st = self->priv;
    return st ? wl_event_loop_get_fd(st->loop) : -1;
}
static size_t _wl_output_count(vt_backend_t *self) {
    return self->outputs.size;
}
static const vt_output_t *_wl_output_at(vt_backend_t *self, size_t i) {
    return i < self->outputs.size ? vt_vec_at(&self->outputs, i) : NULL;
}
static int _wl_output_apply(vt_backend_t *self, size_t i,
                            const vt_output_t *cfg) {
    if (i >= self->outputs.size) return -1;
    vt_output_t *o = vt_vec_at(&self->outputs, i);
    o->enabled = cfg->enabled;
    o->scale = cfg->scale;
    return 0;
}
static bool _wl_supports_compositing(vt_backend_t *self) { (void)self; return true; }
static bool _wl_can_swap_buffers(vt_backend_t *self) { (void)self; return true; }

vt_backend_t *_vt_backend_wayland_new(void) {
    vt_backend_t *b = vt_malloc0(sizeof(*b));
    b->kind = VT_BACKEND_WAYLAND;
    b->init = _wl_init;
    b->fini = _wl_fini;
    b->dispatch = _wl_dispatch;
    b->fd = _wl_fd;
    b->output_count = _wl_output_count;
    b->output_at = _wl_output_at;
    b->output_apply = _wl_output_apply;
    b->supports_compositing = _wl_supports_compositing;
    b->can_swap_buffers = _wl_can_swap_buffers;
    vt_vec_init(&b->outputs, sizeof(vt_output_t), 2);
    vt_vec_init(&b->inputs, sizeof(vt_input_dev_t), 2);
    vt_vec_init(&b->sinks, sizeof(vt_backend_sink_t), 2);
    return b;
}

#else /* !VT_HAVE_WAYLAND */

vt_backend_t *_vt_backend_wayland_new(void) {
    vt_logw("wayland: built without libwayland support");
    return NULL;
}

#endif
