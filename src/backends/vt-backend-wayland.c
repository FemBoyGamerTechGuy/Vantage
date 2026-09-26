/*
 * vt-backend-wayland.c — Native Wayland compositor backend
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * A real Wayland compositor built directly on libwayland-server:
 *
 *   - wl_compositor / wl_surface / wl_region (shm buffers)
 *   - wl_shm (wl_shm_pool / wl_buffer)
 *   - wl_output (real KMS outputs when present)
 *   - wl_seat: pointer + keyboard via libinput (udev) + xkbcommon
 *   - xdg_wm_base / xdg_surface / xdg_toplevel (move/resize/close,
 *     fullscreen/maximized states) via wayland-scanner code
 *   - damage tracking + software compositing into the scanout buffer
 *
 * Startup is a 15-stage pipeline, each stage bracketed by [wayland]
 * log markers, so a real TTY run pinpoints exactly where setup stops:
 *
 *   session → seat → vt → drm → drm-master → gbm → egl → renderer →
 *   outputs → crtc → scanout → input → socket → compositor → desktop
 *
 * Output path: seat (libseat: logind/elogind/seatd, else direct VT
 * ioctls) + DRM/KMS/GBM scanout with async page flips on real
 * hardware; an honest HEADLESS framebuffer fallback when no KMS output
 * can be acquired (VANTAGE_WAYLAND_REQUIRE_KMS=1 turns that fallback
 * into a hard failure). VANTAGE_WAYLAND_FORCE_HEADLESS=1 skips the
 * seat/vt/drm stages outright — deterministic tests/CI that never
 * touch the host's real session, VT or GPU.
 *
 * XLibre/Xorg users never touch this file; Wayland users get a native
 * compositor with zero X dependencies.
 */

/* _GNU_SOURCE (memfd_create) is provided by the build (meson) */
#define VT_LOG_DOMAIN "backend-wayland"
#include <vantage/vt-backend.h>
#include <vantage/vt-seat.h>
#include <vantage/vt-kms.h>
#include <vantage/vt-ipc.h>

#if defined(VT_HAVE_WAYLAND)

#include <wayland-server.h>
#include "xdg-shell-protocol.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#if defined(VT_HAVE_XCURSOR)
#include <X11/Xcursor/Xcursor.h>
#endif

#if defined(VT_HAVE_LIBINPUT)
#include <libinput.h>
#include <libudev.h>
#endif

#if defined(VT_HAVE_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

#include "vt-wl-panel.h"

/* ------------------------------------------------------------ logging */

/* The 15 startup stages, in order. */
static const char *const _stages[] = {
    "session", "seat", "vt", "drm", "drm-master", "gbm", "egl", "renderer",
    "outputs", "crtc", "scanout", "input", "socket", "compositor", "desktop",
};
#define _N_STAGES ((int)(sizeof(_stages) / sizeof(_stages[0])))

static void _stage_begin(int n, const char *detail) {
    vt_logi("[wayland] %s: starting%s%s",
            _stages[n], detail ? " — " : "", detail ? detail : "");
}
static void _stage_ok(int n, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vt_logi("[wayland] %s: ok — %s", _stages[n], buf);
}
static void _stage_skip(int n, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vt_logi("[wayland] %s: skipped — %s", _stages[n], buf);
}
static void _stage_fail(int n, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vt_loge("[wayland] %s: FAILED — %s", _stages[n], buf);
}

/* Truthy env flag: 1/true/yes/on (case-insensitive), else false. */
static bool _env_flag(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return false;
    return vt_strcaseeq(v, "1") || vt_strcaseeq(v, "true") ||
           vt_strcaseeq(v, "yes") || vt_strcaseeq(v, "on");
}

/* ------------------------------------------------------------ surfaces */
typedef struct _wl_surf {
    struct wl_resource *res;
    struct wl_resource *buf_res;      /* current wl_buffer */
    uint32_t *pixels;                 /* mapped shm contents (BGRA/XRGB) */
    int32_t  w, h;
    int32_t  stride;                  /* row stride in uint32 units */
    int32_t  dx, dy;                  /* attach offset */
    int      x, y;                    /* composited position */
    int      ws;                      /* workspace (all if sticky-ish) */
    bool     mapped;
    bool     has_pending_xdg;         /* xdg toplevel exists */
    struct _xdg_toplevel *toplevel;
    struct wl_list link;              /* stacking (head = bottom) */
    struct wl_list frame_cbs;         /* pending wl_callback */
    /* cursor-surface duties (wl_pointer.set_cursor) */
    bool     is_cursor;
    int      hotspot_x, hotspot_y;
    /* wl_subsurface duties: children are painted relative to this
     * surface, in their own stacking order (place_above/below) */
    struct _wl_surf *parent;
    struct wl_list  subs;             /* child subsurfaces */
    struct wl_list  sub_link;
} _wl_surf_t;

typedef struct _xdg_toplevel {
    struct wl_resource *res;
    _wl_surf_t *surf;
    uint64_t    id;                   /* stable window id for the WM */
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

/* per-client pointer/keyboard resources */
typedef struct _ptr_res {
    struct wl_list link;
    struct wl_resource *res;          /* wl_pointer resource */
} _ptr_res_t;

typedef struct _kbd_res {
    struct wl_list link;
    struct wl_resource *res;          /* wl_keyboard resource */
} _kbd_res_t;

typedef struct {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_event_source *src;
    struct wl_event_source *seat_src;   /* seat fd source */
    struct wl_event_source *drm_src;   /* drm fd source */
    struct wl_event_source *li_src;    /* libinput fd source */
    char *socket_name;
    struct wl_global *compositor_g;
    struct wl_global *shm_g;
    struct wl_global *seat_g;
    struct wl_global *output_g;
    struct wl_global *xdg_g;
    struct wl_global *subcomp_g;
    struct wl_global *ddm_g;
    struct wl_listener client_created;
    struct wl_list surfaces;          /* bottom→top */
    vt_backend_t *backend_self;       /* for event emission */
    void *user_data;                  /* wm host pointer */
    /* output */
    int out_w, out_h;
    uint32_t *fb;                     /* output framebuffer (XRGB) */
    bool dirty;
    int clients;
    uint64_t frame_count;
    bool headless;                    /* honest marker: no KMS */

    /* compositor-side panel + workspace state */
    vt_wl_panel_t *panel;
    int ws_count, ws_cur;
    time_t panel_clock_sync;

    /* real session path */
    vt_seat_t *seat;
    vt_kms_t  *kms;
    struct wl_event_source *vt_switch_src;

    /* input */
#if defined(VT_HAVE_LIBINPUT)
    struct libinput *li;
    struct udev *udev;
#endif
    int cursor_x, cursor_y;
    bool buttons[16];                  /* pressed buttons (0-indexed) */
    uint32_t serial;                   /* wayland serial counter */
    /* keyboard */
#if defined(VT_HAVE_XKBCOMMON)
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_km;
    struct xkb_state   *xkb_st;
    int  keymap_fd;
    size_t keymap_size;
#endif
    struct wl_list ptr_reses;          /* _ptr_res_t */
    struct wl_list kbd_reses;          /* _kbd_res_t */
    _wl_surf_t *ptr_focus;             /* surface under cursor */
    _wl_surf_t *kbd_focus;             /* focused surface */
    _xdg_toplevel_t *focused_toplevel;
    uint64_t next_win_id;

    /* clipboard */
    struct wl_list data_devs;        /* _data_dev_t */
    struct _data_src *selection;
    /* cursor sprite */
    uint32_t cursor_img[64 * 64];
    int cur_img_w, cur_img_h, cur_img_hx, cur_img_hy;
    bool cur_client_set;               /* client provided a cursor */
    _wl_surf_t *cursor_surf;

    /* interactive move/resize (xdg toplevel requests) */
    bool op_active;                    /* interactive op in progress */
    bool op_resize;
    _wl_surf_t *op_surf;
    int op_grab_x, op_grab_y;
    int op_start_w, op_start_h;
} _wl_state_t;

static _wl_state_t *_wls = NULL;

/* used by the WM host (vantage-wm) to route compositor hotkeys */
static bool _hotkey_try(vt_backend_t *self, const char *combo) {
    if (self && self->hotkey) return self->hotkey(self, combo);
    return false;
}

/* ------------------------------------------------- window event emission */
static void _emit_win(_wl_state_t *st, vt_backend_wl_event_kind_t kind,
                      _xdg_toplevel_t *t) {
    if (!st || !t) return;
    vt_backend_wl_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.window_id = t->id;
    ev.title = t->title;
    ev.app_id = t->app_id;
    _wl_surf_t *s = t->surf;
    if (s) {
        ev.x = s->x; ev.y = s->y;
        ev.w = s->w; ev.h = s->h;
    }
    ev.focused = t->activated;
    ev.maximized = t->maximized;
    ev.fullscreen = t->fullscreen;
    vt_backend_t *b = NULL;
    /* emit through the backend's sink list: find backend from st */
    if (_wls && _wls->backend_self) b = _wls->backend_self;
    if (b) vt_backend_emit_event(b, &ev);
}

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
            /* the client's row stride is NOT width*4 in general —
             * toolkits pad rows. Reading with w as the stride shears
             * the whole window into a diagonal smear. */
            int32_t bytes = wl_shm_buffer_get_stride(shm);
            s->stride = bytes / 4 >= s->w ? bytes / 4 : s->w;
        }
        if (!s->mapped && s->w > 0 && s->h > 0) {
            s->mapped = true;
            wl_list_insert(_wls->surfaces.prev, &s->link);
            /* center the first frame */
            if (s->x == 0 && s->y == 0) {
                s->x = (_wls->out_w - s->w) / 2;
                s->y = (_wls->out_h - s->h) / 2;
                if (s->x < 0) s->x = 0;
                if (s->y < 0) s->y = 0;
            }
            vt_logi("wayland: window 0x%llx '%s' mapped %dx%d at +%d+%d",
                    (unsigned long long)(s->toplevel ? s->toplevel->id : 0),
                    s->toplevel && s->toplevel->title ?
                        s->toplevel->title : "(untitled)",
                    s->w, s->h, s->x, s->y);
            if (s->toplevel) {
                s->toplevel->activated = true;
                _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_MAP, s->toplevel);
            }
            /* new window takes keyboard focus */
            _wls->kbd_focus = s;
            _wls->focused_toplevel = s->toplevel;
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
static void _surf_set_buffer_transform(struct wl_client *cli,
                                        struct wl_resource *res,
                                        int32_t transform) {
    (void)cli; (void)res; (void)transform;
}
static void _surf_set_buffer_scale(struct wl_client *cli,
                                    struct wl_resource *res,
                                    int32_t scale) {
    (void)cli; (void)res; (void)scale;
}
static void _surf_offset(struct wl_client *cli, struct wl_resource *res,
                         int32_t x, int32_t y) {
    (void)cli; (void)res; (void)x; (void)y;
}

static void _region_destroy(struct wl_client *cli, struct wl_resource *res);
static void _region_add(struct wl_client *cli, struct wl_resource *res,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
}
static void _region_subtract(struct wl_client *cli,
                             struct wl_resource *res,
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
    .set_buffer_transform = _surf_set_buffer_transform,
    .set_buffer_scale = _surf_set_buffer_scale,
    .damage_buffer = _surf_damage,
    .offset = _surf_offset,
};

static void _surf_resource_destroy(struct wl_resource *res) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    if (s->parent) {
        wl_list_remove(&s->sub_link);
        s->parent = NULL;
    }
    if (s->mapped) {
        wl_list_remove(&s->link);
        if (_wls) {
            if (_wls->ptr_focus == s) _wls->ptr_focus = NULL;
            if (_wls->kbd_focus == s) _wls->kbd_focus = NULL;
            if (_wls->cursor_surf == s) _wls->cursor_surf = NULL;
        }
        if (s->toplevel)
            _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_UNMAP, s->toplevel);
    }
    if (s->toplevel) {
        vt_free(s->toplevel->title);
        vt_free(s->toplevel->app_id);
        vt_free(s->toplevel);
    }
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
    wl_list_init(&s->subs);
    wl_list_init(&s->sub_link);
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

/* ------------------------------------------------ wl_subcompositor */
/* Real clients (GTK/Qt/kitty) use subsurfaces for menus, overlays and
 * sometimes video planes. Without the global they abort surface
 * creation. Children paint relative to their parent, above it, in the
 * order place_above/place_below established. */
static void _subsurface_destroy(struct wl_client *cli,
                                struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _subsurface_set_position(struct wl_client *cli,
                                     struct wl_resource *res,
                                     int32_t x, int32_t y) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    /* store relative offsets in dx/dy reuse */
    s->dx = x;
    s->dy = y;
    if (_wls) _wls->dirty = true;
}
static void _subsurface_place(struct wl_resource *res,
                              struct wl_resource *sib_res, bool above) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    _wl_surf_t *sib = sib_res ?
        wl_resource_get_user_data(sib_res) : NULL;
    if (!s || !s->parent || !sib || sib->parent != s->parent) return;
    wl_list_remove(&s->sub_link);
    if (above) wl_list_insert(&sib->sub_link, &s->sub_link);
    else wl_list_insert(sib->sub_link.prev, &s->sub_link);
    if (_wls) _wls->dirty = true;
}
static void _subsurface_place_above(struct wl_client *cli,
                                    struct wl_resource *res,
                                    struct wl_resource *sib) {
    (void)cli;
    _subsurface_place(res, sib, true);
}
static void _subsurface_place_below(struct wl_client *cli,
                                    struct wl_resource *res,
                                    struct wl_resource *sib) {
    (void)cli;
    _subsurface_place(res, sib, false);
}
static void _subsurface_set_sync(struct wl_client *cli,
                                 struct wl_resource *res) {
    /* every commit repaints the whole scene, so synchronized
     * semantics are what we always provide */
    (void)cli; (void)res;
}
static void _subsurface_set_desync(struct wl_client *cli,
                                   struct wl_resource *res) {
    (void)cli; (void)res;
}
static const struct wl_subsurface_interface _subsurface_impl = {
    .destroy = _subsurface_destroy,
    .set_position = _subsurface_set_position,
    .place_above = _subsurface_place_above,
    .place_below = _subsurface_place_below,
    .set_sync = _subsurface_set_sync,
    .set_desync = _subsurface_set_desync,
};

static void _subsurface_res_destroy(struct wl_resource *res) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (s) s->parent = NULL;   /* link removal happens in the surface
                                  destroy path */
}

static void _subcompositor_get_subsurface(struct wl_client *cli,
                                          struct wl_resource *res,
                                          uint32_t id,
                                          struct wl_resource *surface,
                                          struct wl_resource *parent) {
    (void)res;
    _wl_surf_t *s = wl_resource_get_user_data(surface);
    _wl_surf_t *p = wl_resource_get_user_data(parent);
    if (!s || !p || s == p || s->parent) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "invalid subsurface");
        return;
    }
    struct wl_resource *r = wl_resource_create(
        cli, &wl_subsurface_interface,
        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_subsurface_impl, s,
                                   _subsurface_res_destroy);
    s->parent = p;
    /* children start on top of the parent */
    wl_list_insert(p->subs.prev, &s->sub_link);
    if (_wls) _wls->dirty = true;
}

static void _subcompositor_destroy(struct wl_client *cli,
                                   struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct wl_subcompositor_interface _subcompositor_impl = {
    .destroy = _subcompositor_destroy,
    .get_subsurface = _subcompositor_get_subsurface,
};

static void _bind_subcompositor(struct wl_client *cli, void *data,
                                uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &wl_subcompositor_interface, version < 1 ? 1 : 1, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_subcompositor_impl, NULL, NULL);
}

/* -------------------------------------------------- wl_data_device_manager */
/* In-session clipboard: one selection source at a time; the focused
 * client receives the selection offer on focus change and on
 * set_selection. Copy/paste between Vantage clients works; there is
 * no X11/mime bridging here (nothing outside the session to bridge
 * with). */
typedef struct _data_src {
    struct wl_resource *res;         /* wl_data_source */
    struct wl_client  *cli;
    vt_vec_t          mimes;         /* char* */
    bool              dead;
} _data_src_t;

typedef struct _data_dev {
    struct wl_resource *res;         /* wl_data_device */
    struct wl_client  *cli;
    struct wl_list     link;
} _data_dev_t;

static void _offer_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _offer_receive(struct wl_client *cli, struct wl_resource *res,
                           const char *mime, int32_t fd) {
    (void)cli;
    /* the receiving client wants the data: forward to the source */
    _wl_state_t *st = _wls;
    if (!st || !st->selection || st->selection->dead) { close(fd); return; }
    wl_data_source_send_send(st->selection->res, mime, fd);
}
static void _offer_finish(struct wl_client *cli, struct wl_resource *res) {
    (void)cli; (void)res;
}
static void _offer_accept(struct wl_client *cli, struct wl_resource *res,
                          uint32_t serial, const char *mime) {
    (void)cli; (void)res; (void)serial; (void)mime;
}
static void _offer_set_actions(struct wl_client *cli,
                               struct wl_resource *res, uint32_t dnd,
                               uint32_t ask) {
    (void)cli; (void)res; (void)dnd; (void)ask;
}
static const struct wl_data_offer_interface _offer_impl = {
    .accept = _offer_accept,
    .receive = _offer_receive,
    .destroy = _offer_destroy,
    .finish = _offer_finish,
    .set_actions = _offer_set_actions,
};

static void _send_selection(_wl_state_t *st, struct wl_resource *dev_res) {
    if (!st->selection || st->selection->dead) {
        wl_data_device_send_selection(dev_res, NULL);
        return;
    }
    struct wl_resource *offer = wl_resource_create(
        wl_resource_get_client(dev_res), &wl_data_offer_interface,
        wl_resource_get_version(dev_res), 0);
    if (!offer) return;
    wl_resource_set_implementation(offer, &_offer_impl, NULL, NULL);
    for (size_t i = 0; i < st->selection->mimes.size; i++) {
        const char *m = *(const char *const *)
            vt_vec_at(&st->selection->mimes, i);
        wl_data_device_send_data_offer(dev_res, offer);
        wl_data_offer_send_offer(offer, m);
    }
    wl_data_device_send_selection(dev_res, offer);
}

static void _broadcast_selection(_wl_state_t *st) {
    if (!st || !st->kbd_focus || !st->kbd_focus->res) return;
    _data_dev_t *d;
    wl_list_for_each(d, &st->data_devs, link) {
        if (d->cli == wl_resource_get_client(st->kbd_focus->res))
            _send_selection(st, d->res);
    }
}

static void _src_offer(struct wl_client *cli, struct wl_resource *res,
                       const char *mime) {
    (void)cli;
    _data_src_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    char *m = vt_strdup(mime ? mime : "");
    vt_vec_push(&s->mimes, &m);
}
static void _src_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _data_src_t *s = wl_resource_get_user_data(res);
    if (s) s->dead = true;
    wl_resource_destroy(res);
}
static void _src_set_actions(struct wl_client *cli, struct wl_resource *res,
                             uint32_t dnd) {
    (void)cli; (void)res; (void)dnd;
}
static const struct wl_data_source_interface _data_src_impl = {
    .offer = _src_offer,
    .destroy = _src_destroy,
    .set_actions = _src_set_actions,
};

static void _src_res_destroy(struct wl_resource *res) {
    _data_src_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    if (_wls && _wls->selection == s) {
        _wls->selection = NULL;
        _broadcast_selection(_wls);     /* selection cleared */
    }
    for (size_t i = 0; i < s->mimes.size; i++) {
        char **m = vt_vec_at(&s->mimes, i);
        vt_free(*m);
    }
    vt_vec_fini(&s->mimes);
    vt_free(s);
}

static void _dev_set_selection(struct wl_client *cli,
                               struct wl_resource *res,
                               struct wl_resource *src, uint32_t serial) {
    (void)cli; (void)serial;
    _wl_state_t *st = _wls;
    if (!st) return;
    _data_src_t *s = src ? wl_resource_get_user_data(src) : NULL;
    if (s && s->dead) s = NULL;
    if (st->selection && st->selection != s && !st->selection->dead)
        wl_data_source_send_cancelled(st->selection->res);
    st->selection = s;
    _broadcast_selection(st);
}
static void _dev_start_drag(struct wl_client *cli, struct wl_resource *res,
                            struct wl_resource *src, struct wl_resource *orig,
                            struct wl_resource *icon, uint32_t serial) {
    (void)cli; (void)res; (void)src; (void)orig; (void)icon; (void)serial;
    /* drag-and-drop is not implemented; the request is accepted and
     * ignored (clients fall back to selection semantics) */
}
static void _dev_release(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_data_device_interface _data_dev_impl = {
    .start_drag = _dev_start_drag,
    .set_selection = _dev_set_selection,
    .release = _dev_release,
};

static void _dev_res_destroy(struct wl_resource *res) {
    _data_dev_t *d = wl_resource_get_user_data(res);
    if (!d) return;
    wl_list_remove(&d->link);
    vt_free(d);
}

static void _ddm_get_data_device(struct wl_client *cli,
                                 struct wl_resource *res, uint32_t id,
                                 struct wl_resource *seat) {
    (void)seat;
    _wl_state_t *st = _wls;
    if (!st) return;
    _data_dev_t *d = vt_malloc0(sizeof(*d));
    if (!d) { wl_client_post_no_memory(cli); return; }
    d->cli = cli;
    struct wl_resource *r = wl_resource_create(
        cli, &wl_data_device_interface,
        wl_resource_get_version(res), id);
    if (!r) { vt_free(d); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_data_dev_impl, d,
                                   _dev_res_destroy);
    d->res = r;
    wl_list_insert(st->data_devs.prev, &d->link);
    /* current selection (if any) goes to newly bound devices */
    if (st->kbd_focus && st->kbd_focus->res &&
        wl_resource_get_client(st->kbd_focus->res) == cli)
        _send_selection(st, r);
}

static void _ddm_create_data_source(struct wl_client *cli,
                                    struct wl_resource *res, uint32_t id) {
    (void)res;
    _data_src_t *s = vt_malloc0(sizeof(*s));
    if (!s) { wl_client_post_no_memory(cli); return; }
    s->cli = cli;
    vt_vec_init(&s->mimes, sizeof(char *), 4);
    struct wl_resource *r = wl_resource_create(
        cli, &wl_data_source_interface, 1, id);
    if (!r) {
        vt_vec_fini(&s->mimes);
        vt_free(s);
        wl_client_post_no_memory(cli);
        return;
    }
    wl_resource_set_implementation(r, &_data_src_impl, s,
                                   _src_res_destroy);
    s->res = r;
}

static const struct wl_data_device_manager_interface _ddm_impl = {
    .create_data_source = _ddm_create_data_source,
    .get_data_device = _ddm_get_data_device,
};

static void _bind_ddm(struct wl_client *cli, void *data,
                      uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &wl_data_device_manager_interface,
        version < 3 ? version : 3, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_ddm_impl, NULL, NULL);
}

/* ------------------------------------------------------------ xdg-shell */
/* per-surface xdg data to route get_toplevel */
typedef struct {
    _wl_surf_t *surf;
} _xdg_surf_data_t;

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

static void _xdg_surface_set_window_geometry(struct wl_client *cli,
                                              struct wl_resource *res,
                                              int32_t x, int32_t y,
                                              int32_t w, int32_t h) {
    (void)cli;
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    if (d && d->surf) {
        d->surf->x += x;
        d->surf->y += y;
        if (w > 0) d->surf->w = w;
        if (h > 0) d->surf->h = h;
        if (d->surf->toplevel)
            _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_GEOMETRY,
                      d->surf->toplevel);
    }
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
        _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_TITLE, t);
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
static void _toplevel_move(struct wl_client *cli,
                           struct wl_resource *res,
                           struct wl_resource *seat, uint32_t serial) {
    (void)cli; (void)seat; (void)serial;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t && t->surf && _wls) {
        _wls->op_active = true;
        _wls->op_resize = false;
        _wls->op_surf = t->surf;
        _wls->op_grab_x = _wls->cursor_x - t->surf->x;
        _wls->op_grab_y = _wls->cursor_y - t->surf->y;
    }
}
static void _toplevel_set_parent(struct wl_client *cli,
                                 struct wl_resource *res,
                                 struct wl_resource *parent) {
    (void)cli; (void)res; (void)parent;
}
static void _toplevel_show_window_menu(struct wl_client *cli,
                                        struct wl_resource *res,
                                        struct wl_resource *seat,
                                        uint32_t serial, int32_t x,
                                        int32_t y) {
    (void)cli; (void)res; (void)seat; (void)serial; (void)x; (void)y;
}
static void _toplevel_resize(struct wl_client *cli,
                             struct wl_resource *res,
                             struct wl_resource *seat, uint32_t serial,
                             uint32_t edges) {
    (void)cli; (void)seat; (void)serial; (void)edges;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t && t->surf && _wls) {
        _wls->op_active = true;
        _wls->op_resize = true;
        _wls->op_surf = t->surf;
        _wls->op_grab_x = _wls->cursor_x;
        _wls->op_grab_y = _wls->cursor_y;
        _wls->op_start_w = t->surf->w;
        _wls->op_start_h = t->surf->h;
    }
}
static void _toplevel_set_max(struct wl_client *cli,
                              struct wl_resource *res,
                              int32_t w, int32_t h) {
    (void)cli; (void)res; (void)w; (void)h;
}
static void _toplevel_set_min(struct wl_client *cli,
                              struct wl_resource *res,
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
    if (t) _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
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
    if (t) _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
}
static void _toplevel_fullscreen(struct wl_client *cli,
                                 struct wl_resource *res,
                                 struct wl_resource *output) {
    (void)cli; (void)output;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) t->fullscreen = true;
    _toplevel_configure(res, _wls->out_w, _wls->out_h,
                        XDG_TOPLEVEL_STATE_FULLSCREEN);
    if (t) _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
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
    if (t) _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
}
static const struct xdg_toplevel_interface _toplevel_impl = {
    .destroy = _toplevel_destroy,
    .set_parent = _toplevel_set_parent,
    .set_title = _toplevel_set_title,
    .set_app_id = _toplevel_set_app_id,
    .show_window_menu = _toplevel_show_window_menu,
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
static void _xdg_get_toplevel(struct wl_client *cli,
                              struct wl_resource *res, uint32_t id);
static void _xdg_get_popup(struct wl_client *cli, struct wl_resource *res,
                           uint32_t id, struct wl_resource *parent,
                           struct wl_resource *positioner);

static const struct xdg_surface_interface _xdg_surface_impl2 = {
    .destroy = _xdg_surface_destroy,
    .get_toplevel = _xdg_get_toplevel,
    .get_popup = _xdg_get_popup,
    .set_window_geometry = _xdg_surface_set_window_geometry,
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
    t->id = ++_wls->next_win_id;
    d->surf->toplevel = t;
    d->surf->has_pending_xdg = true;
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
static void _pos_set_gravity(struct wl_client *cli,
                             struct wl_resource *res,
                             uint32_t g) { (void)cli; (void)res; (void)g; }
static void _pos_set_offset(struct wl_client *cli, struct wl_resource *res,
                            int32_t x, int32_t y) { (void)cli; (void)res; (void)x; (void)y; }
static void _pos_set_anchor_rect(struct wl_client *cli,
                                 struct wl_resource *res,
                                 int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
}
static const struct xdg_positioner_interface _pos_impl = {
    .destroy = _pos_destroy,
    .set_size = _pos_set_size,
    .set_anchor_rect = _pos_set_anchor_rect,
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

static void _xdg_get_popup(struct wl_client *cli,
                           struct wl_resource *res,
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

/* --------------------------------------------------------- wl_pointer */

static void _ptr_set_cursor(struct wl_client *cli,
                            struct wl_resource *res, uint32_t serial,
                            struct wl_resource *surface,
                            int32_t hotspot_x, int32_t hotspot_y) {
    (void)cli; (void)serial;
    _wl_state_t *st = _wls;
    if (!st) return;
    if (surface) {
        _wl_surf_t *s = wl_resource_get_user_data(surface);
        if (s) {
            s->is_cursor = true;
            s->hotspot_x = hotspot_x;
            s->hotspot_y = hotspot_y;
            st->cursor_surf = s;
            st->cur_client_set = true;
        }
    } else {
        st->cursor_surf = NULL;
        st->cur_client_set = false;
    }
}
static void _ptr_release(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_pointer_interface _ptr_impl = {
    .set_cursor = _ptr_set_cursor,
    .release = _ptr_release,
};

static void _ptr_res_destroy(struct wl_resource *res) {
    _wl_state_t *st = _wls;
    if (!st) return;
    _ptr_res_t *p;
    wl_list_for_each(p, &st->ptr_reses, link) {
        if (p->res == res) { wl_list_remove(&p->link); vt_free(p); return; }
    }
}

/* ------------------------------------------------------- wl_keyboard */

static void _kbd_release(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_keyboard_interface _kbd_impl = {
    .release = _kbd_release,
};

static void _kbd_res_destroy(struct wl_resource *res) {
    _wl_state_t *st = _wls;
    if (!st) return;
    _kbd_res_t *k;
    wl_list_for_each(k, &st->kbd_reses, link) {
        if (k->res == res) { wl_list_remove(&k->link); vt_free(k); return; }
    }
}

/* send the shared keymap to one keyboard resource (fresh fd per send) */
static void _kbd_send_keymap(_wl_state_t *st, struct wl_resource *kres) {
#if defined(VT_HAVE_XKBCOMMON)
    if (st->keymap_fd >= 0 && st->keymap_size > 0) {
        int fd = dup(st->keymap_fd);
        if (fd >= 0) {
            wl_keyboard_send_keymap(kres, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                     fd, (uint32_t)st->keymap_size);
            close(fd);
            return;
        }
    }
#endif
    /* degenerate empty keymap — clients must cope */
    wl_keyboard_send_keymap(kres, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, -1, 0);
}

/* ----------------------------------------------------------- wl_seat */

static void _seat_get_pointer(struct wl_client *cli,
                              struct wl_resource *res, uint32_t id) {
    _wl_state_t *st = _wls;
    struct wl_resource *p = wl_resource_create(cli, &wl_pointer_interface,
                                               wl_resource_get_version(res),
                                               id);
    if (!p) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(p, &_ptr_impl, NULL, _ptr_res_destroy);
    if (st) {
        _ptr_res_t *pr = vt_malloc0(sizeof(*pr));
        pr->res = p;
        wl_list_insert(st->ptr_reses.prev, &pr->link);
    }
}
static void _seat_get_keyboard(struct wl_client *cli,
                               struct wl_resource *res, uint32_t id) {
    _wl_state_t *st = _wls;
    struct wl_resource *k = wl_resource_create(cli, &wl_keyboard_interface,
                                               wl_resource_get_version(res),
                                               id);
    if (!k) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(k, &_kbd_impl, NULL, _kbd_res_destroy);
    _kbd_send_keymap(st, k);
    if (st) {
        _kbd_res_t *kr = vt_malloc0(sizeof(*kr));
        kr->res = k;
        wl_list_insert(st->kbd_reses.prev, &kr->link);
        /* if a surface is already focused, enter immediately */
        if (st->kbd_focus && st->kbd_focus->res) {
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(k, ++st->serial, st->kbd_focus->res,
                                   &keys);
            wl_array_release(&keys);
        }
    }
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
static void _bind_seat(struct wl_client *cli, void *data,
                       uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli, &wl_seat_interface,
                                                 version < 5 ? version : 5,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_seat_impl, NULL, NULL);
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
}

/* --------------------------------------------------------- wl_output */

static void _bind_output(struct wl_client *cli, void *data, uint32_t version,
                         uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli, &wl_output_interface,
                                                 version < 3 ? version : 3,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, NULL, NULL, NULL);
    int w = _wls ? _wls->out_w : 1024;
    int h = _wls ? _wls->out_h : 768;
    wl_output_send_geometry(res, 0, 0, w * 254 / 960, h * 254 / 960, 0,
                            "unknown", "unknown", 0);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        w, h, 60000);
    if (wl_resource_get_version(res) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(res, 1);
    if (wl_resource_get_version(res) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
}

/* ------------------------------------------------------ cursor sprite */

/* built-in 16x16 arrow (1-bit, '#' = black outline, 'o' = white fill) */
static const char *const _arrow_bits[] = {
    "################",
    "#oooooooooooooo#",
    "#oo#############",
    "#ooo..........##",
    "#oooo..........#",
    "#ooooo.........#",
    "#oooooo........#",
    "#ooooooo.......#",
    "#oooooooo......#",
    "#oooooo........#",
    "#oo.ooo........#",
    "#o...ooo.......#",
    "#.....ooo......#",
    "#......ooo.....#",
    "#.......ooooo..#",
    "################",
};

static void _cursor_default_arrow(_wl_state_t *st) {
    const int n = 16;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            char c = _arrow_bits[y][x];
            uint32_t px;
            if (c == '#') px = 0xff000000;        /* black outline */
            else if (c == 'o') px = 0xffffffff;   /* white body */
            else px = 0x00000000;                 /* transparent */
            st->cursor_img[y * 64 + x] = px;
        }
    }
    st->cur_img_w = n;
    st->cur_img_h = n;
    st->cur_img_hx = 0;
    st->cur_img_hy = 0;
}

static void _cursor_init(_wl_state_t *st) {
    _cursor_default_arrow(st);
    /* VANTAGE_WL_CURSOR=builtin forces the built-in 16x16 arrow —
     * used by tests (exact pixel assertions) and as an override when a
     * theme's cursors misbehave. */
    const char *force = getenv("VANTAGE_WL_CURSOR");
    if (force && vt_streq(force, "builtin")) {
        vt_logi("wayland: cursor: built-in arrow forced "
                "(VANTAGE_WL_CURSOR=builtin)");
        return;
    }
#if defined(VT_HAVE_XCURSOR)
    const char *theme = getenv("XCURSOR_THEME");
    const char *szs = getenv("XCURSOR_SIZE");
    int size = szs && *szs ? atoi(szs) : 24;
    if (size <= 0 || size > 64) size = 24;
    const char *shape = "left_ptr";
    XcursorImages *imgs = XcursorLibraryLoadImages(shape,
                                                   theme && *theme ?
                                                   theme : "default",
                                                   size);
    if (!imgs && theme && *theme)
        imgs = XcursorLibraryLoadImages(shape, "default", size);
    if (imgs && imgs->nimage > 0) {
        XcursorImage *im = imgs->images[0];
        if (im && im->width <= 64 && im->height <= 64 && im->pixels) {
            memset(st->cursor_img, 0, sizeof(st->cursor_img));
            for (uint32_t y = 0; y < im->height; y++)
                for (uint32_t x = 0; x < im->width; x++)
                    st->cursor_img[y * 64 + x] =
                        ((const uint32_t *)im->pixels)[y * im->width + x];
            st->cur_img_w = (int)im->width;
            st->cur_img_h = (int)im->height;
            st->cur_img_hx = (int)im->xhot;
            st->cur_img_hy = (int)im->yhot;
            vt_logi("wayland: cursor: Xcursor '%s' theme '%s' %ux%u "
                    "(hotspot %u,%u)", shape,
                    theme && *theme ? theme : "default",
                    im->width, im->height, im->xhot, im->yhot);
        }
        XcursorImagesDestroy(imgs);
        return;
    }
    vt_logi("wayland: cursor: Xcursor images unavailable (theme '%s') — "
            "built-in arrow sprite", theme && *theme ? theme : "default");
#else
    vt_logi("wayland: cursor: built without Xcursor — built-in arrow "
            "sprite");
#endif
}

/* push the active cursor image to the hardware plane when available */
static void _cursor_apply_hw(_wl_state_t *st) {
    if (!st || !st->kms) return;
    if (st->cur_client_set && st->cursor_surf && st->cursor_surf->pixels) {
        /* client cursor surface: software sprite */
        vt_kms_cursor_hide(st->kms);
        return;
    }
    vt_kms_cursor_set(st->kms, st->cursor_img,
                      st->cur_img_w, st->cur_img_h, 64);
}

/* ----------------------------------------------------- input: libinput */

#if defined(VT_HAVE_LIBINPUT)

static int _li_open_restricted(const char *path, int flags, void *ud) {
    _wl_state_t *st = ud;
    if (st && st->seat) {
        int fd = vt_seat_open_device(st->seat, path);
        if (fd >= 0) {
            int fl = fcntl(fd, F_GETFL);
            fcntl(fd, F_SETFL, fl | (flags & O_NONBLOCK));
            return fd;
        }
        vt_logw("wayland: input: cannot open %s through the seat — "
                "EACCES usually means no session manager holds the "
                "device permissions", path);
        return -1;
    }
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        vt_logw("wayland: input: open(%s) failed: %s — without a "
                "session manager the user needs the 'input' group "
                "(usermod -aG input $USER, re-login), or install "
                "elogind/seatd", path, strerror(errno));
    return fd;
}

static void _li_close_restricted(int fd, void *ud) {
    _wl_state_t *st = ud;
    if (st && st->seat) vt_seat_close_device(st->seat, fd);
    else close(fd);
}

static const struct libinput_interface _li_iface = {
    .open_restricted = _li_open_restricted,
    .close_restricted = _li_close_restricted,
};

static void _li_add_device(_wl_state_t *st,
                           struct libinput_device *dev) {
    const char *name = libinput_device_get_name(dev);
    bool has_ptr = libinput_device_has_capability(
        dev, LIBINPUT_DEVICE_CAP_POINTER);
    bool has_kb = libinput_device_has_capability(
        dev, LIBINPUT_DEVICE_CAP_KEYBOARD);
    bool has_touch = libinput_device_has_capability(
        dev, LIBINPUT_DEVICE_CAP_TOUCH);
    vt_logi("wayland: input device '%s' (%s%s%s)",
            name ? name : "?",
            has_ptr ? "pointer " : "",
            has_kb ? "keyboard " : "",
            has_touch ? "touch" : "");
    vt_input_dev_t d = {0};
    d.name = vt_strdup(name ? name : "input");
    d.syspath = vt_strdup(libinput_device_get_sysname(dev) ?
                          libinput_device_get_sysname(dev) : "");
    d.type = has_kb ? 0 : (has_ptr || has_touch) ? 1 : 2;
    d.id = (int)st->backend_self->inputs.size;
    d.active = true;
    vt_vec_push(&st->backend_self->inputs, &d);
}

static _wl_surf_t *_surface_at(_wl_state_t *st, int x, int y) {
    _wl_surf_t *found = NULL;
    _wl_surf_t *s;
    if (st->panel && vt_wl_panel_contains(st->panel, x, y)) return NULL;
    /* surfaces list is bottom→top: iterate reversed */
    wl_list_for_each_reverse(s, &st->surfaces, link) {
        if (!s->mapped || s->is_cursor) continue;
        if (s->toplevel && s->ws != st->ws_cur) continue;
        if (x >= s->x && x < s->x + s->w &&
            y >= s->y && y < s->y + s->h) {
            found = s;
            break;
        }
    }
    return found;
}

static void _pointer_focus_update(_wl_state_t *st, bool force) {
    _wl_surf_t *s = _surface_at(st, st->cursor_x, st->cursor_y);
    if (s == st->ptr_focus && !force) return;
    /* leave old */
    if (st->ptr_focus && st->ptr_focus->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(st->ptr_focus->res)) {
                wl_pointer_send_leave(pr->res, ++st->serial,
                                      st->ptr_focus->res);
            }
        }
    }
    st->ptr_focus = s;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                wl_pointer_send_enter(pr->res, ++st->serial, s->res,
                                      (wl_fixed_t)(st->cursor_x - s->x) * 256,
                                      (wl_fixed_t)(st->cursor_y - s->y) * 256);
            }
        }
        /* click-to-focus for the keyboard */
        if (st->kbd_focus != s) {
            if (st->kbd_focus && st->kbd_focus->res) {
                _kbd_res_t *kr;
                wl_list_for_each(kr, &st->kbd_reses, link) {
                    if (wl_resource_get_client(kr->res) ==
                        wl_resource_get_client(st->kbd_focus->res))
                        wl_keyboard_send_leave(kr->res, ++st->serial,
                                               st->kbd_focus->res);
                }
            }
            st->kbd_focus = s;
            st->focused_toplevel = s->toplevel;
            _kbd_res_t *kr;
            wl_list_for_each(kr, &st->kbd_reses, link) {
                if (wl_resource_get_client(kr->res) ==
                    wl_resource_get_client(s->res)) {
                    struct wl_array keys;
                    wl_array_init(&keys);
                    wl_keyboard_send_enter(kr->res, ++st->serial, s->res,
                                           &keys);
                    wl_array_release(&keys);
                }
            }
            if (s->toplevel)
                _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, s->toplevel);
            _broadcast_selection(st);
        }
    }
}

static void _pointer_motion(_wl_state_t *st, double dx, double dy) {
    st->cursor_x += (int)dx;
    st->cursor_y += (int)dy;
    if (st->cursor_x < 0) st->cursor_x = 0;
    if (st->cursor_y < 0) st->cursor_y = 0;
    if (st->cursor_x >= st->out_w) st->cursor_x = st->out_w - 1;
    if (st->cursor_y >= st->out_h) st->cursor_y = st->out_h - 1;
    if (st->kms && !st->cur_client_set)
        vt_kms_cursor_move(st->kms, st->cursor_x - st->cur_img_hx,
                           st->cursor_y - st->cur_img_hy);
    _pointer_focus_update(st, false);
    _wl_surf_t *s = st->ptr_focus;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res))
                wl_pointer_send_motion(pr->res, 0,
                    (wl_fixed_t)(st->cursor_x - s->x) * 256,
                    (wl_fixed_t)(st->cursor_y - s->y) * 256);
        }
    }
    /* interactive move/resize */
    if (st->op_active && st->op_surf) {
        if (st->op_resize) {
            st->op_surf->w = st->op_start_w + st->cursor_x - st->op_grab_x;
            st->op_surf->h = st->op_start_h + st->cursor_y - st->op_grab_y;
            if (st->op_surf->w < 1) st->op_surf->w = 1;
            if (st->op_surf->h < 1) st->op_surf->h = 1;
            if (st->op_surf->toplevel)
                _toplevel_configure(st->op_surf->toplevel->res,
                                    st->op_surf->w, st->op_surf->h, 0);
        } else {
            st->op_surf->x = st->cursor_x - st->op_grab_x;
            st->op_surf->y = st->cursor_y - st->op_grab_y;
            if (st->op_surf->x < 0) st->op_surf->x = 0;
            if (st->op_surf->y < 0) st->op_surf->y = 0;
            if (st->op_surf->toplevel)
                _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY,
                          st->op_surf->toplevel);
        }
    }
    st->dirty = true;
}

static void _pointer_button(_wl_state_t *st, uint32_t button,
                            bool pressed) {
    if (button == 0x110 && !pressed)
        st->op_active = false;     /* BTN_LEFT release ends interactive op */
    if (st->op_active && pressed) return;
    /* panel (bar + menus) swallows pointer events before any client */
    if (st->panel &&
        vt_wl_panel_pointer(st->panel, st->cursor_x, st->cursor_y,
                            pressed ? 1 : 2,
                            button == 0x110 ? 1 :
                            button == 0x111 ? 2 :
                            button == 0x112 ? 3 : 0)) {
        st->dirty = true;
        return;
    }
    _wl_surf_t *s = st->ptr_focus;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res))
                wl_pointer_send_button(pr->res, ++st->serial, 0, button,
                                       pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                               : WL_POINTER_BUTTON_STATE_RELEASED);
        }
    }
}

static void _pointer_axis(_wl_state_t *st, double value) {
    /* panel (menus + volume wheel) consumes scroll events first */
    if (st->panel &&
        vt_wl_panel_axis(st->panel, st->cursor_x, st->cursor_y,
                         value > 0 ? 1 : -1)) {
        st->dirty = true;
        return;
    }
    _wl_surf_t *s = st->ptr_focus;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                wl_pointer_send_axis(pr->res, 0,
                                     WL_POINTER_AXIS_VERTICAL_SCROLL,
                                     (wl_fixed_t)(value * 256));
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_AXIS_STOP_SINCE_VERSION)
                    wl_pointer_send_axis_stop(pr->res, 0,
                                              WL_POINTER_AXIS_VERTICAL_SCROLL);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
    }
}

#if defined(VT_HAVE_XKBCOMMON)
/* build "Alt+Tab"-style combo strings for the WM shortcut table */
static bool _combo_from_xkb(_wl_state_t *st, xkb_keysym_t sym, char *buf,
                            size_t bn) {
    uint32_t mods = xkb_state_serialize_mods(
        st->xkb_st, XKB_STATE_MODS_DEPRESSED);
    bool shift = mods & 1, ctrl = mods & 4, alt = mods & 8, super = mods & 64;
    char name[64];
    if (xkb_keysym_get_name(sym, name, sizeof(name)) <= 0) return false;
    /* normalize a few names to the WM spelling */
    if (strcmp(name, "ISO_Left_Tab") == 0) snprintf(name, sizeof(name), "Tab");
    snprintf(buf, bn, "%s%s%s%s%s",
             ctrl ? "Ctrl+" : "", alt ? "Alt+" : "",
             super ? "Super+" : "", shift ? "Shift+" : "", name);
    return true;
}

static void _kbd_modifiers_send(_wl_state_t *st) {
    if (!st->kbd_focus || !st->kbd_focus->res) return;
    _kbd_res_t *kr;
    wl_list_for_each(kr, &st->kbd_reses, link) {
        if (wl_resource_get_client(kr->res) ==
            wl_resource_get_client(st->kbd_focus->res)) {
            wl_keyboard_send_modifiers(
                kr->res, ++st->serial,
                xkb_state_serialize_mods(st->xkb_st,
                                         XKB_STATE_MODS_DEPRESSED),
                xkb_state_serialize_mods(st->xkb_st,
                                         XKB_STATE_MODS_LATCHED),
                xkb_state_serialize_mods(st->xkb_st,
                                         XKB_STATE_MODS_LOCKED),
                xkb_state_serialize_layout(st->xkb_st,
                                           XKB_STATE_LAYOUT_EFFECTIVE));
        }
    }
}
#endif /* VT_HAVE_XKBCOMMON */

/* Ctrl+Alt+F1..F12 switches VTs — the compositor owns the keyboard via
 * evdev, so the kernel's own console switch combination never fires; we
 * must perform the switch ourselves or a wedged session means a reboot. */
static void _vt_hotkey(_wl_state_t *st, xkb_keysym_t sym) {
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
        int vt = (int)(sym - XKB_KEY_F1) + 1;
        vt_logi("wayland: VT-switch hotkey Ctrl+Alt+F%d — switching "
                "(release/acquire will drop/retake DRM master)", vt);
        if (vt_seat_vt_switch_to(st->seat, vt) != 0)
            vt_logw("wayland: VT switch to %d failed — keyboard input "
                    "may be the only way out (Ctrl+Alt+F1..F12, or "
                    "Ctrl+Alt+Delete to log out)", vt);
    }
}

static void _kbd_key(_wl_state_t *st, uint32_t key, bool pressed) {
#if defined(VT_HAVE_XKBCOMMON)
    if (!st->xkb_st) return;
    xkb_state_update_key(st->xkb_st, key + 8,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    _kbd_modifiers_send(st);
    if (pressed) {
        const xkb_keysym_t *syms;
        int ns = xkb_state_key_get_syms(st->xkb_st, key + 8, &syms);
        for (int i = 0; i < ns; i++) {
            /* open panel menus consume keys first: Escape closes,
             * BackSpace edits the search, printable characters type
             * into the search bar */
            if (st->panel) {
                const char *combo = NULL;
                char cmb[96] = "";
                if (syms[i] == XKB_KEY_Escape) combo = "Escape";
                else if (syms[i] == XKB_KEY_BackSpace) combo = "BackSpace";
                else if (!_combo_from_xkb(st, syms[i], cmb, sizeof(cmb)))
                    combo = NULL;
                /* try combo form first (Escape/BackSpace) */
                if (combo && vt_wl_panel_key(st->panel, combo, 0)) {
                    st->dirty = true;
                    return;
                }
                /* printable → search text */
                uint32_t cp = xkb_keysym_to_utf32(syms[i]);
                if (!combo && cp > 0 &&
                    vt_wl_panel_key(st->panel, NULL, cp)) {
                    st->dirty = true;
                    return;
                }
            }
            char combo[96];
            if (_combo_from_xkb(st, syms[i], combo, sizeof(combo)) &&
                _hotkey_try(st->backend_self, combo)) {
                vt_logi("wayland: hotkey consumed: %s", combo);
                return;    /* do not forward to the client */
            }
            /* VT switching: Ctrl+Alt+F1..F12 */
            uint32_t mods = xkb_state_serialize_mods(
                st->xkb_st, XKB_STATE_MODS_DEPRESSED);
            if ((mods & 0x4) && (mods & 0x8) && st->seat) {
                _vt_hotkey(st, syms[i]);
                return;
            }
        }
    }
#endif
    /* forward to the focused client */
    _wl_surf_t *s = st->kbd_focus;
    if (s && s->res) {
        _kbd_res_t *kr;
        wl_list_for_each(kr, &st->kbd_reses, link) {
            if (wl_resource_get_client(kr->res) ==
                wl_resource_get_client(s->res))
                wl_keyboard_send_key(kr->res, ++st->serial, 0, key,
                                     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                             : WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

static void _li_process(_wl_state_t *st) {
    struct libinput_event *ev;
    while ((ev = libinput_get_event(st->li))) {
        enum libinput_event_type t = libinput_event_get_type(ev);
        switch (t) {
        case LIBINPUT_EVENT_DEVICE_ADDED:
            _li_add_device(st, libinput_event_get_device(ev));
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED: {
            /* remove from the model — a removed device usually means the
             * open failed (EACCES: no session manager, no input group) */
            struct libinput_device *dev = libinput_event_get_device(ev);
            const char *sys = dev ? libinput_device_get_sysname(dev) : NULL;
            if (sys) {
                for (size_t i = 0; i < st->backend_self->inputs.size; i++) {
                    vt_input_dev_t *d = vt_vec_at(&st->backend_self->inputs, i);
                    if (d->syspath && vt_streq(d->syspath, sys)) {
                        vt_free(d->name);
                        vt_free(d->syspath);
                        vt_vec_remove(&st->backend_self->inputs, i);
                        break;
                    }
                }
            }
            vt_logd("wayland: input device removed");
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION: {
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            _pointer_motion(st,
                            libinput_event_pointer_get_dx(pe),
                            libinput_event_pointer_get_dy(pe));
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            double x = libinput_event_pointer_get_absolute_x(pe);
            double y = libinput_event_pointer_get_absolute_y(pe);
            _pointer_motion(st, x - st->cursor_x, y - st->cursor_y);
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            _pointer_button(st, libinput_event_pointer_get_button(pe),
                            libinput_event_pointer_get_button_state(pe) ==
                            LIBINPUT_BUTTON_STATE_PRESSED);
            break;
        }
        case LIBINPUT_EVENT_POINTER_AXIS: {
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            if (libinput_event_pointer_get_axis_source(pe) ==
                LIBINPUT_POINTER_AXIS_SOURCE_WHEEL) {
                double v = libinput_event_pointer_get_axis_value(
                    pe, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                _pointer_axis(st, v);
            }
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            struct libinput_event_keyboard *ke =
                libinput_event_get_keyboard_event(ev);
            _kbd_key(st, (uint32_t)libinput_event_keyboard_get_key(ke),
                     libinput_event_keyboard_get_key_state(ke) ==
                     LIBINPUT_KEY_STATE_PRESSED);
            break;
        }
        default:
            break;
        }
        libinput_event_destroy(ev);
    }
}

static int _li_fd_cb(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    _wl_state_t *st = data;
    libinput_dispatch(st->li);
    _li_process(st);
    return 0;
}

static bool _input_init(_wl_state_t *st) {
    st->udev = udev_new();
    if (!st->udev) {
        vt_logw("wayland: udev_new failed: %s", strerror(errno));
        return false;
    }
    st->li = libinput_udev_create_context(&_li_iface, st, st->udev);
    if (!st->li) {
        vt_logw("wayland: libinput context failed: %s", strerror(errno));
        return false;
    }
    const char *seat = getenv("XDG_SEAT");
    if (!seat || !*seat) seat = "seat0";
    if (libinput_udev_assign_seat(st->li, seat) < 0) {
        vt_logw("wayland: libinput_udev_assign_seat(%s) failed: %s",
                seat, strerror(errno));
        return false;
    }
    int fd = libinput_get_fd(st->li);
    if (fd < 0) return false;
    st->li_src = wl_event_loop_add_fd(st->loop, fd, WL_EVENT_READABLE,
                                      _li_fd_cb, st);
    libinput_dispatch(st->li);
    _li_process(st);   /* initial device-added events */
    /* honest accounting: did we actually GET usable devices? The
     * DEVICE_ADDED event fires before the device is opened — count what
     * ended up openable instead of lying about it. */
    int kb = 0, ptr = 0;
    for (size_t i = 0; i < st->backend_self->inputs.size; i++) {
        vt_input_dev_t *d = vt_vec_at(&st->backend_self->inputs, i);
        if (d->type == 0) kb++;
        else if (d->type == 1) ptr++;
    }
    if (kb == 0 && ptr == 0) {
        vt_loge("wayland: input: NO usable input devices — keyboard and "
                "pointer will not work. Remedies: (1) run through a "
                "session manager (elogind/seatd), or (2) add the user "
                "to the 'input' group: usermod -aG input $USER and "
                "re-login. Ctrl+Alt+F1..F12 VT switching and "
                "Ctrl+Alt+Delete logout are handled by the compositor "
                "once input works.");
    }
    return true;
}

static void _input_fini(_wl_state_t *st) {
    if (st->li_src) { wl_event_source_remove(st->li_src); st->li_src = NULL; }
    if (st->li) { libinput_unref(st->li); st->li = NULL; }
    if (st->udev) { udev_unref(st->udev); st->udev = NULL; }
}

#else /* !VT_HAVE_LIBINPUT */

static bool _input_init(_wl_state_t *st) {
    (void)st;
    vt_logw("wayland: built without libinput — no real input devices");
    return false;
}
static void _input_fini(_wl_state_t *st) { (void)st; }

#endif /* VT_HAVE_LIBINPUT */

/* ------------------------------------------------------ xkb keymap */

#if defined(VT_HAVE_XKBCOMMON)
static bool _xkb_init(_wl_state_t *st) {
    st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!st->xkb_ctx) return false;
    struct xkb_rule_names names;
    memset(&names, 0, sizeof(names));   /* defaults honour XKB_DEFAULT_* */
    st->xkb_km = xkb_keymap_new_from_names(st->xkb_ctx, &names,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!st->xkb_km) return false;
    st->xkb_st = xkb_state_new(st->xkb_km);
    if (!st->xkb_st) return false;
    char *str = xkb_keymap_get_as_string(st->xkb_km,
                                         XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) return false;
    size_t len = strlen(str) + 1;
    int fd = memfd_create("vantage-xkb-keymap",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        xkb_keymap_unref(st->xkb_km);
        return false;
    }
    if (write(fd, str, len) != (ssize_t)len) {
        close(fd);
        return false;
    }
    lseek(fd, 0, SEEK_SET);
    fcntl(fd, F_ADD_SEALS,
          F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    st->keymap_fd = fd;
    st->keymap_size = len;
    vt_logi("wayland: xkb keymap ready (%zu bytes, layout '%s')",
            len, xkb_keymap_layout_get_name(st->xkb_km, 0));
    return true;
}

static void _xkb_fini(_wl_state_t *st) {
    if (st->xkb_st) { xkb_state_unref(st->xkb_st); st->xkb_st = NULL; }
    if (st->xkb_km) { xkb_keymap_unref(st->xkb_km); st->xkb_km = NULL; }
    if (st->xkb_ctx) { xkb_context_unref(st->xkb_ctx); st->xkb_ctx = NULL; }
    if (st->keymap_fd >= 0) { close(st->keymap_fd); st->keymap_fd = -1; }
}
#else
static bool _xkb_init(_wl_state_t *st) {
    (void)st;
    vt_logw("wayland: built without xkbcommon — clients get an empty "
            "keymap");
    return false;
}
static void _xkb_fini(_wl_state_t *st) { (void)st; }
#endif

/* ------------------------------------------------- seat/VT lifecycle */

static void _kms_first_scanout(vt_kms_t *k, void *ud) {
    (void)k;
    _wl_state_t *st = ud;
    if (!st) return;
    /* first real pixels on the CRTC → now switch the VT away from text
     * (doing it earlier would leave a black screen with no console) */
    if (st->seat)
        vt_seat_vt_set_graphics(st->seat, true);
    _cursor_apply_hw(st);
}

static void _seat_notify(vt_seat_t *seat, vt_seat_notify_kind_t kind,
                         void *ud) {
    (void)seat;
    _wl_state_t *st = ud;
    if (!st) return;
    if (kind == VT_SEAT_NOTIFY_DISABLE) {
        if (st->kms) vt_kms_pause(st->kms);
    } else {
        if (st->kms && vt_kms_resume(st->kms) == 0) {
            _cursor_apply_hw(st);
            st->dirty = true;
        }
    }
}

static int _seat_fd_cb(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    _wl_state_t *st = data;
    if (st && st->seat) vt_seat_dispatch(st->seat);
    return 0;
}

static int _drm_fd_cb(int fd, uint32_t mask, void *data) {
    (void)fd; (void)mask;
    _wl_state_t *st = data;
    if (st && st->kms) vt_kms_handle_events(st->kms);
    return 0;
}

/* ------------------------------------------------------------ painting */

static void _paint_background(_wl_state_t *st) {
    /* plain desktop background — the real panel is drawn by
     * vt-wl-panel.c on top of everything (not placeholder blocks) */
    for (int y = 0; y < st->out_h; y++) {
        uint32_t row = 0xff1a1a1a;
        for (int x = 0; x < st->out_w; x++)
            st->fb[y * st->out_w + x] = row;
    }
}

/* ---- panel glue: compositor → panel callbacks ---- */
static void _panel_cb_focus(uint64_t id, void *ud) {
    _wl_state_t *st = ud;
    if (!st) return;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel->id == id) {
            st->kbd_focus = s;
            st->focused_toplevel = s->toplevel;
            _pointer_focus_update(st, true);
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, s->toplevel);
            st->dirty = true;
            return;
        }
    }
}

static void _panel_cb_close(uint64_t id, void *ud) {
    vt_backend_t *self = ((_wl_state_t *)ud)->backend_self;
    if (self && self->close_window) self->close_window(self, id);
}

static void _panel_emit_ws(_wl_state_t *st) {
    vt_backend_wl_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = VT_BACKEND_WL_EVENT_WORKSPACE;
    ev.window_id = (uint64_t)(unsigned)st->ws_cur;
    ev.title = NULL;
    ev.app_id = NULL;
    vt_backend_emit_event(st->backend_self, &ev);
}

static void _panel_cb_ws(int ws, void *ud) {
    _wl_state_t *st = ud;
    if (!st || ws < 0 || ws >= st->ws_count || ws == st->ws_cur) return;
    st->ws_cur = ws;
    vt_wl_panel_set_workspaces(st->panel, st->ws_count, st->ws_cur);
    _panel_emit_ws(st);
    st->dirty = true;
    vt_logi("wayland: workspace -> %d", ws + 1);
}

static void _panel_cb_logout(const char *action, void *ud) {
    _wl_state_t *st = ud;
    if (!st) return;
    const char *act = action ? action : "";
    /* Session-managed run: the session manager owns child supervision
     * and the shutdown policy — ask IT to end the session. It will
     * SIGTERM every child (including this compositor, whose unwind
     * restores the CRTC and returns the VT to text) and then exit,
     * leaving the user back on their original TTY. Exiting here
     * WITHOUT telling the session would trigger the supervisor's
     * restart loop and re-take the screen — the old "trapped
     * session" bug.
     * Standalone run (no session manager): local clean unwind. */
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (rd && *rd) {
        char *path = vt_strprintf("%s/vantage-session.sock", rd);
        vt_ipc_t *ipc = vt_ipc_new_client(path);
        vt_free(path);
        if (ipc) {
            vt_ipc_msg_t resp = {0};
            vt_ipc_call(ipc, VT_IPC_MSG_WM_LOGOUT, act, (uint32_t)strlen(act),
                        &resp, 1500);
            vt_ipc_msg_free(&resp);
            vt_ipc_free(ipc);
            vt_logi("wayland: panel logout (%s) — session manager is "
                    "ending the session (supervised shutdown, no restart)",
                    *act ? act : "logout");
            return;
        }
    }
    if (vt_streq(act, "reboot") || vt_streq(act, "shutdown")) {
        /* no session manager — schedule the power action to run AFTER
         * our own clean unwind (detached child in its own session
         * survives the compositor exit) */
        char *cmd = vt_strprintf(
            "sleep 1 && (loginctl %s 2>/dev/null || systemctl %s "
            "2>/dev/null)", act, act);
        vt_proc_spawn_detached(cmd);
        vt_free(cmd);
        vt_logw("wayland: panel %s without a session manager — the "
                "action runs right after the compositor exits", act);
    }
    vt_logi("wayland: panel logout — no session manager; clean unwind");
    _hotkey_try(st->backend_self, "Ctrl+Alt+Delete");
}

static void _panel_sync_windows(_wl_state_t *st) {
    vt_wl_panel_win_t wins[16];
    size_t n = 0;
    _wl_surf_t *s;
    wl_list_for_each_reverse(s, &st->surfaces, link) {
        if (!s->mapped || s->is_cursor || !s->toplevel) continue;
        if (s->ws != st->ws_cur) continue;
        wins[n].id = s->toplevel->id;
        wins[n].title = s->toplevel->title ? s->toplevel->title : "";
        wins[n].focused = (st->kbd_focus == s);
        if (++n >= 16) break;
    }
    vt_wl_panel_set_windows(st->panel, wins, n);
}

/* alpha-blend an ARGB sprite over the XRGB framebuffer.
 * stride is in uint32 units and may exceed sw (cursor_img is a
 * 64x64 cell with the image in the top-left corner; client cursor
 * surfaces follow the client's own row padding). */
static void _blend_sprite(_wl_state_t *st, const uint32_t *sprite,
                          int sstride, int sw, int sh, int hx, int hy) {
    int cx = st->cursor_x - hx;
    int cy = st->cursor_y - hy;
    for (int y = 0; y < sh; y++) {
        int dy = cy + y;
        if (dy < 0 || dy >= st->out_h) continue;
        for (int x = 0; x < sw; x++) {
            int dx = cx + x;
            if (dx < 0 || dx >= st->out_w) continue;
            uint32_t sp = sprite[y * sstride + x];
            uint32_t a = sp >> 24;
            if (a == 0) continue;
            uint32_t dp = st->fb[dy * st->out_w + dx];
            uint32_t out;
            if (a == 0xff) {
                out = 0xff000000 | (sp & 0xffffff);
            } else {
                uint32_t rb = ((sp & 0x00ff00ff) * a +
                               (dp & 0x00ff00ff) * (255 - a)) / 255;
                uint32_t g = ((sp & 0x0000ff00) * a +
                              (dp & 0x0000ff00) * (255 - a)) / 255;
                out = 0xff000000 | (rb & 0x00ff00ff) | (g & 0x0000ff00);
            }
            st->fb[dy * st->out_w + dx] = out;
        }
    }
}

static void _paint(void) {
    _wl_state_t *st = _wls;
    if (!st || !st->dirty) return;
    _paint_background(st);
    /* surfaces bottom→top (client windows; cursor surfaces skipped;
     * windows on other workspaces are hidden). Subsurfaces paint
     * directly above their parent, at their relative position. */
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (!s->mapped || !s->pixels || s->is_cursor) continue;
        if (s->parent) continue;            /* painted with the parent */
        if (s->toplevel && s->ws != st->ws_cur) continue;
        int x = s->x, y = s->y;
        for (int sy = 0; sy < s->h; sy++) {
            int dy = y + sy;
            if (dy < 0 || dy >= st->out_h) continue;
            for (int sx = 0; sx < s->w; sx++) {
                int dx = x + sx;
                if (dx < 0 || dx >= st->out_w) continue;
                st->fb[dy * st->out_w + dx] =
                    s->pixels[sy * s->stride + sx];
            }
        }
        /* children above the parent */
        _wl_surf_t *sub;
        wl_list_for_each(sub, &s->subs, sub_link) {
            if (!sub->mapped || !sub->pixels) continue;
            int cx2 = x + sub->dx, cy2 = y + sub->dy;
            for (int sy = 0; sy < sub->h; sy++) {
                int dy = cy2 + sy;
                if (dy < 0 || dy >= st->out_h) continue;
                for (int sx = 0; sx < sub->w; sx++) {
                    int dx = cx2 + sx;
                    if (dx < 0 || dx >= st->out_w) continue;
                    st->fb[dy * st->out_w + dx] =
                        sub->pixels[sy * sub->stride + sx];
                }
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
    /* the REAL compositor panel: bar + open menus, on top of clients */
    if (st->panel) {
        _panel_sync_windows(st);
        vt_wl_panel_paint(st->panel, st->fb, st->out_w, st->out_h);
    }
    /* Software cursor sprite — ALWAYS drawn. The hardware cursor plane
     * is a bonus (used when the driver actually supports it); making the
     * sprite the source of truth guarantees a visible cursor on every
     * GPU, including NVIDIA where drmModeSetCursor can silently fail. */
    if (st->cur_client_set && st->cursor_surf && st->cursor_surf->pixels) {
        _blend_sprite(st, st->cursor_surf->pixels,
                      st->cursor_surf->stride ? st->cursor_surf->stride
                                              : st->cursor_surf->w,
                      st->cursor_surf->w, st->cursor_surf->h,
                      st->cursor_surf->hotspot_x,
                      st->cursor_surf->hotspot_y);
    } else {
        _blend_sprite(st, st->cursor_img, 64,
                      st->cur_img_w, st->cur_img_h,
                      st->cur_img_hx, st->cur_img_hy);
    }
    st->dirty = false;
    st->frame_count++;
}

static void _present(void) {
    _wl_state_t *st = _wls;
    if (!st) return;
    if (st->kms)
        vt_kms_present(st->kms, st->fb, st->out_w, st->out_h);
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

static int _wl_close_window(vt_backend_t *self, uint64_t id) {
    _wl_state_t *st = self->priv;
    if (!st) return -1;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel->id == id) {
            if (s->toplevel->res)
                xdg_toplevel_send_close(s->toplevel->res);
            return 0;
        }
    }
    return -1;
}

static void _wl_set_user_data(vt_backend_t *self, void *ud) {
    if (!self->priv) return;
    ((_wl_state_t *)self->priv)->user_data = ud;
}
static void *_wl_get_user_data(vt_backend_t *self) {
    return self->priv ? ((_wl_state_t *)self->priv)->user_data : NULL;
}

static vt_kms_scanout_t _scanout_from_env(void) {
    const char *s = getenv("VANTAGE_WAYLAND_SCANOUT");
    if (!s || !*s) return VT_KMS_SCANOUT_AUTO;
    if (vt_strcaseeq(s, "gbm")) return VT_KMS_SCANOUT_GBM;
    if (vt_strcaseeq(s, "dumb")) return VT_KMS_SCANOUT_DUMB;
    return VT_KMS_SCANOUT_AUTO;
}

static int _wl_init(vt_backend_t *self) {
    if (_wls) {
        self->priv = _wls;
        return 0;
    }
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (wd && *wd) {
        vt_logi("wayland: WAYLAND_DISPLAY is set ('%s') — this process "
                "would be a *client* of another compositor; refusing "
                "to nest", wd);
        return -1;
    }

    /* Tests/CI knob: never acquire the host's real seat/VT/GPU, so the
     * 15-stage trace is identical on every machine. The real path is
     * exercised by a genuine TTY launch (docs/wayland-backend.md). */
    bool force_headless = _env_flag("VANTAGE_WAYLAND_FORCE_HEADLESS");

    _wl_state_t *st = vt_malloc0(sizeof(*st));
    self->priv = st;
    st->backend_self = self;
    st->keymap_fd = -1;
    st->cursor_x = 100;
    st->cursor_y = 100;
    st->display = wl_display_create();
    if (!st->display) { vt_free(st); return -1; }
    st->loop = wl_display_get_event_loop(st->display);
    wl_list_init(&st->surfaces);
    wl_list_init(&st->ptr_reses);
    wl_list_init(&st->kbd_reses);
    wl_list_init(&st->data_devs);

    /* ---- stage 1/15: session (XDG_RUNTIME_DIR) -------------------- */
    _stage_begin(0, "XDG_RUNTIME_DIR preparation");
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) {
        char fb_dir[64];
        snprintf(fb_dir, sizeof(fb_dir), "/tmp/vantage-%d", (int)getuid());
        if (mkdir(fb_dir, 0700) == 0 || errno == EEXIST) {
            chmod(fb_dir, 0700);
            setenv("XDG_RUNTIME_DIR", fb_dir, 1);
            rd = fb_dir;
            _stage_ok(0, "XDG_RUNTIME_DIR was unset — created %s "
                       "(no session manager ran)", fb_dir);
        } else {
            _stage_fail(0, "XDG_RUNTIME_DIR unset and %s cannot be "
                          "created: %s", fb_dir, strerror(errno));
            wl_display_destroy(st->display);
            vt_free(st);
            return -1;
        }
    } else {
        _stage_ok(0, "XDG_RUNTIME_DIR=%s", rd);
    }

    /* ---- stage 2/15: seat ------------------------------------------ */
    _stage_begin(1, "libseat (logind → elogind-compatible → seatd) or direct VT");
    if (force_headless) {
        _stage_skip(1, "forced headless (VANTAGE_WAYLAND_FORCE_HEADLESS) — "
                       "no seat/VT is acquired, the host session is not "
                       "touched");
    } else {
        st->seat = vt_seat_acquire();
        if (st->seat) {
            vt_seat_set_notify(st->seat, _seat_notify, st);
            _stage_ok(1, "%s, seat '%s', VT %d",
                      vt_seat_mode_str(st->seat), vt_seat_name(st->seat),
                      vt_seat_vt(st->seat));
        } else {
            _stage_skip(1, "no session manager and no usable TTY — headless "
                           "operation follows");
        }
    }

    /* ---- stage 3/15: vt -------------------------------------------- */
    _stage_begin(2, "VT acquisition/activation");
    if (st->seat) {
        if (vt_seat_is_active(st->seat)) {
            _stage_ok(2, "VT %d already active (granted with the session)",
                      vt_seat_vt(st->seat));
        } else if (vt_seat_vt_activate(st->seat, 2000) == 0) {
            _stage_ok(2, "switched to VT %d", vt_seat_vt(st->seat));
        } else {
            _stage_fail(2, "could not activate VT %d",
                        vt_seat_vt(st->seat));
            vt_seat_release(st->seat);
            st->seat = NULL;
        }
    } else {
        _stage_skip(2, "no seat — headless operation follows");
    }

    /* ---- stage 4-11/15: drm → scanout ------------------------------- */
    bool require_kms = getenv("VANTAGE_WAYLAND_REQUIRE_KMS") &&
                       *getenv("VANTAGE_WAYLAND_REQUIRE_KMS") == '1';
    const char *picked_path = "(none)";
    _stage_begin(3, "/dev/dri card discovery");
    if (force_headless && require_kms) {
        _stage_fail(3, "VANTAGE_WAYLAND_FORCE_HEADLESS and "
                       "VANTAGE_WAYLAND_REQUIRE_KMS are both set — pick one");
        goto fail_no_kms;
    }
    vt_kms_card_t cards[8];
    int nc = force_headless ? 0 : vt_kms_discover_cards(cards, 8);
    if (force_headless) {
        _stage_skip(3, "forced headless (VANTAGE_WAYLAND_FORCE_HEADLESS) — "
                       "/dev/dri is not touched");
        st->headless = true;
    } else if (nc == 0) {
        if (require_kms) {
            _stage_fail(3, "no DRM cards under /dev/dri and "
                           "VANTAGE_WAYLAND_REQUIRE_KMS=1");
            goto fail_no_kms;
        }
        _stage_skip(3, "no DRM cards under /dev/dri — headless "
                       "framebuffer follows");
        st->headless = true;
    } else {
        /* pick the first card with a connected output (else card 0) */
        int pick = 0;
        for (int i = 0; i < nc; i++)
            if (cards[i].has_connected) { pick = i; break; }
        picked_path = cards[pick].path;
        vt_kms_status_t kst = VT_KMS_OK;
        vt_logi("wayland: using %s (driver '%s')", cards[pick].path,
                cards[pick].driver);
        st->kms = vt_kms_open(cards[pick].path, st->seat,
                              _scanout_from_env(), &kst);
        if (!st->kms) {
            if (require_kms) {
                _stage_fail(3, "%s: %s (VANTAGE_WAYLAND_REQUIRE_KMS=1)",
                            cards[pick].path, vt_kms_status_str(kst));
                goto fail_no_kms;
            }
            _stage_fail(3, "%s: %s — headless framebuffer follows",
                        cards[pick].path, vt_kms_status_str(kst));
            st->headless = true;
        }
    }

    if (st->kms) {
        _stage_ok(3, "%s opened%s", picked_path,
                  st->seat ? " through the seat" : " directly");
        _stage_ok(4, "DRM master acquired on the card");
        const char *sstr = vt_kms_scanout_str(st->kms);
        if (vt_streq(sstr, "gbm"))
            _stage_ok(5, "GBM device on the card");
        else
            _stage_skip(5, "gbm unusable — dumb scanout buffers");
        const char *r = vt_kms_renderer(st->kms);
        if (strstr(r, "EGL"))
            _stage_ok(6, "EGL display + GLES context initialized");
        else
            _stage_skip(6, "no EGL display (CPU path)");
        _stage_ok(7, "%s", r);
        _stage_ok(8, "%d output(s): %s %dx%d@%d",
                  vt_kms_output_count(st->kms),
                  vt_kms_out_name(st->kms, 0),
                  vt_kms_out_width(st->kms, 0),
                  vt_kms_out_height(st->kms, 0),
                  vt_kms_out_refresh(st->kms, 0));
    } else {
        const char *why = "card unusable (see [kms] logs above)";
        if (force_headless)
            why = "forced headless (VANTAGE_WAYLAND_FORCE_HEADLESS)";
        else if (nc == 0)
            why = "no DRM cards";
        _stage_skip(4, "%s (headless)", why);
        _stage_skip(5, "%s (headless)", why);
        _stage_skip(6, "%s (headless)", why);
        _stage_skip(7, "%s (headless)", why);
        _stage_skip(8, "%s (headless)", why);
    }

    /* output geometry + framebuffer */
    if (st->kms) {
        st->out_w = vt_kms_out_width(st->kms, 0);
        st->out_h = vt_kms_out_height(st->kms, 0);
    } else {
        st->out_w = 1024;
        st->out_h = 768;
        st->headless = true;
    }
    st->fb = vt_malloc0(sizeof(uint32_t) * (size_t)st->out_w * st->out_h);
    st->cursor_x = st->out_w / 2;
    st->cursor_y = st->out_h / 2;

    /* ---- stage 10/15: crtc (first mode set) ------------------------ */
    _stage_begin(9, "CRTC mode set");
    if (st->kms) {
        _paint_background(st);
        vt_kms_prime(st->kms, st->fb, st->out_w, st->out_h);
        if (vt_kms_start(st->kms, _kms_first_scanout, st) == 0) {
            _stage_ok(9, "%s: mode %dx%d@%d, scanout live",
                      vt_kms_out_name(st->kms, 0), st->out_w, st->out_h,
                      vt_kms_out_refresh(st->kms, 0));
        } else {
            _stage_fail(9, "drmModeSetCrtc failed (see [kms] logs)");
            goto fail_no_kms;
        }
    } else {
        _stage_skip(9, "headless — no CRTC to mode-set");
    }

    /* ---- stage 11/15: scanout buffers/flip chain -------------------- */
    _stage_begin(10, "scanout buffer chain");
    if (st->kms)
        _stage_ok(10, "%s ping-pong buffers, async page flips, %s cursor",
                  vt_kms_scanout_str(st->kms),
                  vt_kms_hw_cursor(st->kms) ? "hardware" : "software");
    else
        _stage_skip(10, "headless — in-memory framebuffer only");

    /* ---- stage 12/15: input ----------------------------------------- */
    _stage_begin(11, "libinput (udev) + xkbcommon");
    if (!st->headless && st->seat) {
        bool li_ok = _input_init(st);
        bool xkb_ok = _xkb_init(st);
        if (li_ok && xkb_ok) {
            _stage_ok(11, "%zu device(s), xkb keymap ready",
                      self->inputs.size);
        } else if (li_ok) {
            _stage_ok(11, "%zu device(s); xkb keymap unavailable — "
                          "clients get an empty keymap", self->inputs.size);
        } else {
            _stage_fail(11, "libinput unavailable — no real input "
                            "(keyboard/pointer) will be received");
        }
    } else {
        _stage_skip(11, "headless — no seat to open input devices "
                        "through");
        vt_input_dev_t k = { .name = vt_strdup("wl-keyboard"), .id = 0,
                             .type = 0, .active = true };
        vt_input_dev_t p = { .name = vt_strdup("wl-pointer"), .id = 1,
                             .type = 1, .active = true };
        vt_vec_push(&self->inputs, &k);
        vt_vec_push(&self->inputs, &p);
    }
    _cursor_init(st);

    /* ---- stage 13/15: wayland socket -------------------------------- */
    _stage_begin(12, "wl_display socket");
    if (wl_display_init_shm(st->display) < 0) {
        _stage_fail(12, "wl_display_init_shm failed");
        goto fail_no_kms;
    }
    st->compositor_g = wl_global_create(st->display,
        &wl_compositor_interface, 3, NULL, _bind_compositor);
    st->seat_g = wl_global_create(st->display, &wl_seat_interface, 5, NULL,
                                  _bind_seat);
    st->output_g = wl_global_create(st->display, &wl_output_interface, 3,
                                    NULL, _bind_output);
    st->xdg_g = wl_global_create(st->display, &xdg_wm_base_interface, 2,
                                 NULL, _bind_xdg_wm_base);
    st->subcomp_g = wl_global_create(st->display,
                                     &wl_subcompositor_interface, 1, NULL,
                                     _bind_subcompositor);
    st->ddm_g = wl_global_create(st->display,
                                 &wl_data_device_manager_interface, 3, NULL,
                                 _bind_ddm);
    if (!st->subcomp_g || !st->ddm_g)
        vt_logw("wayland: wl_subcompositor/wl_data_device_manager "
                "global creation failed");
    if (!st->compositor_g || !st->seat_g || !st->output_g || !st->xdg_g) {
        _stage_fail(12, "failed to create globals");
        goto fail_no_kms;
    }
    st->client_created.notify = _client_destroyed;
    wl_display_add_client_created_listener(st->display, &st->client_created);
    const char *sock = wl_display_add_socket_auto(st->display);
    if (!sock) {
        _stage_fail(12, "wl_display_add_socket_auto failed (bad "
                        "XDG_RUNTIME_DIR?)");
        goto fail_no_kms;
    }
    st->socket_name = vt_strdup(sock);
    setenv("WAYLAND_DISPLAY", st->socket_name, 1);
    _stage_ok(12, "WAYLAND_DISPLAY=%s (%s/%s)", st->socket_name,
              getenv("XDG_RUNTIME_DIR"), st->socket_name);
    st->src = wl_event_loop_add_fd(st->loop,
                                   wl_event_loop_get_fd(st->loop),
                                   WL_EVENT_READABLE, _loop_fd, st->display);
    if (st->seat) {
        int sfd = vt_seat_fd(st->seat);
        if (sfd >= 0)
            st->seat_src = wl_event_loop_add_fd(st->loop, sfd,
                                                WL_EVENT_READABLE,
                                                _seat_fd_cb, st);
    }
    if (st->kms) {
        int dfd = vt_kms_fd(st->kms);
        if (dfd >= 0)
            st->drm_src = wl_event_loop_add_fd(st->loop, dfd,
                                               WL_EVENT_READABLE,
                                               _drm_fd_cb, st);
    }

    /* ---- stage 14/15: compositor READY ------------------------------ */
    if (st->headless) {
        vt_logi("[wayland] NOTICE: HEADLESS mode — no KMS output was "
                "acquired; pixels go to an in-memory framebuffer "
                "(tests/CI only, nothing appears on any screen). Set "
                "VANTAGE_WAYLAND_REQUIRE_KMS=1 to turn this into a hard "
                "failure.");
    }
    vt_logi("[wayland] compositor: READY");
    if (st->kms)
        vt_logi("wayland: compositor on WAYLAND_DISPLAY=%s — real KMS "
                "output %s (%dx%d, %s cursor, %s)",
                st->socket_name, vt_kms_out_name(st->kms, 0),
                st->out_w, st->out_h,
                vt_kms_hw_cursor(st->kms) ? "hardware" : "software",
                vt_kms_renderer(st->kms));
    else
        vt_logi("wayland: compositor on WAYLAND_DISPLAY=%s (%dx%d headless "
                "framebuffer; SIGUSR1 → /tmp/vantage-wayland.ppm)",
                st->socket_name, st->out_w, st->out_h);

    /* outputs model */
    vt_output_t o = {0};
    if (st->kms) {
        o.name = vt_strdup(vt_kms_out_name(st->kms, 0));
        o.refresh_hz = vt_kms_out_refresh(st->kms, 0);
    } else {
        o.name = vt_strdup("WL-1");
        o.refresh_hz = 60;
    }
    o.id = 0;
    o.w = st->out_w;
    o.h = st->out_h;
    o.scale = 1;
    o.connected = true;
    o.enabled = true;
    o.primary = true;
    vt_vec_push(&self->outputs, &o);

    signal(SIGUSR1, _screenshot);
    _wls = st;
    st->dirty = true;

    /* ---- the REAL compositor panel (no placeholder blocks) -------- */
    st->ws_count = 4;
    st->ws_cur = 0;
    st->panel = vt_wl_panel_create(st->out_w, 32);
    vt_wl_panel_set_workspaces(st->panel, st->ws_count, st->ws_cur);
    {
        vt_wl_panel_cbs_t cbs = {
            .focus_window = _panel_cb_focus,
            .close_window = _panel_cb_close,
            .switch_ws = _panel_cb_ws,
            .logout = _panel_cb_logout,
        };
        vt_wl_panel_set_callbacks(st->panel, &cbs, st);
    }

    /* ---- stage 15/15: desktop (first frame) -------------------------- */
    _stage_begin(14, "first frame");
    _paint();
    _present();
    _stage_ok(14, "desktop painted %dx%d (%s), cursor software sprite, "
              "compositor panel (Vantage menu, window list, workspaces, "
              "clock, session menu)",
              st->out_w, st->out_h,
              st->kms ? vt_kms_out_name(st->kms, 0) : "headless");
    vt_logi("[wayland] desktop: ready");
    return 0;

fail_no_kms:
    if (st->kms) vt_kms_close(st->kms);
    if (st->seat) vt_seat_release(st->seat);
    if (st->display) wl_display_destroy(st->display);
    vt_free(st->fb);
    vt_free(st);
    self->priv = NULL;
    return -1;
}

static void _wl_fini(vt_backend_t *self) {
    if (self->priv != _wls || !_wls) return;
    _wl_state_t *st = _wls;
    vt_logi("wayland: shutting down the compositor");
    if (st->panel) {
        vt_wl_panel_destroy(st->panel);
        st->panel = NULL;
    }
    _input_fini(st);
    _xkb_fini(st);
    if (st->drm_src) { wl_event_source_remove(st->drm_src); st->drm_src = NULL; }
    if (st->seat_src) { wl_event_source_remove(st->seat_src); st->seat_src = NULL; }
    /* KMS close restores the saved CRTC (before we drop master/seat) */
    if (st->kms) {
        vt_kms_close(st->kms);
        st->kms = NULL;
        vt_logi("wayland: KMS closed — original CRTC restored");
    }
    if (st->seat) {
        vt_seat_release(st->seat);   /* VT text mode + VT_AUTO + libseat */
        st->seat = NULL;
        vt_logi("wayland: seat released — VT returned to text mode");
    }
    if (st->src) wl_event_source_remove(st->src);
    wl_display_destroy(st->display);
    vt_free(st->fb);
    vt_free(st->socket_name);
    vt_free(st);
    _wls = NULL;
    vt_logi("[wayland] compositor: exited cleanly");
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
    _present();
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
/* ---- headless test-input hook -----------------------------------
 * Exercises the REAL input pipeline (the exact handlers libinput
 * feeds) so integration harnesses can drive the pointer without any
 * input device. Nothing is faked: motion/press/release/axis run the
 * same compositor code as hardware events. */
static int _wl_test_input(vt_backend_t *self, const char *spec) {
    _wl_state_t *st = (self && self->priv && self->priv == (void *)_wls)
                      ? _wls : NULL;
    if (!st || !spec) return -1;
    int x, y, b, d;
    if (sscanf(spec, "motion x=%d y=%d", &x, &y) == 2) {
        _pointer_motion(st, (double)(x - st->cursor_x),
                        (double)(y - st->cursor_y));
        return 0;
    }
    if (sscanf(spec, "press b=%d", &b) == 1 && b >= 1 && b <= 3) {
        _pointer_button(st, b == 2 ? 0x111 : b == 3 ? 0x112 : 0x110,
                        true);
        return 0;
    }
    if (sscanf(spec, "release b=%d", &b) == 1 && b >= 1 && b <= 3) {
        _pointer_button(st, b == 2 ? 0x111 : b == 3 ? 0x112 : 0x110,
                        false);
        return 0;
    }
    if (sscanf(spec, "axis d=%d", &d) == 1 && d != 0) {
        _pointer_axis(st, (double)(d * 15));
        return 0;
    }
    vt_logw("wayland: test-input: unrecognized spec '%s'", spec);
    return -1;
}

static bool _wl_supports_compositing(vt_backend_t *self) { (void)self; return true; }
static bool _wl_can_swap_buffers(vt_backend_t *self) { (void)self; return true; }

vt_backend_t *_vt_backend_wayland_new(void) {
    vt_backend_t *b = vt_malloc0(sizeof(*b));
    /* Launched applications are children of the compositor; auto-reap
     * them so long sessions never accumulate zombies. */
    signal(SIGCHLD, SIG_IGN);
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
    b->close_window = _wl_close_window;
    b->test_input = _wl_test_input;
    b->set_user_data = _wl_set_user_data;
    b->get_user_data = _wl_get_user_data;
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
