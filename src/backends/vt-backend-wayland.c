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
#include <stddef.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-seat.h>
#include <vantage/vt-kms.h>
#include <vantage/vt-ipc.h>
#include <vantage/vt-wallpaper.h>

#if defined(VT_HAVE_WAYLAND)

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

#if defined(VT_HAVE_FREETYPE)
#include <ft2build.h>
#include <freetype/freetype.h>
#include <fontconfig/fontconfig.h>
#endif

/* shared types + protocol-module seams (layer-shell, Xwayland) */
#include "vt-wl-internal.h"

/* forward-declared MODULE seam: the per-surface xdg-shell data.
 * Defined HERE (not in the xdg-shell section) because the surface
 * destructor must orphan its back-pointer at surface death — at
 * client teardown libwayland destroys resources in creation order,
 * so the xdg_surface handler runs AFTER the surface is freed. */
typedef struct _xdg_surf_data {
    _wl_surf_t *surf;
} _xdg_surf_data_t;

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

_wl_state_t *_wls = NULL;


/* used by the WM host (vantage-wm) to route compositor hotkeys */
static bool _hotkey_try(vt_backend_t *self, const char *combo);
void _kbd_enter_focus(_wl_state_t *st, _wl_surf_t *s);
static void _decor_notify(_wl_surf_t *s);
static void _decor_orphan(_wl_surf_t *s);
static void _cursor_apply_hw(_wl_state_t *st);
void _focus_top_on_ws(_wl_state_t *st);
static bool _hotkey_try(vt_backend_t *self, const char *combo) {
    if (self && self->hotkey) return self->hotkey(self, combo);
    return false;
}

/* ------------------------------------------------- window event emission */
static void _surface_output_enter(_wl_surf_t *s);
static void _surface_output_leave(_wl_surf_t *s);

void _emit_win(_wl_state_t *st, vt_backend_wl_event_kind_t kind,
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
        /* report the WINDOW (content) rect, not the raw buffer: CSD
         * clients commit buffers that include shadow margins outside
         * set_window_geometry — announcing the raw rect made the pager
         * and taskbar over-report every CSD window by its shadow
         * (428x329 for a 400x300 GTK window). */
        int cx, cy, cw, ch;
        _win_content_rect(s, &cx, &cy, &cw, &ch);
        ev.x = cx; ev.y = cy;
        ev.w = cw > 0 ? cw : 0;
        ev.h = ch > 0 ? ch : 0;
    } else {
        /* XWayland windows emit events before their surface pairs —
         * the toplevel's geometry mirror (from the X window) keeps the
         * WM model honest instead of announcing a 0x0 window at +0+0
         * ("a tiny window in the corner" in the pager). */
        ev.x = t->x; ev.y = t->y;
        ev.w = t->w > 0 ? t->w : 0;
        ev.h = t->h > 0 ? t->h : 0;
    }
    ev.focused = t->activated;
    ev.maximized = t->maximized;
    ev.fullscreen = t->fullscreen;
    ev.minimized = t->minimized;
    vt_backend_t *b = NULL;
    /* emit through the backend's sink list: find backend from st */
    if (_wls && _wls->backend_self) b = _wls->backend_self;
    if (b) vt_backend_emit_event(b, &ev);
}

/* The WINDOW rect (content rect) in screen coordinates — see the
 * header. Buffer-bounded: win_gw/gh are clamped to the committed
 * buffer so a geometry larger than the buffer cannot make hit-tests
 * or painting read outside the client content. */
void _win_content_rect(const _wl_surf_t *s, int *cx, int *cy,
                        int *cw, int *ch) {
    *cx = s->x; *cy = s->y; *cw = s->w; *ch = s->h;
    if (s->have_win_geo) {
        int gx = s->win_gx, gy = s->win_gy;
        int gw = s->win_gw, gh = s->win_gh;
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx > s->w) gx = s->w;
        if (gy > s->h) gy = s->h;
        if (gw > s->w - gx) gw = s->w - gx;
        if (gh > s->h - gy) gh = s->h - gy;
        *cx = s->x + gx;
        *cy = s->y + gy;
        *cw = gw > 0 ? gw : 0;
        *ch = gh > 0 ? gh : 0;
    }
}

/* pointer-input containment: the client's input region if it set
 * one (CSD toolkits exclude shadow margins), else the window/content
 * rect — NEVER the raw buffer rect (that delivered clicks and resize
 * grabs into the transparent shadow band around CSD windows, which
 * read as "a black region that behaves like a solid part of the
 * window"). */
/* content size helpers (set_window_geometry aware) — used by the
 * interactive resize seeding (defined near _pointer_button) */
static inline int _surf_cw(const _wl_surf_t *s);
static inline int _surf_ch(const _wl_surf_t *s);

/* exclusive activation bookkeeping (defined after _click_to_focus) */
static void _activate_toplevel(_wl_state_t *st, _xdg_toplevel_t *t);

bool _surf_input_contains(const _wl_surf_t *s, int x, int y) {
    if (!s || !s->mapped) return false;
    int sx = s->x, sy = s->y;
    if (s->popup && s->popup->parent) {
        sx = s->popup->parent->x + s->popup->rel_x;
        sy = s->popup->parent->y + s->popup->rel_y;
    }
    int lx = x - sx, ly = y - sy;
    if (s->input_set) {
        bool inside = false;
        for (int i = 0; i < s->n_in_adds; i++) {
            const struct _vt_rect *r = &s->in_adds[i];
            if (lx >= r->x && lx < r->x + r->w &&
                ly >= r->y && ly < r->y + r->h) { inside = true; break; }
        }
        if (!inside) return false;
        for (int i = 0; i < s->n_in_subs; i++) {
            const struct _vt_rect *r = &s->in_subs[i];
            if (lx >= r->x && lx < r->x + r->w &&
                ly >= r->y && ly < r->y + r->h) return false;
        }
        return true;
    }
    int cx, cy, cw, ch;
    if (s->popup && s->popup->parent) {
        /* popup: _win_content_rect is buffer-relative — re-root it at
         * the popup's on-screen origin (parent + rel) */
        int bx, by, bw, bh;
        _win_content_rect(s, &bx, &by, &bw, &bh);
        cx = bx - s->x + sx;
        cy = by - s->y + sy;
        cw = bw;
        ch = bh;
    } else {
        _win_content_rect(s, &cx, &cy, &cw, &ch);
    }
    return x >= cx && x < cx + cw && y >= cy && y < cy + ch;
}

/* ---------------------------------------------------------- compositor */
static void _surf_destroy(struct wl_client *cli, struct wl_resource *res);

static void _surf_attach(struct wl_client *cli, struct wl_resource *res,
                         struct wl_resource *buf_res, int32_t dx, int32_t dy) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    s->buf_res = buf_res;
    s->attach_pending = true;
    s->dx = dx;
    s->dy = dy;
}

static void _surf_damage(struct wl_client *cli, struct wl_resource *res,
                         int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli; (void)res; (void)x; (void)y; (void)w; (void)h;
    /* coarse damage: whole-surface repaint (correct, simple) */
    if (_wls) _wls->dirty = true;
}

/* --- ultra-cheap frame tracing (SHARED-MMAP ring, ZERO syscalls) -------
 * ANY syscall (even a 2-byte write) in the commit/paint path perturbs
 * timing enough to HIDE the frame-callback freeze. This variant writes
 * plain bytes into an mmap'd file: no syscall at all per event, so the
 * bug stays visible while we watch what actually happens.
 * Enabled with VANTAGE_FRAME_TRACE=/path; off by default, free when off. */
static char *_ftrace_map = NULL;
static size_t _ftrace_pos = 0, _ftrace_cap = 0;
static void _ftrace_init(void) {
    const char *p = getenv("VANTAGE_FRAME_TRACE");
    if (!p || !*p) return;
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (ftruncate(fd, 1 << 20) < 0) { close(fd); return; }
    _ftrace_map = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    _ftrace_cap = _ftrace_map != MAP_FAILED ? (1 << 20) : 0;
    if (_ftrace_map == MAP_FAILED) _ftrace_map = NULL;
    close(fd);
}
static void _ftrace(char c) {
    static int _inited = 0;
    if (!_inited) { _inited = 1; _ftrace_init(); }
    if (_ftrace_map && _ftrace_pos + 1 < _ftrace_cap) {
        _ftrace_map[_ftrace_pos++] = c;
        _ftrace_map[_ftrace_pos] = '\n';
    }
}

/* Fire all pending frame callbacks with the current frame time.
 *
 * ARCHITECTURE (the browser-stuck-on-one-frame fix): callbacks live on
 * a GLOBAL list, not per-surface — a surface that nil-commits (unmaps)
 * or dies with a pending callback must never strand the client waiting
 * for a done that can no longer be sent (toolkits pace their whole
 * render loop off wl_surface.frame; one lost done freezes them after a
 * single frame — reproduced with weston-simple-damage and, on real
 * hardware, with the browser).
 *
 * They fire EVERY loop tick (the loop is the frame clock), not only on
 * dirty paints: a client that committed while the compositor had
 * nothing to repaint still gets its done on the next tick, and the
 * done reaches the wire in the SAME iteration because _wl_dispatch
 * flushes clients AFTER the callbacks are marshaled. The old order
 * (flush → paint → done) left every fired callback sitting in the
 * connection output buffer for a full 20ms iteration — and under a
 * timing race the buffered event could be lost outright, freezing the
 * client forever (Send-Q empty, client in poll(-1), compositor looping
 * idle). */
static void _fire_frame_callbacks(_wl_state_t *st) {
    if (!st) return;
    /* FIRST: destroy last tick's FIRED callback resources. Deferring
     * the destruction one full loop tick keeps done and delete_id out
     * of the same client dispatch batch — libwayland dispatches the
     * display queue (delete_id) before the default queue (done), so a
     * same-batch delete_id finalizes the callback proxy and the done
     * event never reaches the client's listener. weston's toy toolkit
     * destroys the callback INSIDE its done handler; never seeing the
     * done killed its animation after exactly one frame. */
    while (!wl_list_empty(&st->retired_cbs)) {
        _cb_node_t *rn = (_cb_node_t *)(void *)(
            (char *)st->retired_cbs.next - offsetof(_cb_node_t, link));
        wl_resource_destroy(rn->cb);   /* destructor unlinks + frees */
    }
    if (wl_list_empty(&st->frame_cbs)) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000 +
                      (uint64_t)ts.tv_nsec / 1000;
    /* pace: this IS the compositor's frame clock. Clients pace their
     * whole render loop off wl_surface.frame — firing as fast as their
     * commits arrive lets animation clients busy-spin (measured 1100
     * redraws/s, a full core burned, before this cap). ~60Hz floor,
     * tunable via VANTAGE_FRAME_INTERVAL_US. */
    uint64_t min_us = 15000;
    const char *p = getenv("VANTAGE_FRAME_INTERVAL_US");
    if (p && *p) {
        long v = atol(p);
        if (v > 0 && v < 1000000) min_us = (uint64_t)v;
    }
    if (st->last_fire_us && now_us - st->last_fire_us < min_us)
        return;
    st->last_fire_us = now_us;
    uint32_t msec = (uint32_t)(now_us / 1000);
    _cb_node_t *n, *tmp;
    wl_list_for_each_safe(n, tmp, &st->frame_cbs, link) {
        _ftrace('F');
        wl_callback_send_done(n->cb, msec);
        /* RETIRE instead of destroying: the resource dies next tick
         * (see the head of this function). The node keeps the
         * resource destructor as its cleanup path either way. */
        wl_list_remove(&n->link);
        wl_list_insert(st->retired_cbs.prev, &n->link);
    }
}

/* wl_callback resource destructor: owns the pending-node cleanup for
 * BOTH the normal fire path and client death (libwayland destroys the
 * resources of a disconnecting client — the nodes must not outlive
 * them, or the next fire would marshal into freed memory). */
static void _cb_res_destroy(struct wl_resource *res) {
    _cb_node_t *n = wl_resource_get_user_data(res);
    if (!n) return;
    wl_list_remove(&n->link);
    vt_free(n);
}

static void _surf_frame(struct wl_client *cli, struct wl_resource *res,
                        uint32_t callback_id) {
    _wl_surf_t *s = wl_resource_get_user_data(res);
    _wl_state_t *st = _wls;
    if (!st) return;
    (void)s;
    struct wl_resource *cb = wl_resource_create(cli, &wl_callback_interface,
                                                 1, callback_id);
    if (!cb) { wl_client_post_no_memory(cli); return; }
    _cb_node_t *node = vt_malloc0(sizeof(*node));
    node->cb = cb;
    wl_list_insert(st->frame_cbs.prev, &node->link);
    /* event-only resource: no request implementation, the node is the
     * user data, the destructor owns the unlink */
    wl_resource_set_implementation(cb, NULL, node, _cb_res_destroy);
}

static void _surf_commit(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    _wl_state_t *st = _wls;
    _ftrace('C');
    if (!st) return;

    if (s->buf_res) {
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buf_res);
        if (shm) {
            int32_t w = wl_shm_buffer_get_width(shm);
            int32_t h = wl_shm_buffer_get_height(shm);
            int32_t bytes = wl_shm_buffer_get_stride(shm);
            int32_t stride = bytes / 4 >= w ? bytes / 4 : w;
            if (w > 0 && h > 0 && stride > 0) {
                /* COPY the buffer and END the shm access immediately:
                 *
                 * 1. libwayland asserts (wl_shm_buffer_begin_access)
                 *    when two different pools are opened at once — the
                 *    old never-ended access ABORTED the compositor the
                 *    moment a real toolkit committed its second pool.
                 *    Every app died with "Broken pipe" — the whole
                 *    Wayland session crashed. This copy fixes that.
                 * 2. Keeping a raw pointer into the pool dangles when
                 *    the client destroys the buffer/pool after the next
                 *    attach (toolkits cycle buffers constantly).
                 * 3. With our own copy the buffer can be RELEASED right
                 *    away, so clients never stall waiting for
                 *    wl_buffer.release (they would after two commits). */
                size_t need = (size_t)stride * (size_t)h;
                if (!s->own || s->own_cap < need) {
                    vt_free(s->own);
                    s->own_cap = need + need / 2;
                    s->own = vt_malloc(sizeof(uint32_t) * s->own_cap);
                }
                if (s->own) {
                    wl_shm_buffer_begin_access(shm);
                    memcpy(s->own, wl_shm_buffer_get_data(shm),
                           need * sizeof(uint32_t));
                    wl_shm_buffer_end_access(shm);
                    s->pixels = s->own;
                    s->w = w;
                    s->h = h;
                    s->buf_w = w;
                    s->buf_h = h;
                    s->stride = stride;
                }
            }
            wl_buffer_send_release(s->buf_res);
        } else {
            /* Non-shm buffer: a GPU client's dma-buf (zwp_linux_dmabuf_v1)
             * — import ladder + readback lives in vt-wl-dmabuf.c. Same
             * contract as the shm path: WE own the copied pixels, the
             * buffer is released right away, the client never stalls.
             * GPU apps render on the REAL driver; we read the result. */
            int32_t dw = 0, dh = 0;
            bool ok = _dmabuf_commit_pixels(s->buf_res, NULL, 0, &dw, &dh);
            if (ok && dw > 0 && dh > 0) {
                size_t need = (size_t)dw * (size_t)dh;
                if (!s->own || s->own_cap < need) {
                    vt_free(s->own);
                    s->own_cap = need + need / 2;
                    s->own = vt_malloc(sizeof(uint32_t) * s->own_cap);
                }
                if (s->own &&
                    _dmabuf_commit_pixels(s->buf_res, s->own, dw,
                                          &dw, &dh)) {
                    s->pixels = s->own;
                    s->w = dw;
                    s->h = dh;
                    s->buf_w = dw;
                    s->buf_h = dh;
                    s->stride = dw;
                } else {
                    ok = false;
                }
            }
            if (!ok) {
                static bool warned_nonshm = false;
                if (!warned_nonshm) {
                    warned_nonshm = true;
                    vt_logw("wayland: client committed a buffer we cannot "
                            "import (non-shm, import failed) — it will "
                            "fall back to wl_shm");
                }
            }
            /* release in ALL cases: withholding it would deadlock the
             * client's buffer cycling (two commits and it stalls) */
            wl_buffer_send_release(s->buf_res);
        }
        s->buf_res = NULL;
    } else if (s->attach_pending) {
        /* attach(NULL) + commit: the surface enters the EMPTY state
         * (this is how GTK hides a popover: xdg_popup.destroy, then a
         * nil commit). The stale size must be cleared or the next
         * commit logic would re-map the surface with its OLD pixels —
         * closed menus would stay painted on screen forever.
         * (A BARE commit with no attach request since the last one is
         * a content-preserving sync commit — GDK frame cycles use it —
         * and must NOT touch the surface state.) */
        s->w = 0;
        s->h = 0;
        s->buf_w = 0;
        s->buf_h = 0;
        s->pixels = NULL;
        if (s->mapped) {
            s->mapped = false;
            wl_list_remove(&s->link);
            _surface_output_leave(s);
            if (st) st->dirty = true;
            if (st && st->ptr_focus == s) st->ptr_focus = NULL;
            if (st && st->kbd_focus == s) st->kbd_focus = NULL;
            /* A TOPLEVEL hiding itself with a nil commit IS a window
             * close from the WM's point of view — and it is the close
             * path real toolkits take after xdg_toplevel.send_close:
             * GTK destroys the window (nil commit unmaps it) and only
             * then destroys the wl_surface, whose destructor now sees
             * mapped == false and stays silent. Without this emit the
             * WM model kept the window forever: the "ghost window"
             * (visible in the pager, dead to every interaction) after
             * closing an app. Popups hide the same way but are not WM
             * windows — they must not emit. */
            if (s->toplevel && st) {
                if (st->focused_toplevel == s->toplevel)
                    st->focused_toplevel = NULL;
                if (st->op_surf == s) {
                    st->op_active = false;
                    st->op_surf = NULL;
                }
                _emit_win(st, VT_BACKEND_WL_EVENT_WIN_UNMAP, s->toplevel);
            }
        }
    }
    s->attach_pending = false;

    /* buffer/offset bookkeeping before mapping math */
    if (s->dx || s->dy) {
        if (s->mapped) {
            if (s->xwl)
                vt_logd("wayland: xwl surface %p attach-delta %d,%d: "
                        "(%d,%d) -> (%d,%d)", (void*)s, s->dx, s->dy,
                        s->x, s->y, s->x - s->dx, s->y - s->dy);
            s->x -= s->dx;
            s->y -= s->dy;
        }
        s->dx = 0;
        s->dy = 0;
    }

    if (!s->mapped && s->w > 0 && s->h > 0) {
        /* layer-shell surfaces (docked panels) configure/place/map via
         * their own handshake — never the generic toplevel path */
        if (s->layer) {
            if (_layer_commit(s))
                return;
        }
        s->mapped = true;
        s->minimized = false;
        _stack_insert(s);
        /* REQUIRED: tell the client which output the surface is on —
         * Xwayland's software path refuses to attach a buffer before
         * it, so X11 apps were invisible (buffer created, never sent) */
        _surface_output_enter(s);
        if (s->xwl) {
            /* Xwayland window: the X-side WM (vt-wl-xwayland.c) owns
             * geometry; the surface paints exactly where the X window
             * is, no xdg configure handshake involved */
            vt_logd("wayland: xwl surface %p first commit %dx%d at "
                    "+%d+%d (pre-geom)", (void*)s, s->w, s->h, s->x, s->y);
            _xwl_win_geom(s);
            vt_logd("wayland: xwl surface %p mapped at +%d+%d %dx%d",
                    (void*)s, s->x, s->y, s->w, s->h);
            _xwl_announce_geom(s);
            return;
        }
        if (s->popup) {
            if (!s->popup->parent) {
                /* a popup with no parent never got its layer-shell
                 * attachment — it cannot be placed, so reject it */
                if (s->res)
                    wl_resource_post_error(
                        s->res, XDG_WM_BASE_ERROR_ROLE,
                        "popup committed without a parent surface");
                return;
            }
            /* popups map ON TOP (surfaces list head = bottom; inserting
             * at prev = top) — they are menus */
            if (s->popup->grabbed && s->res) {
                /* grabbed popups take keyboard focus (menu navigation) */
                _wl_surf_t *old = st->kbd_focus;
                if (old && old->res) {
                    _kbd_res_t *kr;
                    wl_list_for_each(kr, &st->kbd_reses, link) {
                        if (wl_resource_get_client(kr->res) ==
                            wl_resource_get_client(old->res))
                            wl_keyboard_send_leave(kr->res, ++st->serial,
                                                   old->res);
                    }
                }
                st->kbd_focus = s;
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
            }
            vt_logd("wayland: popup mapped %ux%u at +%d+%d (rel)",
                    (unsigned)s->w, (unsigned)s->h,
                    (int)s->popup->rel_x, (int)s->popup->rel_y);
        } else if (s->toplevel) {
            /* new windows appear on the CURRENT workspace (s->ws was
             * only ever set on taskbar-restore — every window stayed on
             * workspace 1 forever) */
            s->ws = st->ws_cur;
            /* place the first frame: maximized/fullscreen anchor below
             * the panel (or at the very top for fullscreen); everything
             * else centers inside the workarea — the WINDOW (content)
             * rect, not the buffer: CSD shadow margins must not count
             * toward the window's on-screen size */
            int wx, wy, ww, wh;
            _layer_workarea(_wls, &wx, &wy, &ww, &wh);
            int gx = s->have_win_geo ? s->win_gx : 0;
            int gy = s->have_win_geo ? s->win_gy : 0;
            int gw = s->have_win_geo && s->win_gw > 0 ? s->win_gw : s->w;
            int gh = s->have_win_geo && s->win_gh > 0 ? s->win_gh : s->h;
            if (s->toplevel->fullscreen) {
                s->x = -gx;
                s->y = -gy;
            } else if (s->toplevel->maximized) {
                s->x = wx - gx;
                s->y = wy - gy;
            } else if (s->x == 0 && s->y == 0) {
                s->x = wx + (ww - gw) / 2 - gx;
                s->y = wy + (wh - gh) / 2 - gy;
                if (s->y + gy < wy) s->y = wy - gy;
                if (s->x + gx < 0) s->x = -gx;
                if (s->y + gy < 0) s->y = -gy;
            }
            vt_logi("wayland: window 0x%llx '%s' mapped %dx%d at +%d+%d",
                    (unsigned long long)(s->toplevel->id),
                    s->toplevel->title ? s->toplevel->title : "(untitled)",
                    s->w, s->h, s->x, s->y);
            if (s->toplevel->minimized) s->toplevel->minimized = false;
            _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_MAP, s->toplevel);
            /* new window takes keyboard focus */
            _wls->kbd_focus = s;
            _wls->focused_toplevel = s->toplevel;
            _wls->dirty = true;
            /* tell the client which decoration mode won (xdg-decoration
             * clients render their own chrome when we answer "client") */
            _decor_notify(s);
            _kbd_enter_focus(st, s);
            _activate_toplevel(_wls, s->toplevel);
        }
        /* POINTER focus must be recomputed at MAP: a window that
         * appears under the stationary pointer owns it from now on.
         * Nothing ever did this — wl_pointer.enter was only sent when
         * the pointer PHYSICALLY moved, so a freshly-mapped window
         * under the cursor got no pointer enter at all. Toolkits that
         * wake their redraw pipeline on display events (weston's toy
         * toolkit: frame-callback → schedule-redraw idle → the idle
         * runs only when the event loop wakes) then froze after their
         * FIRST frame — weston-flower stopped animating exactly here,
         * with the pointer sitting inside it and no enter delivered. */
        _pointer_focus_update(st, false);
        _wls->dirty = true;
    } else if (s->mapped && s->w > 0) {
        _wls->dirty = true;
        if (s->layer) {
            /* resize of a docked surface: re-anchor, re-configure */
            _layer_commit(s);
            return;
        }
        if (s->xwl) {
            /* Xwayland redraw: the X WM keeps the geometry current */
            _xwl_win_geom(s);
        } else if (s->toplevel) {
            /* client-driven resize (applying OUR configure — maximize,
             * interactive resize ack, or the app resizing itself): the
             * WM model must hear about the new size. Without this the
             * model kept the pre-maximize geometry forever while the
             * window on screen was already huge (pager and taskbar
             * showed a stale window). Emit ONLY on change — this runs
             * on every redraw. */
            if (s->buf_w != s->last_announced_w ||
                s->buf_h != s->last_announced_h) {
                s->last_announced_w = s->buf_w;
                s->last_announced_h = s->buf_h;
                _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_GEOMETRY,
                          s->toplevel);
            }
        }
        if (s->toplevel && s->toplevel->minimized) {
            s->toplevel->minimized = false;
            s->minimized = false;
        }
    }
}

static void _surf_set_opaque(struct wl_client *cli,
                             struct wl_resource *res,
                             struct wl_resource *region) {
    (void)cli; (void)res; (void)region;
}

/* wl_region: REAL rect storage now — set_input_region hands the rect
 * list to the surface, and CSD toolkits (GTK4/Qt) build their input
 * region as content-rect MINUS shadow margins. The old no-op made
 * the compositor deliver clicks into transparent shadow bands. */
typedef struct _region {
    struct _vt_rect *adds;
    int             n_adds;
    struct _vt_rect *subs;
    int             n_subs;
} _region_t;

static void _surf_set_input(struct wl_client *cli,
                            struct wl_resource *res,
                            struct wl_resource *region) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (!s) return;

    /* drop the previous region (both arrays) */
    vt_free(s->in_adds); vt_free(s->in_subs);
    s->in_adds = NULL; s->in_subs = NULL;
    s->n_in_adds = 0; s->n_in_subs = 0;
    s->input_set = false;
    if (!region) return;          /* NULL region = whole surface */
    _region_t *rg = wl_resource_get_user_data(region);
    if (!rg || (rg->n_adds == 0 && rg->n_subs == 0)) {
        /* an EMPTY region: no input anywhere (protocol: an empty region
         * means the surface does not accept input at all) */
        s->input_set = true;
        return;
    }
    if (rg->n_adds > 0) {
        s->in_adds = vt_malloc(sizeof(struct _vt_rect) * (size_t)rg->n_adds);
        if (s->in_adds) {
            memcpy(s->in_adds, rg->adds,
                   sizeof(struct _vt_rect) * (size_t)rg->n_adds);
            s->n_in_adds = rg->n_adds;
        }
    }
    if (rg->n_subs > 0) {
        s->in_subs = vt_malloc(sizeof(struct _vt_rect) * (size_t)rg->n_subs);
        if (s->in_subs) {
            memcpy(s->in_subs, rg->subs,
                   sizeof(struct _vt_rect) * (size_t)rg->n_subs);
            s->n_in_subs = rg->n_subs;
        }
    }
    s->input_set = true;
    /* pointer state may change immediately: the surface under the
     * cursor may no longer accept input there */
    if (_wls) _pointer_focus_update(_wls, false);
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
static void _surf_offset(struct wl_client *cli,
                         struct wl_resource *res, int32_t x, int32_t y) {
    (void)cli;
    _wl_surf_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    /* wl_surface.offset (v5): the next commit moves the surface's
     * content by (-x, -y). GTK4 uses this when its CSD shadow size
     * changes; ignoring it sheared client windows. */
    s->dx = x;
    s->dy = y;
}

/* wl_region implementation (see _region_t above) */
static void _region_destroy(struct wl_client *cli,
                            struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _region_add(struct wl_client *cli, struct wl_resource *res,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli;
    _region_t *rg = wl_resource_get_user_data(res);
    if (!rg || w <= 0 || h <= 0) return;
    struct _vt_rect *na = vt_realloc(rg->adds,
        sizeof(struct _vt_rect) * (size_t)(rg->n_adds + 1));
    if (!na) return;
    rg->adds = na;
    rg->adds[rg->n_adds].x = x;
    rg->adds[rg->n_adds].y = y;
    rg->adds[rg->n_adds].w = w;
    rg->adds[rg->n_adds].h = h;
    rg->n_adds++;
}
static void _region_subtract(struct wl_client *cli,
                             struct wl_resource *res,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli;
    _region_t *rg = wl_resource_get_user_data(res);
    if (!rg || w <= 0 || h <= 0) return;
    struct _vt_rect *ns = vt_realloc(rg->subs,
        sizeof(struct _vt_rect) * (size_t)(rg->n_subs + 1));
    if (!ns) return;
    rg->subs = ns;
    rg->subs[rg->n_subs].x = x;
    rg->subs[rg->n_subs].y = y;
    rg->subs[rg->n_subs].w = w;
    rg->subs[rg->n_subs].h = h;
    rg->n_subs++;
}
static const struct wl_region_interface _region_impl = {
    .destroy = _region_destroy,
    .add = _region_add,
    .subtract = _region_subtract,
};
static void _region_res_destroy(struct wl_resource *res) {
    _region_t *rg = wl_resource_get_user_data(res);
    if (!rg) return;
    vt_free(rg->adds);
    vt_free(rg->subs);
    vt_free(rg);
}
static void _compositor_create_region(struct wl_client *cli,
                                      struct wl_resource *res, uint32_t id) {
    (void)res;
    _region_t *rg = vt_malloc0(sizeof(*rg));
    if (!rg) { wl_client_post_no_memory(cli); return; }
    struct wl_resource *r = wl_resource_create(cli, &wl_region_interface,
                                               1, id);
    if (!r) { vt_free(rg); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_region_impl, rg,
                                   _region_res_destroy);
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
    /* roles own back-pointers into this surface; detach them FIRST —
     * their resource destroy handlers may run AFTER this one when a
     * client dies (libwayland destroys resources in creation order),
     * and they would dereference freed memory */
    if (s->layer)
        _layer_detach(s);
    if (s->xwl) {
        _xwl_surface_destroyed(s);   /* also clears s->xwl */
    }
    if (s->parent) {
        wl_list_remove(&s->sub_link);
        s->parent = NULL;
    }
    if (s->popup) {
        /* the popup OBJECT is owned by its wl_resource (freed in
         * _popup_res_destroy); here we only detach — freeing in both
         * places was a double free / use-after-free */
        s->popup->surf = NULL;
        s->popup = NULL;
    }
    if (s->xdg_res) {
        /* same class of bug, caught by ASan at client teardown: the
         * xdg_surface resource is created AFTER the wl_surface, so at
         * client death ITS destroy handler runs AFTER the surface is
         * freed — orphan its back-pointer now so it never
         * dereferences us */
        struct _xdg_surf_data *d = wl_resource_get_user_data(s->xdg_res);
        if (d) d->surf = NULL;
        s->xdg_res = NULL;
    }
    if (s->decor_res) {
        /* zxdg_toplevel_decoration_v1: same after-us destroy order
         * as xdg_surface above (created later = destroyed later at
         * client death). Its destructor read the freed surface
         * through d->surf (ASan heap-use-after-free) — orphan it. */
        _decor_orphan(s);
    }
    /* staging-protocol weak links (activation tokens, fractional
     * scale objects) must be detached before the surface memory goes */
    _proto_surface_destroyed(s);
    if (s->mapped) {
        wl_list_remove(&s->link);
        if (_wls) {
            if (_wls->ptr_focus == s) _wls->ptr_focus = NULL;
            if (_wls->kbd_focus == s) _wls->kbd_focus = NULL;
            if (_wls->cursor_surf == s) {
                /* the client's cursor surface died (window closed /
                 * cursor swapped): fall back to the compositor cursor
                 * and RE-ARM the hardware plane (leaving it hidden
                 * stranded the pointer with no visible cursor). */
                _wls->cursor_surf = NULL;
                _wls->cur_client_set = false;
                _cursor_apply_hw(_wls);
                _wls->dirty = true;
            }
            /* a window that dies MID-DRAG/RESIZE (app crash, kill)
             * leaves the interactive op holding a freed surface: the
             * next motion event would write through op_surf into
             * freed memory. End the op NOW — the same class of guard
             * as the focus pointers above. */
            if (_wls->op_surf == s) {
                _wls->op_active = false;
                _wls->op_surf = NULL;
            }
            /* same guard for the implicit pointer grab: a press that
             * was forwarded to this surface must not try to deliver
             * its release to freed memory */
            if (_wls->press_surf == s) {
                _wls->press_surf = NULL;
                _wls->press_consumed = true;
            }
        }
        if (s->toplevel)
            _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_UNMAP, s->toplevel);
    }
    if (s->toplevel) {
        _xdg_toplevel_t *t = s->toplevel;
        if (t->res == NULL) {
            /* synthetic toplevel (XWayland) or one whose resource is
             * already gone: the SURFACE destructor owns the record */
            _toplevel_free(t);
        } else {
            /* the xdg_toplevel resource outlives the surface at client
             * death (libwayland destroys resources in creation order:
             * wl_surface first) — its destructor owns the record, we
             * only detach so it never writes through freed memory */
            t->surf = NULL;
            s->toplevel = NULL;
        }
    }
    vt_free(s->in_adds);
    vt_free(s->in_subs);
    vt_free(s->own);
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
    wl_list_init(&s->subs);
    wl_list_init(&s->sub_link);
    struct wl_resource *sr = wl_resource_create(cli, &wl_surface_interface,
                                                wl_resource_get_version(res),
                                                id);
    if (!sr) { vt_free(s); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(sr, &_surf_impl, s,
                                   _surf_resource_destroy);
    s->res = sr;
    /* Identify the Xwayland client by PID the moment it creates its
     * first surface: legacy Xwayland builds never bind
     * xwayland_shell_v1, so _bind_xwl_shell cannot learn the client —
     * but their WL_SURFACE_ID association messages are useless without
     * it (the handler resolves resource ids through X.client). */
    _xwl_learn_client(cli);
    /* wl_surface.enter at CREATION (single-output desktop: every
     * surface is on the one output). Waiting for the first commit is
     * circular: Xwayland's software path waits for enter before it
     * attaches its buffer, so an enter-on-map never happened and X11
     * apps rendered nothing. Clients that have not bound wl_output
     * yet are covered by the commit-path retry. */
    _surface_output_enter(s);
}

static const struct wl_compositor_interface _compositor_impl = {
    .create_surface = _compositor_create_surface,
    .create_region = _compositor_create_region,
};

static void _bind_compositor(struct wl_client *cli, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    /* the resource MUST match the version the client bound at (capped
     * at what we truly support, 6): the old hardcoded cap of 4 with a
     * v6-advertised global gave clients a v6 proxy over a v4 resource —
     * the first wl_surface.offset (v5+, GTK4 frame cycles) was rejected
     * with "invalid method 10 (since 4 < 5)" and the client DIED with
     * EINVAL. */
    struct wl_resource *res = wl_resource_create(cli,
        &wl_compositor_interface, version > 6 ? 6 : version, id);
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
    /* PROTOCOL: wl_data_device.data_offer goes out EXACTLY ONCE,
     * followed by one wl_data_offer.offer per mime type. Sending
     * data_offer once per mime confused real toolkits (kitty aborted
     * its clipboard path on the duplicate events). */
    wl_data_device_send_data_offer(dev_res, offer);
    for (size_t i = 0; i < st->selection->mimes.size; i++) {
        const char *m = *(const char *const *)
            vt_vec_at(&st->selection->mimes, i);
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
    _data_src_t *s = vt_malloc0(sizeof(*s));
    if (!s) { wl_client_post_no_memory(cli); return; }
    s->cli = cli;
    vt_vec_init(&s->mimes, sizeof(char *), 4);
    /* create the resource at the MANAGER's version, not hardcoded 1:
     * a v3 client calling set_actions on a v1 resource is a version
     * violation — libwayland kills the client (kitty died the moment
     * it tried to copy anything: create_data_source → offer →
     * set_actions → connection torn down = "crash") */
    struct wl_resource *r = wl_resource_create(
        cli, &wl_data_source_interface,
        wl_resource_get_version(res), id);
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

/* --------------------------------------------- zwp_primary_selection_v1 */
/* The PRIMARY selection (X11-style: select-to-copy, middle-click paste).
 * Terminals (foot, kitty, mirage's selection) drive their selection
 * clipboard through this protocol; without the global they silently
 * lose selection-paste. Mirrors the clipboard device/source/offer shape
 * one-to-one. */
static void _ps_offer_receive(struct wl_client *cli,
                              struct wl_resource *res,
                              const char *mime, int32_t fd) {
    (void)cli;
    struct wl_resource *src = wl_resource_get_user_data(res);
    _wl_state_t *st = _wls;
    if (!st || !src) { close(fd); return; }
    zwp_primary_selection_source_v1_send_send(src, mime, fd);
}
static void _ps_offer_destroy(struct wl_client *cli,
                              struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct zwp_primary_selection_offer_v1_interface _ps_offer_impl = {
    .receive = _ps_offer_receive,
    .destroy = _ps_offer_destroy,
};

static void _ps_send_selection(_wl_state_t *st, struct wl_resource *dev_res) {
    if (!st->primary_selection || st->primary_selection->dead) {
        zwp_primary_selection_device_v1_send_selection(dev_res, NULL);
        return;
    }
    struct wl_resource *offer = wl_resource_create(
        wl_resource_get_client(dev_res),
        &zwp_primary_selection_offer_v1_interface,
        wl_resource_get_version(dev_res), 0);
    if (!offer) return;
    struct wl_resource *src = st->primary_selection->res;
    wl_resource_set_implementation(offer, &_ps_offer_impl, src, NULL);
    zwp_primary_selection_device_v1_send_data_offer(dev_res, offer);
    for (size_t i = 0; i < st->primary_selection->mimes.size; i++) {
        const char *m = *(const char *const *)
            vt_vec_at(&st->primary_selection->mimes, i);
        zwp_primary_selection_offer_v1_send_offer(offer, m);
    }
    zwp_primary_selection_device_v1_send_selection(dev_res, offer);
}

static void _ps_broadcast(_wl_state_t *st) {
    if (!st || !st->kbd_focus || !st->kbd_focus->res) return;
    _data_dev_t *d;
    wl_list_for_each(d, &st->primary_devs, link) {
        if (d->cli == wl_resource_get_client(st->kbd_focus->res))
            _ps_send_selection(st, d->res);
    }
}

static void _ps_src_offer(struct wl_client *cli, struct wl_resource *res,
                          const char *mime) {
    (void)cli;
    _data_src_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    char *m = vt_strdup(mime ? mime : "");
    vt_vec_push(&s->mimes, &m);
}
static void _ps_src_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _data_src_t *s = wl_resource_get_user_data(res);
    if (s) s->dead = true;
    wl_resource_destroy(res);
}
static const struct zwp_primary_selection_source_v1_interface _ps_src_impl = {
    .offer = _ps_src_offer,
    .destroy = _ps_src_destroy,
};
static void _ps_src_res_destroy(struct wl_resource *res) {
    _data_src_t *s = wl_resource_get_user_data(res);
    if (!s) return;
    if (_wls && _wls->primary_selection == s) {
        _wls->primary_selection = NULL;
        _ps_broadcast(_wls);
    }
    for (size_t i = 0; i < s->mimes.size; i++) {
        char **m = vt_vec_at(&s->mimes, i);
        vt_free(*m);
    }
    vt_vec_fini(&s->mimes);
    vt_free(s);
}

static void _ps_dev_set_selection(struct wl_client *cli,
                                  struct wl_resource *res,
                                  struct wl_resource *src, uint32_t serial) {
    (void)cli; (void)serial;
    _wl_state_t *st = _wls;
    if (!st) return;
    _data_src_t *s = src ? wl_resource_get_user_data(src) : NULL;
    if (s && s->dead) s = NULL;
    if (st->primary_selection && st->primary_selection != s &&
        !st->primary_selection->dead)
        zwp_primary_selection_source_v1_send_cancelled(
            st->primary_selection->res);
    st->primary_selection = s;
    _ps_broadcast(st);
}
static void _ps_dev_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct zwp_primary_selection_device_v1_interface _ps_dev_impl = {
    .set_selection = _ps_dev_set_selection,
    .destroy = _ps_dev_destroy,
};
static void _ps_dev_res_destroy(struct wl_resource *res) {
    _data_dev_t *d = wl_resource_get_user_data(res);
    if (!d) return;
    wl_list_remove(&d->link);
    vt_free(d);
}

static void _ps_mgr_get_device(struct wl_client *cli,
                               struct wl_resource *res, uint32_t id,
                               struct wl_resource *seat) {
    (void)seat;
    _wl_state_t *st = _wls;
    if (!st) return;
    _data_dev_t *d = vt_malloc0(sizeof(*d));
    if (!d) { wl_client_post_no_memory(cli); return; }
    d->cli = cli;
    struct wl_resource *r = wl_resource_create(
        cli, &zwp_primary_selection_device_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { vt_free(d); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_ps_dev_impl, d,
                                   _ps_dev_res_destroy);
    d->res = r;
    wl_list_insert(st->primary_devs.prev, &d->link);
    if (st->kbd_focus && st->kbd_focus->res &&
        wl_resource_get_client(st->kbd_focus->res) == cli)
        _ps_send_selection(st, r);
}
static void _ps_mgr_create_source(struct wl_client *cli,
                                  struct wl_resource *res, uint32_t id) {
    _data_src_t *s = vt_malloc0(sizeof(*s));
    if (!s) { wl_client_post_no_memory(cli); return; }
    s->cli = cli;
    vt_vec_init(&s->mimes, sizeof(char *), 4);
    struct wl_resource *r = wl_resource_create(
        cli, &zwp_primary_selection_source_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) {
        vt_vec_fini(&s->mimes);
        vt_free(s);
        wl_client_post_no_memory(cli);
        return;
    }
    wl_resource_set_implementation(r, &_ps_src_impl, s,
                                   _ps_src_res_destroy);
    s->res = r;
}
static void _ps_mgr_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct
zwp_primary_selection_device_manager_v1_interface _ps_mgr_impl = {
    .create_source = _ps_mgr_create_source,
    .get_device = _ps_mgr_get_device,
    .destroy = _ps_mgr_destroy,
};

static void _bind_primary_sel(struct wl_client *cli, void *data,
                              uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(
        cli, &zwp_primary_selection_device_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_ps_mgr_impl, NULL, NULL);
}

/* ------------------------------------------------------------ xdg-shell */
/* per-surface xdg data (_xdg_surf_data_t) is defined near the top:
 * the surface destructor needs it to orphan back-pointers */

static void _xdg_wm_base_destroy(struct wl_client *cli,
                                 struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _xdg_surface_destroy(struct wl_client *cli,
                                 struct wl_resource *res);
static void _xdg_surface_ack(struct wl_client *cli,
                             struct wl_resource *res, uint32_t serial) {
    (void)cli;
    /* The client acknowledged our last configure batch. The commit
     * that follows applies the configured size (s->w/h update from the
     * committed buffer). Serial mismatch = protocol misuse by the
     * client; we tolerate it (the old code ignored ack entirely —
     * harmless then, but now the serial pairs the handshake). */
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    (void)serial;
    (void)d;
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
    if (!d || !d->surf) return;
    _wl_surf_t *s = d->surf;
    /* SET the geometry (buffer-relative window rect); CSD shadows live
     * in the buffer outside it. Keep the window's VISIBLE origin
     * stable when the client's shadow margins change (anchor the old
     * geometry origin), and never accumulate — the old `x += x` made
     * every resize drag the window across the screen. */
    if (s->have_win_geo) {
        s->x += s->last_gx - x;
        s->y += s->last_gy - y;
    }
    s->win_gx = x;
    s->win_gy = y;
    s->last_gx = x;
    s->last_gy = y;
    if (w > 0) s->win_gw = w;
    if (h > 0) s->win_gh = h;
    s->have_win_geo = true;
    if (s->toplevel)
        _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, s->toplevel);
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
/* xdg_toplevel resource teardown — the OTHER half of the window-close
 * handshake. Destroying the toplevel (without a nil commit first, or
 * after it) is how toolkits close windows; the resource previously had
 * NO destructor, so the record leaked AND the window never emitted
 * WIN_UNMAP when the client kept the wl_surface alive — a stale
 * pager/taskbar entry that no longer responded to anything (the ghost
 * window). The emit only fires while the surface still maps THIS
 * toplevel (the nil-commit path in _surf_commit already emitted). */
static void _toplevel_res_destroy(struct wl_resource *res) {
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (!t) return;
    wl_resource_set_user_data(res, NULL);
    _wl_surf_t *s = t->surf;
    if (s && s->toplevel == t) {
        if (s->mapped) {
            s->mapped = false;
            wl_list_remove(&s->link);
            if (_wls) {
                _surface_output_leave(s);
                if (_wls->ptr_focus == s) _wls->ptr_focus = NULL;
                if (_wls->kbd_focus == s) _wls->kbd_focus = NULL;
                if (_wls->op_surf == s) {
                    _wls->op_active = false;
                    _wls->op_surf = NULL;
                }
                if (_wls->focused_toplevel == t)
                    _wls->focused_toplevel = NULL;
                _wls->dirty = true;
                _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_UNMAP, t);
            }
        }
        s->toplevel = NULL;
    }
    t->surf = NULL;
    _toplevel_free(t);
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
        _wls->op_edges = 0;
        _wls->op_surf = t->surf;
        _wls->op_grab_x = _wls->cursor_x - t->surf->x;
        _wls->op_grab_y = _wls->cursor_y - t->surf->y;
        _wls->op_start_x = t->surf->x;
        _wls->op_start_y = t->surf->y;
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
    (void)cli; (void)seat; (void)serial;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t && t->surf && _wls) {
        /* the CLIENT names the edge it wants resized via
         * XDG_TOPLEVEL_RESIZE_EDGE_* — translate to our E/S/W/N bits
         * (1/2/4/8) instead of always resizing bottom-right */
        static const uint8_t edge_map[9] = {
            0,                 /* NONE          */
            8,                 /* TOP           */
            2,                 /* BOTTOM        */
            4,                 /* LEFT          */
            8 | 4,             /* TOP_LEFT      */
            2 | 4,             /* BOTTOM_LEFT   */
            1,                 /* RIGHT         */
            8 | 1,             /* TOP_RIGHT     */
            2 | 1,             /* BOTTOM_RIGHT  */
        };
        _wls->op_active = true;
        _wls->op_resize = true;
        _wls->op_edges = edges <= 8 ? edge_map[edges] : 0;
        _wls->op_surf = t->surf;
        _wls->op_grab_x = _wls->cursor_x;
        _wls->op_grab_y = _wls->cursor_y;
        _wls->op_start_x = t->surf->x;
        _wls->op_start_y = t->surf->y;
        _wls->op_start_w = _surf_cw(t->surf);
        _wls->op_start_h = _surf_ch(t->surf);
        _wls->op_last_cw = _wls->op_start_w;
        _wls->op_last_ch = _wls->op_start_h;
        _wls->op_last_geo_us = 0;
        t->resizing = true;
    }
}
static void _toplevel_set_max(struct wl_client *cli,
                              struct wl_resource *res,
                              int32_t w, int32_t h) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) { t->max_w = w > 0 ? w : 0; t->max_h = h > 0 ? h : 0; }
}
static void _toplevel_set_min(struct wl_client *cli,
                              struct wl_resource *res,
                              int32_t w, int32_t h) {
    (void)cli;
    /* REAL size hints: interactive edge-resize clamps to the client's
     * minimum instead of shrinking windows to nothing (0 = unset, as
     * the protocol defines) */
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    if (t) { t->min_w = w > 0 ? w : 0; t->min_h = h > 0 ? h : 0; }
}
void _toplevel_configure(_xdg_toplevel_t *t, int32_t w, int32_t h,
                                uint32_t state) {
    /* The FULL xdg-shell configure handshake — this was the root cause
     * of "native Wayland applications cannot actually be resized": the
     * old code sent ONLY the xdg_toplevel.configure event. Per the
     * protocol that event is always followed by an xdg_surface.
     * configure(serial) — THE event that lets the client ack and apply
     * the new size. Real toolkits (GTK4/Qt) buffer the toplevel
     * configure and NEVER act on it until the paired surface configure
     * arrives, so every later configure (interactive resize, maximize,
     * unmaximize) was silently ignored: the compositor's model grew
     * while the client kept committing its old buffer, and the SSD
     * pending-resize backdrop filled the gap with an eternal black
     * rectangle. */
    if (!t || !t->res || !t->surf) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof(uint32_t));
    if (st) *st = state;
    xdg_toplevel_send_configure(t->res, w, h, &states);
    wl_array_release(&states);
    if (t->surf->xdg_res) {
        xdg_surface_send_configure(t->surf->xdg_res, ++_wls->serial);
        t->surf->last_cfg_serial = _wls->serial;
    }
}

/* shared maximize apply/clear for xdg toplevels (protocol requests AND
 * the SSD button): the FRAME fills the workarea — an SSD window's
 * client is inset by the frame extents so the title band stays BELOW
 * the docked panel (the bare-workarea placement put the grab bar
 * behind the panel: "maximize goes over the top panel"); the configure
 * handshake is COMPLETE so real toolkits actually apply it; the
 * previous geometry is saved for restore. */
static void _wl_maximize_apply(_xdg_toplevel_t *t, bool on) {
    if (!t || !t->surf || !_wls) return;
    _wl_surf_t *s = t->surf;
    if (on) {
        int wx, wy, ww, wh;
        _layer_workarea(_wls, &wx, &wy, &ww, &wh);
        if (!t->maximized) {          /* save restore geometry once */
            t->prev_x = s->x; t->prev_y = s->y;
            t->prev_w = s->w; t->prev_h = s->h;
        }
        int tbar = s->ssd ? (_WL_SSD_TITLE + _WL_SSD_BORDER) : 0;
        int brd  = s->ssd ? _WL_SSD_BORDER : 0;
        /* place the WINDOW (content) rect: CSD buffers carry shadow
         * margins outside set_window_geometry — placing the BUFFER
         * origin at the workarea left the content inset by the margin
         * and the shadow band (opaque black before the blend fix)
         * edging the maximized window. */
        int gx = s->have_win_geo ? s->win_gx : 0;
        int gy = s->have_win_geo ? s->win_gy : 0;
        s->x = wx + brd - gx;
        s->y = wy + tbar - gy;
        t->maximized = true;
        _toplevel_configure(t, ww - 2 * brd, wh - tbar - brd,
                            XDG_TOPLEVEL_STATE_MAXIMIZED);
    } else {
        t->maximized = false;
        s->x = t->prev_x;
        s->y = t->prev_y;
        _toplevel_configure(t, 0, 0, 0);   /* client picks its own size */
    }
    _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
    _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, t);
    _wls->dirty = true;
}

static void _toplevel_maximize(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    _wl_maximize_apply(t, true);
}
static void _toplevel_unmaximize(struct wl_client *cli,
                                 struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    _wl_maximize_apply(t, false);
}
static void _toplevel_fullscreen(struct wl_client *cli,
                                 struct wl_resource *res,
                                 struct wl_resource *output);
static void _toplevel_unfullscreen(struct wl_client *cli,
                                   struct wl_resource *res);

/* Fullscreen state application — shared by the client-driven request
 * path (xdg_toplevel.set_fullscreen) and the WM/taskbar-driven path
 * (backend op fullscreen_window: IPC "vantage-remote fullscreen <id>").
 * The WM path used to only flip MODEL flags — the pager showed a
 * fullscreen window while nothing happened on screen (and the old
 * harness never noticed because no check covered it). */
static void _wl_fullscreen_apply(_xdg_toplevel_t *t, bool on) {
    if (!t || !t->surf || !_wls) return;
    _wl_surf_t *s = t->surf;
    if (on) {
        if (t->fullscreen) {
            _toplevel_configure(t, _wls->out_w, _wls->out_h,
                                XDG_TOPLEVEL_STATE_FULLSCREEN);
            return;
        }
        t->prev_x = s->x; t->prev_y = s->y;
        t->prev_w = s->w; t->prev_h = s->h;
        t->fullscreen = true;
        /* the CONTENT rect covers the output: a fullscreen CSD window
         * whose buffer still carries shadow margins would otherwise
         * show them as black edges of the display (the reported
         * "browser ends up with a black portion of the screen in
         * fullscreen"). */
        int gx = s->have_win_geo ? s->win_gx : 0;
        int gy = s->have_win_geo ? s->win_gy : 0;
        s->x = -gx;
        s->y = -gy;
        _toplevel_configure(t, _wls->out_w, _wls->out_h,
                            XDG_TOPLEVEL_STATE_FULLSCREEN);
    } else {
        if (!t->fullscreen) return;
        t->fullscreen = false;
        s->x = t->prev_x;
        s->y = t->prev_y;
        _toplevel_configure(t, 0, 0, 0);
    }
    _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_STATE, t);
    _emit_win(_wls, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, t);
    _wls->dirty = true;
}

static void _toplevel_fullscreen(struct wl_client *cli,
                                 struct wl_resource *res,
                                 struct wl_resource *output) {
    (void)cli; (void)output;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    _wl_fullscreen_apply(t, true);
}
static void _toplevel_unfullscreen(struct wl_client *cli,
                                   struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    _wl_fullscreen_apply(t, false);
}
static void _toplevel_set_minimized(struct wl_client *cli,
                                    struct wl_resource *res) {
    (void)cli;
    _xdg_toplevel_t *t = wl_resource_get_user_data(res);
    _wl_state_t *st = _wls;
    if (!t || !t->surf || !st) return;
    /* real minimize: hidden from compositing, restored from the
     * taskbar. (This used to be aliased onto unset_maximized — a
     * bogus empty configure and nothing was ever minimized.) */
    t->minimized = true;
    t->surf->minimized = true;
    if (st->kbd_focus == t->surf) st->kbd_focus = NULL;
    st->dirty = true;
    vt_logi("wayland: window 0x%llx minimized",
            (unsigned long long)t->id);
    _emit_win(st, VT_BACKEND_WL_EVENT_WIN_STATE, t);
    /* focus falls to the top-most remaining mapped toplevel */
    _wl_surf_t *s;
    wl_list_for_each_reverse(s, &st->surfaces, link) {
        if (!s->mapped || s->is_cursor || s->minimized || !s->toplevel)
            continue;
        if (s->ws != st->ws_cur) continue;
        st->kbd_focus = s;
        st->focused_toplevel = s->toplevel;
        _kbd_enter_focus(st, s);
        _activate_toplevel(st, s->toplevel);
        break;
    }
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
    .set_minimized = _toplevel_set_minimized,
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
    if (d && d->surf && d->surf->xdg_res == res)
        d->surf->xdg_res = NULL;
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
    if (s) s->xdg_res = xres;
    /* send initial configure */
    xdg_surface_send_configure(xres, _wls ? ++_wls->serial : 1);
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
    wl_resource_set_implementation(tres, &_toplevel_impl, t,
                                   _toplevel_res_destroy);
    t->res = tres;
    _toplevel_configure(t, 0, 0, 0);
    /* legacy double-configure kept for toolkits that count exactly
     * two events before their first commit (GTK's initial handshake) */
    xdg_surface_send_configure(res, ++_wls->serial);
}

/* positioner (popup placement) — parsed for REAL: menus land where the
 * client asked (anchored to the parent), with the positioner's own
 * size, not a made-up 320x200 centered on the screen. */
static void _pos_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    vt_free(p);
    wl_resource_destroy(res);
}
static void _pos_set_size(struct wl_client *cli, struct wl_resource *res,
                          int32_t w, int32_t h) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (!p || w < 1 || h < 1) return;
    p->size_w = w;
    p->size_h = h;
    p->has_size = true;
}
static void _pos_set_anchor(struct wl_client *cli, struct wl_resource *res,
                            uint32_t a) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (p) p->anchor = a;
}
static void _pos_set_gravity(struct wl_client *cli,
                             struct wl_resource *res,
                             uint32_t g) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (p) p->gravity = g;
}
static void _pos_set_offset(struct wl_client *cli, struct wl_resource *res,
                            int32_t x, int32_t y) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (!p) return;
    p->off_x = x;
    p->off_y = y;
}
static void _pos_set_anchor_rect(struct wl_client *cli,
                                 struct wl_resource *res,
                                 int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (!p) return;
    p->ax = x;
    p->ay = y;
    p->aw = w;
    p->ah = h;
}
static void _pos_set_constraint(struct wl_client *cli,
                                struct wl_resource *res,
                                uint32_t constraint) {
    (void)cli;
    _xdg_pos_t *p = wl_resource_get_user_data(res);
    if (p) p->constraint = constraint;
}
static const struct xdg_positioner_interface _pos_impl = {
    .destroy = _pos_destroy,
    .set_size = _pos_set_size,
    .set_anchor_rect = _pos_set_anchor_rect,
    .set_anchor = _pos_set_anchor,
    .set_gravity = _pos_set_gravity,
    .set_constraint_adjustment = _pos_set_constraint,
    .set_offset = _pos_set_offset,
};

static void _xdg_create_positioner(struct wl_client *cli,
                                   struct wl_resource *res, uint32_t id) {
    _xdg_pos_t *p = vt_malloc0(sizeof(*p));
    if (!p) { wl_client_post_no_memory(cli); return; }
    struct wl_resource *r = wl_resource_create(cli,
        &xdg_positioner_interface, wl_resource_get_version(res), id);
    if (!r) { vt_free(p); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_pos_impl, p, NULL);
}

/* compute the popup rect (relative to the parent surface) from the
 * positioner state — the xdg-shell anchor/gravity/constraint dance */
/* The anchor/gravity enums are NOT bitmasks: left=3 shares its bits
 * with top=1|bottom=2, so `value & ANCHOR_LEFT` wrongly matches plain
 * top/bottom values. Membership tests are the only correct form. */
static bool _anc_top(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_TOP ||
           a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           a == XDG_POSITIONER_ANCHOR_TOP_RIGHT;
}
static bool _anc_bottom(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_BOTTOM ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}
static bool _anc_left(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_LEFT ||
           a == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
}
static bool _anc_right(uint32_t a) {
    return a == XDG_POSITIONER_ANCHOR_RIGHT ||
           a == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
           a == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}
static bool _grav_top(uint32_t g) {
    return g == XDG_POSITIONER_GRAVITY_TOP ||
           g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           g == XDG_POSITIONER_GRAVITY_TOP_RIGHT;
}
static bool _grav_bottom(uint32_t g) {
    return g == XDG_POSITIONER_GRAVITY_BOTTOM ||
           g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ||
           g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}
static bool _grav_left(uint32_t g) {
    return g == XDG_POSITIONER_GRAVITY_LEFT ||
           g == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           g == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
}
static bool _grav_right(uint32_t g) {
    return g == XDG_POSITIONER_GRAVITY_RIGHT ||
           g == XDG_POSITIONER_GRAVITY_TOP_RIGHT ||
           g == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}

void _popup_place(const _xdg_pos_t *pos, _wl_surf_t *parent,
                         int out_w, int out_h, int32_t *rx, int32_t *ry,
                         int32_t *rw, int32_t *rh) {
    int32_t w = pos->has_size ? pos->size_w : 200;
    int32_t h = pos->has_size ? pos->size_h : 200;
    if (w > out_w) w = out_w;
    if (h > out_h) h = out_h;

    /* anchor point on/inside the anchor rect (xdg-shell spec): corner
     * anchors pick the corner; edge anchors pick the CENTER of that
     * edge; none picks the center of the rect */
    double ax = parent->x + pos->ax;
    double ay = parent->y + pos->ay;
    if (_anc_right(pos->anchor))            ax += pos->aw;
    else if (!_anc_left(pos->anchor) &&
             (_anc_top(pos->anchor) || _anc_bottom(pos->anchor)))
        ax += pos->aw / 2.0;                 /* edge anchor: centered */
    else if (pos->anchor == XDG_POSITIONER_ANCHOR_NONE)
        ax += pos->aw / 2.0;
    if (_anc_bottom(pos->anchor))           ay += pos->ah;
    else if (!_anc_top(pos->anchor) && !_anc_bottom(pos->anchor) &&
             (_anc_left(pos->anchor) || _anc_right(pos->anchor)))
        ay += pos->ah / 2.0;                 /* edge anchor: centered */
    else if (pos->anchor == XDG_POSITIONER_ANCHOR_NONE)
        ay += pos->ah / 2.0;

    /* gravity (spec): "in what direction the surface should be
     * positioned, relative to the anchor point"; axes without a
     * gravity are CENTERED over the anchor point. gravity bottom =
     * the child sits BELOW the anchor (its top edge there); gravity
     * top = ABOVE (its bottom edge there); left/right accordingly. */
    double x, y;
    if (_grav_left(pos->gravity))      x = ax - w;    /* child to the LEFT */
    else if (_grav_right(pos->gravity)) x = ax;       /* child to the RIGHT */
    else                                x = ax - w / 2.0;   /* centered */
    if (_grav_top(pos->gravity))       y = ay - h;    /* child ABOVE */
    else if (_grav_bottom(pos->gravity)) y = ay;      /* child BELOW */
    else                                y = ay - h / 2.0;  /* centered */

    x += pos->off_x;
    y += pos->off_y;

    /* constraint adjustment: FLIP mirrors the popup across the anchor
     * rect (inverting anchor+gravity on that axis) when it would not
     * fit; SLIDE moves it flush inside the output; RESIZE shrinks it.
     * Applied per axis, the spec order: flip → slide → resize. */
    bool flip_x = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
    bool flip_y = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
    bool slide_x = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
    bool slide_y = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
    bool resize_x = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X;
    bool resize_y = pos->constraint &
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;

    /* X axis */
    if (x + w > out_w) {
        if (flip_x) x = ax - w;             /* mirror across the anchor */
        if (slide_x && x + w > out_w) x = out_w - w;
        if (resize_x && x + w > out_w) w = out_w - (x < 0 ? 0 : (int)x);
        if (x + w > out_w) x = out_w - w;   /* never hang off-screen */
    }
    if (x < 0) {
        if (flip_x) x = ax;
        if (slide_x && x < 0) x = 0;
        if (x < 0) x = 0;
    }
    /* Y axis */
    if (y + h > out_h) {
        if (flip_y) y = ay - h;
        if (slide_y && y + h > out_h) y = out_h - h;
        if (resize_y && y + h > out_h) h = out_h - (y < 0 ? 0 : (int)y);
        if (y + h > out_h) y = out_h - h;
    }
    if (y < 0) {
        if (flip_y) y = ay;
        if (slide_y && y < 0) y = 0;
        if (y < 0) y = 0;
    }

    *rx = (int32_t)x - parent->x;
    *ry = (int32_t)y - parent->y;
    *rw = w;
    *rh = h;
}

/* ---- xdg_popup requests (destroy/grab/reposition) — a NULL impl
 * here made libwayland kill the client the moment a GTK menu closed
 * (unknown request → protocol error → "app just crashed"). */
static void _popup_destroy_req(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    _xdg_popup_t *p = wl_resource_get_user_data(res);
    vt_logd("wayland: popup destroy req (p=%p surf=%p mapped=%d)",
            (void *)p, p ? (void *)p->surf : NULL,
            p && p->surf ? (int)p->surf->mapped : -1);
    if (p && p->surf) {
        p->surf->popup = NULL;
        if (p->surf->mapped) {
            p->surf->mapped = false;
            wl_list_remove(&p->surf->link);
        }
        if (_wls) {
            if (_wls->kbd_focus == p->surf) _wls->kbd_focus = NULL;
            if (_wls->ptr_focus == p->surf) _wls->ptr_focus = NULL;
            _wls->dirty = true;
        }
    }
    wl_resource_destroy(res);
}
static void _popup_grab_req(struct wl_client *cli, struct wl_resource *res,
                            struct wl_resource *seat, uint32_t serial) {
    (void)cli; (void)seat; (void)serial;
    _xdg_popup_t *p = wl_resource_get_user_data(res);
    if (p) p->grabbed = true;
}
static void _popup_reposition_req(struct wl_client *cli,
                                  struct wl_resource *res,
                                  struct wl_resource *positioner,
                                  uint32_t token) {
    (void)cli;
    _xdg_popup_t *p = wl_resource_get_user_data(res);
    _xdg_pos_t *pos = wl_resource_get_user_data(positioner);
    if (!p || !pos || !p->parent || !_wls) return;
    int32_t rx, ry, rw, rh;
    _popup_place(pos, p->parent, _wls->out_w, _wls->out_h,
                 &rx, &ry, &rw, &rh);
    p->rel_x = rx;
    p->rel_y = ry;
    xdg_popup_send_configure(res, rx, ry, rw, rh);
    if (p->surf && p->surf->res)
        xdg_surface_send_configure(p->surf->res, token);
    _wls->dirty = true;
}
static const struct xdg_popup_interface _popup_impl = {
    .destroy = _popup_destroy_req,
    .grab = _popup_grab_req,
    .reposition = _popup_reposition_req,
};

static void _popup_res_destroy(struct wl_resource *res) {
    _xdg_popup_t *p = wl_resource_get_user_data(res);
    if (!p) return;
    /* detach from a still-alive surface (the surface may already be
     * gone — it NULLs p->surf on its own destruction) */
    if (p->surf) p->surf->popup = NULL;
    vt_free(p);
}

/* dismiss a grabbed popup: popup_done → the client unmaps/destroys it */
void _popup_done(_wl_state_t *st, _wl_surf_t *s) {
    if (!st || !s || !s->popup || !s->popup->res) return;
    xdg_popup_send_popup_done(s->popup->res);
    s->popup->grabbed = false;
    /* hide it NOW — the client's destroy/unmap follows asynchronously */
    if (s->mapped) {
        s->mapped = false;
        wl_list_remove(&s->link);
        _surface_output_leave(s);
    }
    if (st->kbd_focus == s) {
        st->kbd_focus = NULL;
        /* restore focus to the parent's toplevel */
        _wl_surf_t *ps = s->popup ? s->popup->parent : NULL;
        while (ps && !ps->toplevel && ps->popup) ps = ps->popup->parent;
        if (ps && ps->toplevel) {
            st->kbd_focus = ps;
            st->focused_toplevel = ps->toplevel;
            _kbd_enter_focus(st, ps);
        }
    }
    st->dirty = true;
}

static void _xdg_get_popup(struct wl_client *cli,
                           struct wl_resource *res,
                           uint32_t id, struct wl_resource *parent,
                           struct wl_resource *positioner) {
    _xdg_surf_data_t *d = wl_resource_get_user_data(res);
    /* the parent argument is the parent's xdg_surface resource — OR an
     * xdg_popup for nested menus (submenus, combobox stacks). Its user
     * data is the _xdg_surf_data_t / _xdg_popup_t respectively, NEVER
     * a _wl_surf_t: assigning the raw user_data to _wl_surf_t* was a
     * TYPE CONFUSION that read "parent->x" far out of bounds (ASan:
     * heap-buffer-overflow) — popups were placed at garbage offsets
     * that happened to look harmless in release builds. Resolve to the
     * actual parent SURFACE through the correct type. */
    _wl_surf_t *psurf = NULL;
    if (parent) {
        if (wl_resource_instance_of(parent, &xdg_popup_interface, NULL)) {
            _xdg_popup_t *pp = wl_resource_get_user_data(parent);
            if (pp) psurf = pp->surf;
        } else {
            _xdg_surf_data_t *pd = wl_resource_get_user_data(parent);
            if (pd) psurf = pd->surf;
        }
    }
    _xdg_pos_t *pos = positioner
        ? wl_resource_get_user_data(positioner) : NULL;
    if (!d || !pos || !pos->has_size) {
        wl_resource_post_error(res, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                               "get_popup requires a sized positioner");
        return;
    }
    /* NULL parent is legal ONLY for the layer-shell path: the
     * gtk4-layer-shell shim calls xdg_surface.get_popup(parent=NULL)
     * and then zwlr_layer_surface_v1.get_popup() to attach it. The
     * popup stays unplaced until then; a commit without any parent is
     * rejected below in _surf_commit. */
    _xdg_popup_t *p = vt_malloc0(sizeof(*p));
    if (!p) { wl_client_post_no_memory(cli); return; }
    p->surf = d->surf;
    p->parent = psurf;
    if (pos) p->pos = *pos;
    d->surf->popup = p;
    struct wl_resource *pres = wl_resource_create(cli,
        &xdg_popup_interface, wl_resource_get_version(res), id);
    if (!pres) { vt_free(p); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(pres, &_popup_impl, p,
                                   _popup_res_destroy);
    p->res = pres;
    if (!psurf) {
        /* layer-shell popup: placement + configure happen when
         * zwlr_layer_surface_v1.get_popup() attaches the parent */
        xdg_surface_send_configure(res, ++_wls->serial);
        vt_logd("wayland: popup created (layer parent pending)");
        return;
    }
    int32_t rx, ry, rw, rh;
    _popup_place(pos, psurf, _wls ? _wls->out_w : 1024,
                 _wls ? _wls->out_h : 768, &rx, &ry, &rw, &rh);
    p->rel_x = rx;
    p->rel_y = ry;
    /* configure sequence per spec: popup.configure, then the
     * xdg_surface configure that lets the client commit */
    xdg_popup_send_configure(pres, rx, ry, rw, rh);
    xdg_surface_send_configure(res, ++_wls->serial);
    vt_logd("wayland: popup %dx%d rel +%d+%d (parent %p)",
            (int)rw, (int)rh, (int)rx, (int)ry, (void *)psurf);
}

static void _xdg_wm_base_pong(struct wl_client *cli,
                              struct wl_resource *res, uint32_t serial) {
    (void)cli; (void)res; (void)serial;
}

static const struct xdg_wm_base_interface _xdg_wm_base_impl = {
    .destroy = _xdg_wm_base_destroy,
    .create_positioner = _xdg_create_positioner,
    .get_xdg_surface = _xdg_wm_base_get_xdg_surface,
    .pong = _xdg_wm_base_pong,
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

static void _cursor_apply_hw(_wl_state_t *st);

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
            st->client_shape = 0;   /* surface cursor replaces a named shape */
        }
    } else {
        /* client cleared its cursor: restore the compositor arrow (a
         * client_shape image may currently occupy the cursor slot) */
        _cursor_client_release();
    }
    /* keep the hardware plane consistent with the takeover: while a
     * client cursor is active the plane must be HIDDEN (the sprite is
     * the single source), and when the client clears its cursor the
     * plane must come BACK. Without this the plane kept showing a
     * stale compositor arrow that no longer followed the pointer. */
    _cursor_apply_hw(st);
    st->dirty = true;
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
    /* No usable xkb keymap — STILL never pass fd -1. libwayland dups
     * the fd argument at marshal time; an invalid fd is an EBADF
     * marshal error that KILLS the whole client connection (real apps
     * die the moment they bind wl_keyboard). A valid empty memfd keeps
     * the connection alive; the client sees a size-0 keymap it cannot
     * compile and can cope with that. */
    vt_logw("wayland: sending an EMPTY keymap to a client — keyboard "
            "input will not work (xkb keymap was not compiled)");
    int fd = memfd_create("vantage-keymap-empty", MFD_CLOEXEC);
    if (fd < 0) fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        wl_keyboard_send_keymap(kres, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                 fd, 0);
        close(fd);
    }
    /* if even /dev/null is unavailable the keymap event is skipped —
     * still better than poisoning the connection with an invalid fd */
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
        /* if a surface of THIS client is already focused, enter
         * immediately. The surface resource belongs to the client
         * that created it — sending it on another client's keyboard
         * is a cross-client object violation that libwayland flags as
         * a fatal compositor bug and kills the connection (any app
         * connecting while another app's window is focused would die
         * on the spot). */
        if (st->kbd_focus && st->kbd_focus->res &&
            wl_resource_get_client(st->kbd_focus->res) == cli) {
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
    /* Advertise a NEWER seat protocol: GTK4 accumulates pointer axis
     * deltas and only delivers a scroll event on wl_pointer.frame —
     * which requires pointer resource version >= 7 (axis_value120 and
     * axis_stop need 8/9). The old cap at v5 meant wheel events
     * ARRIVED but were never turned into GTK scroll events: the
     * Start Menu (and every GTK app) could not scroll. Client binds
     * min(their supported version, this) so older clients are
     * unaffected. */
    struct wl_resource *res = wl_resource_create(cli, &wl_seat_interface,
                                                 version < 9 ? version : 9,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_seat_impl, NULL, NULL);
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_POINTER |
                                   WL_SEAT_CAPABILITY_KEYBOARD);
}

/* send wl_keyboard.enter for s to its client's keyboards (leave the
 * old focus first). Shared by focus paths so every transition is
 * paired and client-scoped. */
void _kbd_enter_focus(_wl_state_t *st, _wl_surf_t *s) {
    if (!st || !s || !s->res) return;
    _wl_surf_t *old = st->kbd_focus;
    if (old && old != s && old->res) {
        _kbd_res_t *kr;
        wl_list_for_each(kr, &st->kbd_reses, link) {
            if (wl_resource_get_client(kr->res) ==
                wl_resource_get_client(old->res))
                wl_keyboard_send_leave(kr->res, ++st->serial, old->res);
        }
    }
    _kbd_res_t *kr;
    wl_list_for_each(kr, &st->kbd_reses, link) {
        if (wl_resource_get_client(kr->res) ==
            wl_resource_get_client(s->res)) {
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(kr->res, ++st->serial, s->res, &keys);
            wl_array_release(&keys);
        }
    }
    st->kbd_focus = s;
}

/* ------------------------------------------------ xdg-decoration v1 */
/* Compositors that decorate EVERY toplevel double-decorate CSD apps
 * (the user's browser drew its own frame AND ours). The protocol is
 * the honest fix: advertise it, honor the client's requested mode,
 * DEFAULT to client-side (no double decoration ever), and draw our
 * own SSD frame only for toplevels that explicitly asked for it. */
typedef struct {
    _wl_surf_t *surf;
    struct wl_resource *res;
} _decor_t;

static void _decor_notify(_wl_surf_t *s) {
    if (!s || !s->toplevel || !s->toplevel->res) return;
    uint32_t mode = s->ssd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                            : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    /* the mode event goes to the zxdg_toplevel_decoration_v1 object,
     * remembered per-surface via decor_res below */
    if (s->decor_res)
        zxdg_toplevel_decoration_v1_send_configure(s->decor_res, mode);
}

static void _decor_destroy_req(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    _decor_t *d = wl_resource_get_user_data(res);
    if (d) {
        if (d->surf) d->surf->decor_res = NULL;
        /* detach the user_data BEFORE wl_resource_destroy: its
         * destructor (_decor_res_destroy) reads the record — leaving
         * the pointer set was a heap-use-after-free the moment a real
         * toolkit (foot) destroyed its decoration object at startup */
        wl_resource_set_user_data(res, NULL);
        vt_free(d);
    }
    wl_resource_destroy(res);
}
static void _decor_set_mode(struct wl_client *cli, struct wl_resource *res,
                            uint32_t mode) {
    (void)cli;
    _decor_t *d = wl_resource_get_user_data(res);
    if (!d || !d->surf) return;
    bool ssd = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    if (d->surf->ssd != ssd) {
        d->surf->ssd = ssd;
        if (_wls) _wls->dirty = true;
        vt_logd("wayland: toplevel decoration mode -> %s",
                ssd ? "server" : "client");
    }
    zxdg_toplevel_decoration_v1_send_configure(res, mode);
}
static void _decor_unset_mode(struct wl_client *cli,
                              struct wl_resource *res) {
    /* unset = follow the compositor default (client-side here) */
    _decor_set_mode(cli, res,
                    ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static const struct zxdg_toplevel_decoration_v1_interface _decor_impl = {
    .destroy = _decor_destroy_req,
    .set_mode = _decor_set_mode,
    .unset_mode = _decor_unset_mode,
};
static void _decor_res_destroy(struct wl_resource *res) {
    _decor_t *d = wl_resource_get_user_data(res);
    if (!d) return;
    if (d->surf && d->surf->decor_res == res) d->surf->decor_res = NULL;
    vt_free(d);
}

/* detach a surface's decoration object: called from the SURFACE
 * destructor, which runs BEFORE the decoration resource's own
 * destructor at client death (libwayland destroys resources in
 * creation order, and the decoration was created after the surface) */
static void _decor_orphan(_wl_surf_t *s) {
    if (!s || !s->decor_res) return;
    _decor_t *d = wl_resource_get_user_data(s->decor_res);
    if (d) d->surf = NULL;
    s->decor_res = NULL;
}
static void _decor_mgr_get_decoration(struct wl_client *cli,
                                      struct wl_resource *res, uint32_t id,
                                      struct wl_resource *toplevel_res) {
    _xdg_toplevel_t *t = wl_resource_get_user_data(toplevel_res);
    if (!t || !t->surf) {
        wl_resource_post_error(res,
            ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "get_toplevel_decoration needs a valid toplevel");
        return;
    }
    _decor_t *d = vt_malloc0(sizeof(*d));
    if (!d) { wl_client_post_no_memory(cli); return; }
    d->surf = t->surf;
    struct wl_resource *r = wl_resource_create(
        cli, &zxdg_toplevel_decoration_v1_interface, 1, id);
    if (!r) { vt_free(d); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_decor_impl, d, _decor_res_destroy);
    d->res = r;
    t->surf->decor_res = r;
    /* answer with the effective mode right away */
    zxdg_toplevel_decoration_v1_send_configure(
        r, t->surf->ssd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                        : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static void _decor_mgr_destroy(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct zxdg_decoration_manager_v1_interface _decor_mgr_impl = {
    .destroy = _decor_mgr_destroy,
    .get_toplevel_decoration = _decor_mgr_get_decoration,
};
static void _bind_decor_mgr(struct wl_client *cli, void *data,
                             uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(
        cli, &zxdg_decoration_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_decor_mgr_impl, NULL, NULL);
}

/* --------------------------------------------------------- wl_output */

/* ---------------------------------------------------- wl_output bookkeeping
 *
 * wl_surface.enter(output) is a REQUIRED event ("sent when the surface
 * is shown on an output") — but this compositor never sent it. Real
 * clients wait for it before rendering: Xwayland's software path
 * creates a window's wl_shm buffer and then never attaches it, so X11
 * apps rendered NOTHING under the Wayland session (the surface never
 * entered any output). Track every bound output resource so each
 * surface can be entered into its OWN client's output object. */
#define _MAX_OUT_RES 16
static struct wl_resource *_out_res[_MAX_OUT_RES];
static size_t _n_out_res;

static void _out_res_destroy(struct wl_resource *res) {
    for (size_t i = 0; i < _n_out_res; i++) {
        if (_out_res[i] == res) {
            _out_res[i] = _out_res[--_n_out_res];
            return;
        }
    }
}

static struct wl_resource *_out_res_for_client(struct wl_client *cli) {
    for (size_t i = 0; i < _n_out_res; i++) {
        if (wl_resource_get_client(_out_res[i]) == cli)
            return _out_res[i];
    }
    return NULL;
}

static void _out_release(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_output_interface _out_impl = {
    .release = _out_release,
};

static void _bind_output(struct wl_client *cli, void *data, uint32_t version,
                         uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(cli, &wl_output_interface,
                                                 version < 3 ? version : 3,
                                                 id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    /* v3 clients send wl_output.release() at teardown — a NULL
     * implementation made libwayland dereference a NULL function
     * pointer and SEGFAULTED the whole compositor (foot died at exit
     * and took the session down with it) */
    wl_resource_set_implementation(res, &_out_impl, NULL, _out_res_destroy);
    if (_n_out_res < _MAX_OUT_RES)
        _out_res[_n_out_res++] = res;
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

/* enter the surface into its client's output (single-output desktop:
 * every mapped surface is on it). Idempotent on the client side —
 * wayland allows duplicate enters — but we track enter state anyway. */
static void _surface_output_enter(_wl_surf_t *s) {
    if (!s || !s->res || s->out_entered) return;
    struct wl_resource *out = _out_res_for_client(
        wl_resource_get_client(s->res));
    if (!out) return;   /* client never bound wl_output yet; re-try on
                         * the next commit after it does */
    wl_surface_send_enter(s->res, out);
    s->out_entered = true;
    /* wl_surface v6 preferred scale/transform: a v6 client that never
     * hears from us falls back to wl_output.scale (1) — but foot and
     * GTK4 treat an explicit event as the authoritative answer and
     * some builds wait for it before first render. One event at enter
     * time is the documented point ("whenever the preferred scale
     * changes"; entering the output IS that moment). */
    if (wl_resource_get_version(s->res) >= 6) {
        wl_surface_send_preferred_buffer_scale(s->res, 1);
        wl_surface_send_preferred_buffer_transform(
            s->res, WL_OUTPUT_TRANSFORM_NORMAL);
    }
}

static void _surface_output_leave(_wl_surf_t *s) {
    if (!s || !s->res || !s->out_entered) return;
    struct wl_resource *out = _out_res_for_client(
        wl_resource_get_client(s->res));
    if (out) wl_surface_send_leave(s->res, out);
    s->out_entered = false;
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

/* Built-in fallback resize sprites: 16x16 double-headed arrows for the
 * four edge orientations. Generated analytically (a shaft + two
 * triangular heads in unit space, rotated per orientation) — hand
 * -drawn bitmap diagonals are unmaintainable. White fill with a black
 * one-pixel ring (classic cursor look). Used when the Xcursor theme
 * lacks the shapes or VANTAGE_WL_CURSOR=builtin is forced: resize
 * must stay discoverable everywhere. */
static void _gen_double_arrow(uint32_t *img, int shape) {
    memset(img, 0, 64 * 64 * sizeof(uint32_t));
    const double rot[5][2] = {
        {0, 0},                                  /* unused */
        {1.0, 0.0},                              /* 1: E/W   */
        {0.0, 1.0},                              /* 2: N/S   */
        { 0.70710678,  0.70710678},              /* 3: NW-SE (\) */
        { 0.70710678, -0.70710678},              /* 4: NE-SW (/) */
    };
    double ax = rot[shape][0], ay = rot[shape][1];
    bool in[16][16] = {false};
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            double px = (x - 7.5) / 7.0;
            double py = (y - 7.5) / 7.0;
            /* u = along the arrow axis, v = perpendicular */
            double u =  px * ax + py * ay;
            double v = -px * ay + py * ax;
            double au = fabs(u);
            bool shaft = au <= 0.78 && fabs(v) <= 0.11;
            double head_half = 0.45 * (0.99 - au) / 0.21;   /* au in [0.78, 0.99] */
            bool head = au > 0.78 && au <= 0.99 && fabs(v) <= head_half;
            in[y][x] = shaft || head;
        }
    }
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            if (in[y][x]) {
                img[y * 64 + x] = 0xffffffff;    /* white body */
            } else {
                bool ring = (x > 0 && in[y][x - 1]) || (x < 15 && in[y][x + 1]) ||
                            (y > 0 && in[y - 1][x]) || (y < 15 && in[y + 1][x]);
                if (ring) img[y * 64 + x] = 0xff000000;   /* black outline */
            }
        }
    }
}

/* Copy one shape set into the ACTIVE cursor and push it to the
 * hardware plane. No-op when the shape is already active. */
static void _cursor_apply_hw(_wl_state_t *st);

static void _cursor_set_shape(_wl_state_t *st, int shape) {
    if (shape < 0 || shape > 4) shape = 0;
    if (st->cur_shape == shape) return;
    st->cur_shape = shape;
    memcpy(st->cursor_img, st->cur_shape_img[shape], sizeof(st->cursor_img));
    st->cur_img_w  = st->cur_shape_w[shape];
    st->cur_img_h  = st->cur_shape_h[shape];
    st->cur_img_hx = st->cur_shape_hx[shape];
    st->cur_img_hy = st->cur_shape_hy[shape];
    _cursor_apply_hw(st);
    st->dirty = true;   /* software sprite must re-blend this frame */
}

static void _cursor_init(_wl_state_t *st) {
    _cursor_default_arrow(st);
    /* remember the arrow as shape set 0 */
    memcpy(st->cur_shape_img[0], st->cursor_img, sizeof(st->cursor_img));
    st->cur_shape_w[0]  = st->cur_img_w;
    st->cur_shape_h[0]  = st->cur_img_h;
    st->cur_shape_hx[0] = st->cur_img_hx;
    st->cur_shape_hy[0] = st->cur_img_hy;
    st->cur_shape = 0;

    /* built-in resize arrows as the always-available fallback */
    for (int s = 1; s <= 4; s++) {
        _gen_double_arrow(st->cur_shape_img[s], s);
        st->cur_shape_w[s]  = 16;
        st->cur_shape_h[s]  = 16;
        st->cur_shape_hx[s] = 8;
        st->cur_shape_hy[s] = 8;
    }

    /* VANTAGE_WL_CURSOR=builtin forces the built-in sprites —
     * used by tests (exact pixel assertions) and as an override when a
     * theme's cursors misbehave. */
    const char *force = getenv("VANTAGE_WL_CURSOR");
    if (force && vt_streq(force, "builtin")) {
        vt_logi("wayland: cursor: built-in sprites forced "
                "(VANTAGE_WL_CURSOR=builtin)");
        return;
    }
#if defined(VT_HAVE_XCURSOR)
    const char *theme = getenv("XCURSOR_THEME");
    const char *szs = getenv("XCURSOR_SIZE");
    int size = szs && *szs ? atoi(szs) : 24;
    if (size <= 0 || size > 64) size = 24;
    /* shape 0: the plain arrow */
    static const char *const shapes[5] = {
        "left_ptr",
        "sb_h_double_arrow",       /* E/W */
        "sb_v_double_arrow",       /* N/S */
        "fd_double_arrow",         /* NW/SE (\) */
        "bd_double_arrow",         /* NE/SW (/) */
    };
    for (int s = 0; s < 5; s++) {
        XcursorImages *imgs = NULL;
        /* try the theme, then the default theme, then common aliases
         * (size_all/size_hor families exist in several themes) */
        static const char *const alias[5][3] = {
            { "left_ptr", "arrow", "default" },
            { "sb_h_double_arrow", "size_hor", "h_double_arrow" },
            { "sb_v_double_arrow", "size_ver", "v_double_arrow" },
            { "fd_double_arrow", "size_fdiag", "top_left_corner" },
            { "bd_double_arrow", "size_bdiag", "bottom_left_corner" },
        };
        for (int a = 0; a < 3 && !imgs; a++)
            imgs = XcursorLibraryLoadImages(alias[s][a],
                                            theme && *theme ? theme : "default",
                                            size);
        if (!imgs && theme && *theme)
            for (int a = 0; a < 3 && !imgs; a++)
                imgs = XcursorLibraryLoadImages(alias[s][a], "default", size);
        if (imgs && imgs->nimage > 0) {
            XcursorImage *im = imgs->images[0];
            if (im && im->width <= 64 && im->height <= 64 && im->pixels) {
                memset(st->cur_shape_img[s], 0, sizeof(st->cur_shape_img[s]));
                for (uint32_t y = 0; y < im->height; y++)
                    for (uint32_t x = 0; x < im->width; x++)
                        st->cur_shape_img[s][y * 64 + x] =
                            ((const uint32_t *)im->pixels)[y * im->width + x];
                st->cur_shape_w[s]  = (int)im->width;
                st->cur_shape_h[s]  = (int)im->height;
                st->cur_shape_hx[s] = (int)im->xhot;
                st->cur_shape_hy[s] = (int)im->yhot;
                vt_logd("wayland: cursor: '%s' %ux%u hotspot %u,%u",
                        shapes[s], im->width, im->height, im->xhot, im->yhot);
            }
            XcursorImagesDestroy(imgs);
        }
        /* missing theme shape: the built-in fallback already in place */
    }
    if (st->cur_shape_w[1] && st->cur_shape_w[1] != 16)
        vt_logi("wayland: cursor: Xcursor resize shapes loaded "
                "(theme '%s')", theme && *theme ? theme : "default");
    else
        vt_logi("wayland: cursor: resize shapes: built-in arrows "
                "(theme '%s' lacks them)", theme && *theme ? theme : "default");
    /* re-apply shape 0 from the (possibly themed) set */
    memcpy(st->cursor_img, st->cur_shape_img[0], sizeof(st->cursor_img));
    st->cur_img_w  = st->cur_shape_w[0];
    st->cur_img_h  = st->cur_shape_h[0];
    st->cur_img_hx = st->cur_shape_hx[0];
    st->cur_img_hy = st->cur_shape_hy[0];
#else
    vt_logi("wayland: cursor: built without Xcursor — built-in arrow "
            "sprite");
#endif
}

/* push the active cursor image to the hardware plane when available */
static void _cursor_apply_hw(_wl_state_t *st) {
    if (!st || !st->kms) return;
    /* an active resize/edge shape ALWAYS wins over the client cursor:
     * the frame band belongs to the compositor, not the app */
    if (st->cur_shape != 0) {
        /* compositor resize shape — our image, straight to the plane */
    } else if (st->cur_client_set && st->cursor_surf) {
        /* client cursor SURFACE: software sprite owns the pointer —
         * hide the plane even before the sprite's buffer commits (a
         * briefly invisible cursor during the swap is the standard
         * compositor behavior; leaving the plane up made the compositor
         * arrow linger UNDER/AROUND the client cursor) */
        vt_kms_cursor_hide(st->kms);
        return;
    }
    /* everything else — the compositor arrow OR a cursor-shape-v1
     * named shape the client asked for (WE rendered it from the
     * Xcursor theme into cursor_img) — is OUR pixels: hardware plane */
    if (!vt_kms_cursor_set(st->kms, st->cursor_img,
                           st->cur_img_w, st->cur_img_h, 64)) {
        /* drmModeSetCursor failed (quirky drivers): the software sprite
         * in _paint takes over automatically (cursor_active == false),
         * and dirty forces the repaint that blends it. */
        st->dirty = true;
    }
}

/* ------------------------------------------------ cursor-shape-v1 hooks */

/* cursor-shape enum → Xcursor name (plus a small alias chain). The
 * compositor owns named-shape rendering, so every toolkit gets the
 * SAME theme cursor — no more per-app fallback icons as pointers
 * (the "some applications fall back to whatever cursor they can
 * find" report, and the oversized bluish cursor it produced). */
static const char *const _shape_names[][3] = {
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT]        = { "left_ptr", "arrow", "default" },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU]   = { "context-menu", "left_ptr", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP]           = { "question_arrow", "help", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER]        = { "pointer", "hand", "hand2" },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS]       = { "left_ptr_watch", "watch", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT]           = { "watch", "left_ptr_watch", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL]           = { "cross", "plus", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR]      = { "crosshair", "cross", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT]           = { "xterm", "text", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT]  = { "vertical-text", "xterm", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS]          = { "dnd-link", "link", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY]           = { "dnd-copy", "copy", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE]           = { "dnd-move", "move", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP]        = { "dnd-no-drop", "forbidden", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED]    = { "not-allowed", "crossed_circle", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB]           = { "openhand", "grab", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING]       = { "closedhand", "grabbing", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE]       = { "right_side", "e-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE]       = { "top_side", "n-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE]      = { "top_right_corner", "ne-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE]      = { "top_left_corner", "nw-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE]       = { "bottom_side", "s-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE]      = { "bottom_right_corner", "se-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE]      = { "bottom_left_corner", "sw-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE]       = { "left_side", "w-resize", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE]      = { "sb_h_double_arrow", "size_hor", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE]      = { "sb_v_double_arrow", "size_ver", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE]    = { "fd_double_arrow", "size_bdiag", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE]    = { "bd_double_arrow", "size_fdiag", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE]     = { "split_h", "sb_h_double_arrow", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE]     = { "split_v", "sb_v_double_arrow", NULL },
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL]     = { "fleur", "all-scroll", NULL },
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE]     = { "fleur", "all-scroll", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK]        = { "dnd-ask", "question_arrow", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN]        = { "zoom-in", "zoom_in", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT]       = { "zoom-out", "zoom_out", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_UP_DOWN
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_UP_DOWN]        = { "center_ptr", "sb_v_double_arrow", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_LEFT_RIGHT
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_LEFT_RIGHT]     = { "center_ptr", "sb_h_double_arrow", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TOP_LEFT_CORNER
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TOP_LEFT_CORNER]  = { "top_left_corner", "nw-resize", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TOP_RIGHT_CORNER
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TOP_RIGHT_CORNER] = { "top_right_corner", "ne-resize", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_BOTTOM_RIGHT_CORNER
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_BOTTOM_RIGHT_CORNER] = { "bottom_right_corner", "se-resize", NULL },
#endif
#ifdef WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_BOTTOM_LEFT_CORNER
    [WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_BOTTOM_LEFT_CORNER]  = { "bottom_left_corner", "sw-resize", NULL },
#endif
};

/* loaded-shape cache: shape → 64x64 cell + metrics. Lazy: only shapes
 * actually used get loaded (and only once). */
#define _MAX_SHAPES 64
typedef struct { bool valid; int w, h, hx, hy; uint32_t px[64 * 64]; }
    _shape_cache_t;
static _shape_cache_t _shape_cache[_MAX_SHAPES];

/* the ACTIVE cursor image slot + hardware plane update. The source is
 * a 64-stride cell (same layout as cursor_img itself). */
static void _cursor_set_client_img(const uint32_t *cell, int w, int h,
                                    int hx, int hy) {
    _wl_state_t *st = _wls;
    if (!st) return;
    memcpy(st->cursor_img, cell, sizeof(st->cursor_img));
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    st->cur_img_w = w;
    st->cur_img_h = h;
    st->cur_img_hx = hx;
    st->cur_img_hy = hy;
    _cursor_apply_hw(st);
    st->dirty = true;
}

void _cursor_client_shape(uint32_t shape) {
    _wl_state_t *st = _wls;
    if (!st) return;
    if (shape == 0 || shape >= sizeof(_shape_names) / sizeof(_shape_names[0]))
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    /* remember: a client-named shape is active (clears any surface) */
    st->client_shape = shape;
    st->cur_client_set = true;
    st->cursor_surf = NULL;
    if (st->cur_shape != 0)
        return;   /* compositor resize shape wins while it is active */
    _shape_cache_t *c = &_shape_cache[shape % _MAX_SHAPES];
    if (!c->valid) {
        c->valid = true;
        c->w = c->h = 0;
        bool loaded = false;
#if defined(VT_HAVE_XCURSOR)
        const char *theme = getenv("XCURSOR_THEME");
        const char *szs = getenv("XCURSOR_SIZE");
        int size = szs && *szs ? atoi(szs) : 24;
        if (size <= 0 || size > 64) size = 24;
        for (int a = 0; a < 3 && !loaded; a++) {
            const char *name = _shape_names[shape][a];
            if (!name) break;
            XcursorImages *imgs = XcursorLibraryLoadImages(
                name, theme && *theme ? theme : "default", size);
            if (!imgs && theme && *theme)
                imgs = XcursorLibraryLoadImages(name, "default", size);
            if (imgs && imgs->nimage > 0) {
                XcursorImage *im = imgs->images[0];
                if (im && im->width <= 64 && im->height <= 64 &&
                    im->pixels) {
                    memset(c->px, 0, sizeof(c->px));
                    for (uint32_t y = 0; y < im->height; y++)
                        for (uint32_t x = 0; x < im->width; x++)
                            c->px[y * 64 + x] =
                                ((const uint32_t *)im->pixels)[y * im->width + x];
                    c->w = (int)im->width;
                    c->h = (int)im->height;
                    c->hx = (int)im->xhot;
                    c->hy = (int)im->yhot;
                    loaded = true;
                }
                XcursorImagesDestroy(imgs);
            }
        }
#endif
        if (!loaded) {
            /* no theme art: fall back to the built-in arrow set (and
             * the built-in resize arrows for resize shapes) */
            int fallback = 0;
            if (shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE &&
                shape <= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE)
                fallback = (shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE)
                               ? 1
                               : (shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE)
                                     ? 2
                                     : (shape == WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE)
                                           ? 4 : 3;
            memcpy(c->px, _wls->cur_shape_img[fallback], sizeof(c->px));
            c->w = _wls->cur_shape_w[fallback];
            c->h = _wls->cur_shape_h[fallback];
            c->hx = _wls->cur_shape_hx[fallback];
            c->hy = _wls->cur_shape_hy[fallback];
        }
    }
    _cursor_set_client_img(c->px, c->w, c->h, c->hx, c->hy);
}

/* the pointer left the client (or the client cleared its cursor):
 * restore the COMPOSITOR cursor. The old code never released the
 * client cursor on leave — the last app's cursor surface lingered
 * over the panel and every other window ("the cursor gets bigger and
 * the aura comes back over the bar": the lingering image was some
 * app's oversized fallback icon). */
void _cursor_client_release(void) {
    _wl_state_t *st = _wls;
    if (!st) return;
    st->cur_client_set = false;
    st->cursor_surf = NULL;
    st->client_shape = 0;
    /* restore the compositor image for the active shape (arrow or
     * resize affordance) */
    memcpy(st->cursor_img, st->cur_shape_img[st->cur_shape],
           sizeof(st->cursor_img));
    st->cur_img_w = st->cur_shape_w[st->cur_shape];
    st->cur_img_h = st->cur_shape_h[st->cur_shape];
    st->cur_img_hx = st->cur_shape_hx[st->cur_shape];
    st->cur_img_hy = st->cur_shape_hy[st->cur_shape];
    _cursor_apply_hw(st);
    st->dirty = true;
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
    /* surfaces list is bottom→top: iterate reversed */
    wl_list_for_each_reverse(s, &st->surfaces, link) {
        if (!s->mapped || s->is_cursor || s->minimized) continue;
        if (s->toplevel && s->ws != st->ws_cur) continue;
        /* Input containment is region-aware (set_input_region) and
         * geometry-aware (set_window_geometry): clicks never land in
         * the transparent shadow band around CSD windows — that band
         * was the reported "black region that behaves like a solid
         * part of the window". */
        if (_surf_input_contains(s, x, y)) {
            found = s;
            break;
        }
        /* SSD frame counts as the window for input too */
        if (s->ssd && s->toplevel) {
            int fx, fy, fw, fh;
            _ssd_frame_geom(s, &fx, &fy, &fw, &fh);
            if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) {
                found = s;
                break;
            }
        }
    }
    return found;
}

void _pointer_focus_update(_wl_state_t *st, bool force) {
    _wl_surf_t *s = _surface_at(st, st->cursor_x, st->cursor_y);
    /* INTERACTIVE OP GRAB: while a move/resize op runs on a surface,
     * the pointer focus is PINNED to it. The op is an implicit grab —
     * the pointer may travel outside the client's last-committed
     * input region (a resize GROWS the window under the drag; the
     * client re-commits the matching region only after applying the
     * configure). Recomputing the focus mid-op sent wl_pointer.leave
     * to the resizing client and its toolkit then dropped the
     * drag-ending button release as out-of-order — the button state
     * stuck DOWN, the next click read as a double-press and GTK's
     * margin gestures wedged after every edge resize. */
    if (st->op_active && st->op_surf && st->op_surf->res &&
        st->op_surf->mapped && !st->op_surf->minimized)
        s = st->op_surf;
    if (s == st->ptr_focus && !force) return;
    /* leave old */
    if (st->ptr_focus && st->ptr_focus->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(st->ptr_focus->res)) {
                wl_pointer_send_leave(pr->res, ++st->serial,
                                      st->ptr_focus->res);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
        /* release the client cursor: the wl_pointer contract is that
         * the client re-sends set_cursor on the next enter. Keeping
         * the old client's cursor made the LAST app's pointer image
         * linger over every other surface — over the panel that read
         * as "the cursor gets bigger and the aura comes back". */
        if (st->cur_client_set)
            _cursor_client_release();
    }
    st->ptr_focus = s;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                int sx = s->x, sy = s->y;
                if (s->popup && s->popup->parent) {
                    sx = s->popup->parent->x + s->popup->rel_x;
                    sy = s->popup->parent->y + s->popup->rel_y;
                }
                wl_pointer_send_enter(pr->res, ++st->serial, s->res,
                                      (wl_fixed_t)(st->cursor_x - sx) * 256,
                                      (wl_fixed_t)(st->cursor_y - sy) * 256);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
        /* pointer enter/leave only — the keyboard follows CLICKS, not
         * the pointer. Focusing on hover both violates the WM's
         * click-to-focus default AND rips the keyboard focus out of an
         * open grabbed popup the moment the pointer wanders off it
         * (GTK popovers close themselves on focus loss: menus would
         * flash shut while the user was still scrolling them). */
    }
}

/* click-to-focus: called on button PRESS only. */
static void _click_to_focus(_wl_state_t *st, _wl_surf_t *s) {
    if (!s || !s->res) return;
    /* a grabbed popup owns the keyboard until it is dismissed — never
     * steal its focus from it (the dismissal path in _pointer_button
     * handles clicks that land outside) */
    if (st->kbd_focus && st->kbd_focus->popup &&
        st->kbd_focus->popup->grabbed)
        return;
    /* docked layer surfaces take focus ONLY when they asked for keys
     * (keyboard mode on-demand/exclusive); a passive dock never does */
    if (s->layer && !_layer_wants_kbd(s))
        return;
    /* an exclusive-keyboard layer surface (lock screen) keeps the keys */
    _wl_surf_t *_ls;
    wl_list_for_each(_ls, &st->surfaces, link) {
        if (_ls != s && _layer_kbd_exclusive(_ls)) return;
    }
    if (st->kbd_focus == s) return;
    if (s->xwl)
        _xwl_focus_changed(st, s);
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
        _activate_toplevel(st, s->toplevel);
    _broadcast_selection(st);
    _ps_broadcast(st);
}

/* xdg-activation equivalent of a focus click (exported for
 * vt-wl-protocols.c): apps "activate" their own window when a token
 * they hold is redeemed — taskbar-like focus from the client side */
void _focus_surface(_wl_surf_t *s) {
    _wl_state_t *st = _wls;
    if (!st || !s || !s->res) return;
    _click_to_focus(st, s);
}

/* EXCLUSIVE activation bookkeeping: mark `t` as THE focused window,
 * clear every other toplevel, emit WIN_FOCUS for each change. Native
 * Wayland windows previously NEVER got t->activated set (the taskbar
 * never showed them as active) while XWayland windows never got it
 * cleared (ALL of them showed active) — the focus state was wrong on
 * both sides of the session. */
static void _activate_toplevel(_wl_state_t *st, _xdg_toplevel_t *t) {
    if (!st || !t) return;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel != t && s->toplevel->activated) {
            s->toplevel->activated = false;
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, s->toplevel);
        }
    }
    if (!t->activated) {
        t->activated = true;
        _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, t);
    }
}

/* Edge bits under the cursor for an SSD-framed toplevel:
 * 1=E 2=S 4=W 8=N (same encoding the X11 WM uses). 0 = not on an
 * edge. The grab zone is the visible 2px border plus a few pixels of
 * the client edge; the TOP strip lives on the title band (rows 0..
 * margin-1 of the frame) so a top-edge grab is actually reachable. */
static uint8_t _ssd_edge_at(_wl_state_t *st, _wl_surf_t *s) {
    if (!s || !s->ssd || !s->toplevel || s->minimized) return 0;
    /* fullscreen windows have NO frame (the client covers everything) */
    if (s->toplevel->fullscreen) return 0;
    int fx, fy, fw, fh;
    _ssd_frame_geom(s, &fx, &fy, &fw, &fh);
    int lx = st->cursor_x - fx, ly = st->cursor_y - fy;
    if (lx < 0 || lx >= fw || ly < 0 || ly >= fh) return 0;
    int m = _WL_SSD_RESIZE_MARGIN;
    bool t = ly < m;                       /* title band top strip */
    bool b = ly >= fh - m;
    /* side strips below the title band; at the very top they extend
     * INTO the title band so the top CORNERS grab both edges */
    bool l = lx < m && (ly > _WL_SSD_TITLE || t);
    bool r = lx >= fw - m && (ly > _WL_SSD_TITLE || t);
    uint8_t e = 0;
    if (r) e |= 1;
    if (b) e |= 2;
    if (l) e |= 4;
    if (t) e |= 8;
    return e;
}

/* shape-set index for an edge bitmask (0 = default arrow) */
static int _edge_shape(uint8_t edges) {
    if (!edges) return 0;
    bool e = edges & 1, s = edges & 2, w = edges & 4, n = edges & 8;
    if ((e && w) || (n && s)) return 1;        /* E/W shaft   */
    if ((n && e) || (s && w)) return 4;        /* NE/SW (/)   */
    if ((n && w) || (s && e)) return 3;        /* NW/SE (\)   */
    if (e || w) return 1;
    if (n || s) return 2;
    return 0;
}

static void _pointer_motion(_wl_state_t *st, double dx, double dy) {
    st->cursor_x += (int)dx;
    st->cursor_y += (int)dy;
    if (st->cursor_x < 0) st->cursor_x = 0;
    if (st->cursor_y < 0) st->cursor_y = 0;
    if (st->cursor_x >= st->out_w) st->cursor_x = st->out_w - 1;
    if (st->cursor_y >= st->out_h) st->cursor_y = st->out_h - 1;
    if (st->kms && vt_kms_cursor_active(st->kms))
        vt_kms_cursor_move(st->kms, st->cursor_x - st->cur_img_hx,
                           st->cursor_y - st->cur_img_hy);
    _pointer_focus_update(st, false);
    _wl_surf_t *s = st->ptr_focus;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                int sx = s->x, sy = s->y;
                if (s->popup && s->popup->parent) {
                    sx = s->popup->parent->x + s->popup->rel_x;
                    sy = s->popup->parent->y + s->popup->rel_y;
                }
                wl_pointer_send_motion(pr->res, 0,
                    (wl_fixed_t)(st->cursor_x - sx) * 256,
                    (wl_fixed_t)(st->cursor_y - sy) * 256);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
    }
    /* cursor shape: resize affordance over SSD edges, kept during the
     * whole interactive op; plain arrow everywhere else. THIS is what
     * makes the frame borders discoverable as resize handles (the
     * reported "black bars look like a resize area but resizing does
     * not seem to be implemented"). */
    if (st->op_active && st->op_resize)
        _cursor_set_shape(st, _edge_shape(st->op_edges));
    else
        _cursor_set_shape(st, _edge_shape(_ssd_edge_at(st, s)));

    /* interactive move/resize */
    if (st->op_active && st->op_surf) {
        if (st->op_resize) {
            /* EDGE-AWARE resize: dragging the WEST/NORTH edges moves
             * the origin as the size changes (the old math always
             * behaved as a bottom-right corner grab, so top/left edges
             * resized "backwards" and felt broken). Client size hints
             * (xdg_toplevel.set_min_size/set_max_size) are honored. */
            _wl_surf_t *os = st->op_surf;
            _xdg_toplevel_t *t = os->toplevel;
            int rdx = st->cursor_x - st->op_grab_x;
            int rdy = st->cursor_y - st->op_grab_y;
            int w = st->op_start_w, h = st->op_start_h;
            int x = st->op_start_x, y = st->op_start_y;
            if (st->op_edges & 1) w = st->op_start_w + rdx;
            if (st->op_edges & 2) h = st->op_start_h + rdy;
            if (st->op_edges & 4) { w = st->op_start_w - rdx; x = st->op_start_x + rdx; }
            if (st->op_edges & 8) { h = st->op_start_h - rdy; y = st->op_start_y + rdy; }
            int minw = (t && t->min_w > 0) ? t->min_w : 20;
            int minh = (t && t->min_h > 0) ? t->min_h : 20;
            if (w < minw) { if (st->op_edges & 4) x -= minw - w; w = minw; }
            if (h < minh) { if (st->op_edges & 8) y -= minh - h; h = minh; }
            if (t && t->max_w > 0 && w > t->max_w) {
                if (st->op_edges & 4) x += w - t->max_w;
                w = t->max_w;
            }
            if (t && t->max_h > 0 && h > t->max_h) {
                if (st->op_edges & 8) y += h - t->max_h;
                h = t->max_h;
            }
            /* remember the configure-side CONTENT size for the
             * release-time final configure (os->w is the live BUFFER
             * size once the client re-commits — see op_last_cw) */
            st->op_last_cw = w;
            st->op_last_ch = h;
            os->w = w;
            os->h = h;
            os->x = x;
            os->y = y;
            if (os->xwl) {
                _xwl_move_resize(os, os->x, os->y, os->w, os->h);
            }
            /* geometry events + the (now COMPLETE) resize configure at
             * the same throttled cadence as move events: a real toolkit
             * answers every configure with an ack + a fresh buffer, so
             * an unthrottled send would flood the client with resizes
             * mid-drag. The FINAL configure goes out on button release. */
            if (t && !os->xwl) {
                uint64_t now = vt_time_now_us();
                if (now - st->op_last_geo_us >= _WL_GEO_EVENT_INTERVAL_US) {
                    st->op_last_geo_us = now;
                    _toplevel_configure(t, os->w, os->h,
                                        XDG_TOPLEVEL_STATE_RESIZING);
                    _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, t);
                }
            } else if (t) {
                uint64_t now = vt_time_now_us();
                if (now - st->op_last_geo_us >= _WL_GEO_EVENT_INTERVAL_US) {
                    st->op_last_geo_us = now;
                    _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, t);
                }
            }
        } else {
            st->op_surf->x = st->cursor_x - st->op_grab_x;
            st->op_surf->y = st->cursor_y - st->op_grab_y;
            /* Keep the title bar REACHABLE: a window dragged all the
             * way up disappears behind the top panel (its exclusive
             * zone is not usable space) and there is nothing left to
             * grab to pull it back down. Clamp interactive moves to
             * the workarea top — and clamp the FRAME, not the client:
             * the SSD title band (26px title + 2px border) sits ABOVE
             * the client surface, so a client clamped to the bare
             * workarea top still parks its grab bar behind the panel
             * (the off-by-a-title-bar version of exactly that bug). */
            int wx = 0, wy = 0, ww = 0, wh = 0;
            _layer_workarea(st, &wx, &wy, &ww, &wh);
            if (st->op_surf->x < 0) st->op_surf->x = 0;
            int ymin = wy + (st->op_surf->ssd
                                 ? (_WL_SSD_TITLE + _WL_SSD_BORDER) : 0);
            if (st->op_surf->y < ymin) st->op_surf->y = ymin;
            /* Geometry events during a drag: CONTINUOUS at ~30 fps.
             * The panel consumes them event-driven now (its GLib loop
             * watches the WM socket), so the pager follows the drag
             * frame by frame; the FINAL geometry still goes out on
             * button release. The rate is bounded so a 1000 Hz pointer
             * cannot flood the panel's input queue. */
            if (st->op_surf->toplevel) {
                uint64_t now = vt_time_now_us();
                if (now - st->op_last_geo_us >= _WL_GEO_EVENT_INTERVAL_US) {
                    st->op_last_geo_us = now;
                    _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY,
                              st->op_surf->toplevel);
                }
            }
            if (st->op_surf->xwl)
                _xwl_move_resize(st->op_surf, st->op_surf->x,
                                 st->op_surf->y, 0, 0);
        }
    }
    st->dirty = true;
}

/* content size of a surface (set_window_geometry aware) — interactive
 * resize must track the WINDOW size, not the buffer: CSD margins
 * added ~28px to every Super+drag start and the client then re-added
 * its margins on top of the configured size (slow window growth per
 * drag). */
static inline int _surf_cw(const _wl_surf_t *s) {
    return (s->have_win_geo && s->win_gw > 0 && s->win_gw <= s->w)
               ? s->win_gw : s->w;
}
static inline int _surf_ch(const _wl_surf_t *s) {
    return (s->have_win_geo && s->win_gh > 0 && s->win_gh <= s->h)
               ? s->win_gh : s->h;
}

static void _pointer_button(_wl_state_t *st, uint32_t button,
                            bool pressed) {
    if (!pressed && (button == 0x110 || button == 0x111 || button == 0x112)) {
        /* ANY button release ends an interactive op: a Super+right-drag
         * resize used to keep running until some later left click.
         * Emit the FINAL geometry (motion is rate-limited; the last
         * position must reach the WM model and the pager even when the
         * last motion was skipped) and drop the xdg resizing state. */
        if (st->op_active && st->op_surf && st->op_surf->toplevel) {
            _xdg_toplevel_t *t = st->op_surf->toplevel;
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, t);
            if (t->resizing) {
                t->resizing = false;
                /* the FINAL resize configure — with the complete
                 * handshake this is what actually lands the new size
                 * in a real toolkit (mid-drag configures are throttled
                 * and may have skipped the last few pixels). Send the
                 * CONTENT math recorded by _op_motion (op_last_cw/ch):
                 * re-reading op_surf->w here sends the client's live
                 * BUFFER dims (content + CSD margins) — every CSD
                 * edge-drag then grew the window by its shadow
                 * margins (+28/+29 observed with GTK4). */
                int fw = (st->op_last_cw > 0) ? st->op_last_cw
                                              : _surf_cw(st->op_surf);
                int fh = (st->op_last_ch > 0) ? st->op_last_ch
                                              : _surf_ch(st->op_surf);
                if (t->res)
                    _toplevel_configure(t, fw, fh,
                                        t->activated
                                            ? XDG_TOPLEVEL_STATE_ACTIVATED
                                            : 0);
                st->op_last_cw = 0;
                st->op_last_ch = 0;
            }
        }
        st->op_active = false;
        _cursor_set_shape(st, _edge_shape(_ssd_edge_at(st, st->ptr_focus)));
    }
    if (st->op_active && pressed) {
        /* a second press while an interactive op runs is consumed by it */
        st->press_consumed = true;
        st->press_surf = NULL;
        return;
    }
    /* a grabbed popup is dismissed when the press is NOT inside it or
     * its parent chain (menus close when you click elsewhere) */
    if (pressed) {
        _wl_surf_t *top;
        wl_list_for_each_reverse(top, &st->surfaces, link) {
            if (top->popup && top->mapped && top->popup->grabbed) {
                bool inside =
                    (top == st->ptr_focus);
                _wl_surf_t *anc = top->popup->parent;
                while (!inside && anc) {
                    if (anc == st->ptr_focus) inside = true;
                    anc = anc->popup ? anc->popup->parent : NULL;
                }
                if (!inside) {
                    _popup_done(st, top);
                    st->press_consumed = true;   /* dismiss click consumed */
                    st->press_surf = NULL;
                    return;
                }
                break;
            }
            if (top->mapped && !top->popup) break; /* top-most non-popup */
        }
    }
    /* RELEASE FORWARDING — under IMPLICIT-GRAB rules, and OUTSIDE the
     * current-pointer-focus gate: a release belongs to the surface that
     * received the PRESS (press_surf), never to whatever is under the
     * cursor at release time. During an interactive resize the pointer
     * can sit OUTSIDE the client's last-committed input region (the
     * window is growing under the drag; the client re-commits the new
     * region only after it applies the configure) — gating on
     * ptr_focus then dropped the release on the floor, leaving the
     * toolkit's button state stuck DOWN (the next click in the window
     * arrived as a double-press and GTK's margin gestures wedged). */
    if (!pressed) {
        bool consumed = st->press_consumed;
        _wl_surf_t *target = st->press_surf;
        st->press_consumed = false;
        st->press_surf = NULL;
        if (consumed)
            return;                   /* release pairs with a consumed press */
        if (!target || !target->res)
            return;                   /* no forwarded press ever happened */
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(target->res)) {
                wl_pointer_send_button(pr->res, ++st->serial, 0, button,
                                       WL_POINTER_BUTTON_STATE_RELEASED);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
        return;
    }
    _wl_surf_t *s = st->ptr_focus;
    if (s && s->res) {
        /* Super+drag: move/resize ANY window (also CSD-less ones that
         * have no titlebar to drag) — the daily-driver escape hatch */
        if (pressed && (st->mods_depressed & 0x40) && s->toplevel) {
            if (button == 0x110) {
                st->op_active = true;
                st->op_resize = false;
                st->op_edges = 0;
                st->op_surf = s;
                st->op_grab_x = st->cursor_x - s->x;
                st->op_grab_y = st->cursor_y - s->y;
                st->op_start_x = s->x;
                st->op_start_y = s->y;
                st->press_consumed = true;
                st->press_surf = NULL;
                return;
            } else if (button == 0x111) {
                /* edge picked from which half of the WINDOW the
                 * pointer is in — Super+right-drag in the left half
                 * resizes the LEFT edge, not always the bottom-right
                 * corner. The halves are computed on the CONTENT rect:
                 * CSD shadow margins are asymmetric (e.g. 12 top /
                 * 17 bottom), so the BUFFER's center is not the
                 * window's visual center — a center-of-window grab
                 * picked the NORTH edge and the drag INVERTED the
                 * height (300 -> 190 for a +110 pull).
                 * 0x111 is BTN_RIGHT (linux/input-event-codes.h); the
                 * old 0x112 test was BTN_MIDDLE — Super+right-drag did
                 * NOTHING on real hardware (libinput sends 0x111) and
                 * only worked in harnesses whose own test-input mapping
                 * shared the same inverted code. */
                int ccx, ccy, ccw, cch;
                _win_content_rect(s, &ccx, &ccy, &ccw, &cch);
                uint8_t e = 0;
                if (st->cursor_x < ccx + ccw / 2) e |= 4; else e |= 1;
                if (st->cursor_y < ccy + cch / 2) e |= 8; else e |= 2;
                st->op_active = true;
                st->op_resize = true;
                st->op_edges = e;
                st->op_surf = s;
                st->op_grab_x = st->cursor_x;
                st->op_grab_y = st->cursor_y;
                st->op_start_x = s->x;
                st->op_start_y = s->y;
                st->op_start_w = _surf_cw(s);
                st->op_start_h = _surf_ch(s);
                st->op_last_cw = st->op_start_w;
                st->op_last_ch = st->op_start_h;
                st->op_last_geo_us = 0;
                st->press_consumed = true;
                st->press_surf = NULL;
                return;
            }
        }
        /* click-to-focus on PRESS (the keyboard never follows motion) */
        if (pressed)
            _click_to_focus(st, s);
        /* SSD frame interactions (buttons, edges, titlebar drag) */
        if (s->ssd && s->toplevel && pressed && button == 0x110) {
            int fx, fy, fw, fh;
            _ssd_frame_geom(s, &fx, &fy, &fw, &fh);
            int lx = st->cursor_x - fx, ly = st->cursor_y - fy;
            if (lx >= 0 && lx < fw && ly >= 0 && ly < fh) {
                int bx = fw - _WL_SSD_BORDER - _WL_SSD_BTN;
                if (ly <= _WL_SSD_TITLE + _WL_SSD_BORDER) {
                    /* window buttons come FIRST: their hit boxes sit in
                     * the title band and must never be eaten by the
                     * top-edge resize strip. XWayland windows have NO
                     * xdg_toplevel resource (their toplevel is a
                     * synthetic WM-side record) — calling the xdg
                     * sends with res == NULL SEGFAULTED the whole
                     * compositor on the first × / ▢ click. Route X11
                     * windows through their X-side operations. */
                    if (lx >= bx) {
                        /* close */
                        if (s->xwl) {
                            _xwl_close(s);
                        } else if (s->toplevel->res) {
                            xdg_toplevel_send_close(s->toplevel->res);
                        }
                        st->press_consumed = true;
                        st->press_surf = NULL;
                        return;
                    } else if (lx >= bx - (_WL_SSD_BTN + 2)) {
                        /* maximize toggle: the FRAME fills the WORKAREA
                         * (the old full-output configure covered the
                         * docked panel and pushed the × button
                         * off-screen). */
                        if (s->xwl) {
                            _xwl_maximize(s, !s->toplevel->maximized);
                        } else {
                            _wl_maximize_apply(s->toplevel,
                                               !s->toplevel->maximized);
                        }
                        st->press_consumed = true;
                        st->press_surf = NULL;
                        return;
                    } else if (lx >= bx - 2 * (_WL_SSD_BTN + 2)) {
                        /* minimize via our own SSD button */
                        s->toplevel->minimized = true;
                        s->minimized = true;
                        if (s->xwl) {
                            /* X-side unmap (the WM-side flag alone left
                             * the X window mapped and visible) */
                            _xwl_minimize(s, true);
                            st->press_consumed = true;
                            st->press_surf = NULL;
                            return;
                        }
                        if (st->kbd_focus == s) st->kbd_focus = NULL;
                        st->dirty = true;
                        _emit_win(st, VT_BACKEND_WL_EVENT_WIN_STATE,
                                  s->toplevel);
                        st->press_consumed = true;
                        st->press_surf = NULL;
                        return;
                    }
                }
                /* EDGES/CORNERS → resize. Checked BEFORE the plain
                 * titlebar drag: the top strip lives on the title band
                 * (rows 0..margin-1) and must win over "drag the
                 * titlebar". This is the resize handle users see (the
                 * cursor changes over it). */
                uint8_t edges = _ssd_edge_at(st, s);
                if (edges) {
                    st->op_active = true;
                    st->op_resize = true;
                    st->op_edges = edges;
                    st->op_surf = s;
                    st->op_grab_x = st->cursor_x;
                    st->op_grab_y = st->cursor_y;
                    st->op_start_x = s->x;
                    st->op_start_y = s->y;
                    st->op_start_w = _surf_cw(s);
                    st->op_start_h = _surf_ch(s);
                    st->op_last_cw = st->op_start_w;
                    st->op_last_ch = st->op_start_h;
                    st->op_last_geo_us = 0;
                    if (s->toplevel && s->toplevel->res && !s->xwl) {
                        s->toplevel->resizing = true;
                        _toplevel_configure(s->toplevel, s->w, s->h,
                                            XDG_TOPLEVEL_STATE_RESIZING);
                    }
                    st->press_consumed = true;
                    st->press_surf = NULL;
                    return;
                }
                /* titlebar drag → move */
                if (ly <= _WL_SSD_TITLE + _WL_SSD_BORDER) {
                    st->op_active = true;
                    st->op_resize = false;
                    st->op_edges = 0;
                    st->op_surf = s;
                    st->op_grab_x = st->cursor_x - s->x;
                    st->op_grab_y = st->cursor_y - s->y;
                    st->op_start_x = s->x;
                    st->op_start_y = s->y;
                    st->press_consumed = true;
                    st->press_surf = NULL;
                    return;
                }
            }
        }
        st->press_surf = s;
        st->press_consumed = false;
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                wl_pointer_send_button(pr->res, ++st->serial, 0, button,
                                       WL_POINTER_BUTTON_STATE_PRESSED);
                if (wl_resource_get_version(pr->res) >=
                    WL_POINTER_FRAME_SINCE_VERSION)
                    wl_pointer_send_frame(pr->res);
            }
        }
        return;
    }
    /* no surface under the pointer: the press is consumed by the
     * compositor (background/desktop click) so its release cannot
     * become a stray release for an unrelated client later */
    if (pressed) {
        st->press_consumed = true;
        st->press_surf = NULL;
    } else {
        st->press_consumed = false;
        st->press_surf = NULL;
    }
}

/* Pointer axis (wheel/scroll) forwarding. THE ENCODING MATTERS:
 * GTK4 accumulates axis deltas and emits its scroll event at
 * wl_pointer.frame — but a bare "axis + axis_stop + frame" batch makes
 * GDK drop the delta (verified empirically: a 15px scroll produced a
 * 2-pixel frame diff). The wheel protocol that real compositors send —
 * and GDK fully honors — is:
 *
 *     axis_source(WHEEL)
 *     axis_value120(±120 per detent)   [pointer v8+, GTK4 preferred]
 *     axis(t, VERTICAL, ±px)           [legacy value for older clients]
 *     axis_stop(t, VERTICAL)           [pointer v9+]
 *     frame()
 *
 * v5..v7 clients get axis_discrete(±1) instead of value120. Finger/
 * touchpad sources (continuous, no detents) send axis + frame with no
 * stop — a stop would terminate the kinetic sequence mid-gesture. */
static void _pointer_axis(_wl_state_t *st, double value, bool wheel,
                          bool horizontal) {
    _wl_surf_t *s = st->ptr_focus;
    uint32_t axis = horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                               : WL_POINTER_AXIS_VERTICAL_SCROLL;
    if (s && s->res) {
        _ptr_res_t *pr;
        wl_list_for_each(pr, &st->ptr_reses, link) {
            if (wl_resource_get_client(pr->res) ==
                wl_resource_get_client(s->res)) {
                uint32_t v = wl_resource_get_version(pr->res);
                if (v >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                    wl_pointer_send_axis_source(
                        pr->res,
                        wheel ? WL_POINTER_AXIS_SOURCE_WHEEL
                              : WL_POINTER_AXIS_SOURCE_FINGER);
                if (wheel && v >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
                    /* 120 units per detent, the value120 convention */
                    int units = (int)(value * 8);
                    if (units == 0) units = value > 0 ? 8 : -8;
                    wl_pointer_send_axis_value120(pr->res, axis, units);
                } else if (wheel && v >= 5) {
                    wl_pointer_send_axis_discrete(
                        pr->res, axis, value > 0 ? 1 : -1);
                }
                wl_pointer_send_axis(pr->res, 0, axis,
                                     (wl_fixed_t)(value * 256));
                if (wheel && v >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
                    wl_pointer_send_axis_stop(pr->res, 0, axis);
                if (v >= WL_POINTER_FRAME_SINCE_VERSION)
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
#if defined(VT_HAVE_XKBCOMMON)
    st->mods_depressed = xkb_state_serialize_mods(st->xkb_st,
                                                  XKB_STATE_MODS_DEPRESSED);
#endif
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
#if defined(LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)
            /* libinput >= 1.19 sends BOTH this legacy event and the new
             * SCROLL_* events for the same physical scroll — processing
             * both doubled every scroll (the "wonky/inconsistent
             * scrolling": apps scrolled twice per detent on machines
             * with newer libinput, once on older ones). When the new
             * event types exist we consume ONLY those (the documented
             * "do not mix and match" rule). */
            break;
#else
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            enum libinput_pointer_axis_source src =
                libinput_event_pointer_get_axis_source(pe);
            bool wheel = (src == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL);
            double v = libinput_event_pointer_get_axis_value(
                pe, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            if (v != 0.0)
                _pointer_axis(st, v, wheel, false);
            double hv = libinput_event_pointer_get_axis_value(
                pe, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
            if (hv != 0.0)
                _pointer_axis(st, hv, wheel, true);
            break;
#endif
        }
#if defined(LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
            struct libinput_event_pointer *pe =
                libinput_event_get_pointer_event(ev);
            bool wheel =
                (t == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL);
            bool finger =
                (t == LIBINPUT_EVENT_POINTER_SCROLL_FINGER);
            double v = libinput_event_pointer_get_axis_value(
                pe, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
            if (v != 0.0)
                _pointer_axis(st, v, wheel || !finger, false);
            double hv = libinput_event_pointer_get_axis_value(
                pe, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
            if (hv != 0.0)
                _pointer_axis(st, hv, wheel || !finger, true);
            break;
        }
#endif
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
    /* the string is owned by the caller (xkbcommon allocates it fresh
     * per call) — keeping it would leak the whole keymap text (64 KB
     * per compositor start, per LeakSanitizer) */
    free(str);
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

/* draw the compositor-side SSD frame (title bar + border + buttons) */
void _ssd_frame_geom(const _wl_surf_t *s, int *fx, int *fy,
                            int *fw, int *fh) {
    /* frame wraps the WINDOW (content) rect: a client that keeps CSD
     * margins in its buffer (GTK in SSD mode still ships shadow
     * margins) would otherwise get a frame around its SHADOW, pushing
     * the title band off the visible window. */
    int cx, cy, cw, ch;
    _win_content_rect(s, &cx, &cy, &cw, &ch);
    *fx = cx - _WL_SSD_BORDER;
    *fy = cy - (_WL_SSD_TITLE + _WL_SSD_BORDER);
    *fw = cw + 2 * _WL_SSD_BORDER;
    *fh = ch + _WL_SSD_TITLE + 2 * _WL_SSD_BORDER;
}

/* ---- SSD title text (FreeType, fontconfig "sans", 13px) --------
 * Rendered once per (title, color) and cached: titles change rarely,
 * so the cache hit rate is near 100% while dragging. */
typedef struct _title_cache {
    char *key;
    uint32_t *argb;
    int w, h;
    struct _title_cache *next;
} _title_cache_t;
static _title_cache_t *_title_cache;

/* the one shared title font (FreeType face resolved through
 * fontconfig "sans" at first use). File-scope so shutdown can
 * release it — plus fontconfig's library cache via FcFini — and a
 * clean exit is leak-free under LSan too (the ~320 B FcConfig
 * cache used to be the only report left at WM exit). */
#if defined(VT_HAVE_FREETYPE)
static FT_Library _title_ft = NULL;
static FT_Face _title_face = NULL;
static bool _title_face_tried = false;
#endif

static void _title_cache_free_all(void) {
    _title_cache_t *t = _title_cache;
    while (t) {
        _title_cache_t *n = t->next;
        vt_free(t->key);
        vt_free(t->argb);
        vt_free(t);
        t = n;
    }
    _title_cache = NULL;
}

/* release the shared title font + fontconfig's library cache at
 * shutdown — a clean exit is then fully leak-free under LSan (the
 * FcConfig cache would otherwise be the only report left). Called
 * from _wl_fini AFTER the title cache (the glyphs reference nothing
 * in the face, but release order is still face → library → fc). */
#if defined(VT_HAVE_FREETYPE)
static void _ssd_title_font_fini(void) {
    if (_title_face) { FT_Done_Face(_title_face); _title_face = NULL; }
    if (_title_ft) { FT_Done_FreeType(_title_ft); _title_ft = NULL; }
    FcFini();
}
#endif

static void _ssd_title_paint(uint32_t *fb, int W, int H,
                             int x, int y, int max_w, const char *utf8,
                             uint32_t argb, bool active) {
#if defined(VT_HAVE_FREETYPE)
    (void)active;
    /* cache key: title + color (both change the bitmap) */
    char key[256];
    snprintf(key, sizeof(key), "%08x:%.200s", argb, utf8);
    _title_cache_t *hit = NULL;
    for (_title_cache_t *t = _title_cache; t; t = t->next)
        if (strcmp(t->key, key) == 0) { hit = t; break; }
    if (!hit) {
        if (!_title_face && !_title_face_tried) {
            _title_face_tried = true;
            if (FT_Init_FreeType(&_title_ft) == 0) {
                FcPattern *pat = FcNameParse((const FcChar8 *)"sans");
                FcConfigSubstitute(NULL, pat, FcMatchPattern);
                FcDefaultSubstitute(pat);
                FcResult res = FcResultNoMatch;
                FcPattern *mat = FcFontMatch(NULL, pat, &res);
                if (mat) {
                    FcChar8 *file = NULL;
                    if (FcPatternGetString(mat, FC_FILE, 0, &file) ==
                        FcResultMatch) {
                        FT_New_Face(_title_ft, (const char *)file, 0,
                                    &_title_face);
                        if (_title_face)
                            FT_Set_Pixel_Sizes(_title_face, 0, 13);
                    }
                    FcPatternDestroy(mat);
                }
                FcPatternDestroy(pat);
            }
        }
        if (!_title_face) return;
        hit = vt_malloc0(sizeof(*hit));
        if (!hit) return;
        hit->key = vt_strdup(key);
        /* measure */
        int adv = 0;
        for (const unsigned char *p = (const unsigned char *)utf8;
             *p && adv <= max_w;) {
            unsigned cp;
            int len = 1;
            if ((*p & 0x80) == 0) { cp = *p; len = 1; }
            else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
            else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
            else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
            else { cp = '?'; len = 1; }
            if (FT_Load_Char(_title_face, cp, FT_LOAD_RENDER) == 0) {
                adv += (int)_title_face->glyph->advance.x >> 6;
                if (adv > max_w) { adv = max_w; break; }
            }
            p += len;
        }
        int asc = (int)(_title_face->size->metrics.ascender >> 6);
        int desc = (int)(-_title_face->size->metrics.descender >> 6);
        hit->w = adv;
        hit->h = asc + desc + 2;
        if (hit->w < 1) hit->w = 1;
        hit->argb = vt_malloc0(sizeof(uint32_t) *
                               (size_t)hit->w * (size_t)hit->h);
        /* rasterize */
        int cx = 0;
        for (const unsigned char *p = (const unsigned char *)utf8;
             *p && cx < max_w;) {
            unsigned cp;
            int len = 1;
            if ((*p & 0x80) == 0) { cp = *p; len = 1; }
            else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
            else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
            else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
            else { cp = '?'; len = 1; }
            if (FT_Load_Char(_title_face, cp, FT_LOAD_RENDER) == 0) {
                FT_Bitmap *bm = &_title_face->glyph->bitmap;
                int gx = cx + (int)_title_face->glyph->bitmap_left;
                int gy = asc - (int)_title_face->glyph->bitmap_top;
                for (unsigned yy = 0; yy < bm->rows; yy++) {
                    int dy = gy + (int)yy;
                    if (dy < 0 || dy >= hit->h) continue;
                    for (unsigned xx = 0; xx < bm->width; xx++) {
                        int dx = gx + (int)xx;
                        if (dx < 0 || dx >= hit->w) continue;
                        unsigned char a = bm->buffer
                            ? bm->buffer[yy * bm->pitch + xx] : 0;
                        if (a == 0) continue;
                        uint32_t old = hit->argb[dy * hit->w + dx];
                        unsigned na = (a * 255u) / 255u;
                        unsigned oa = old >> 24;
                        unsigned mix = (na > oa) ? na : oa;
                        hit->argb[dy * hit->w + dx] =
                            (mix << 24) | (argb & 0xffffff);
                    }
                }
                cx += (int)_title_face->glyph->advance.x >> 6;
            }
            p += len;
        }
        hit->next = _title_cache;
        _title_cache = hit;
        /* keep the cache bounded */
        int n = 0;
        for (_title_cache_t *t = _title_cache; t; t = t->next) n++;
        if (n > 64) {
            _title_cache_t *t = _title_cache;
            while (t->next && t->next->next) t = t->next;
            if (t->next) {
                vt_free(t->next->key);
                vt_free(t->next->argb);
                vt_free(t->next);
                t->next = NULL;
            }
        }
    }
    if (!hit || !hit->argb) return;
    /* alpha-blend the cached bitmap onto the framebuffer */
    int x0 = x, y0 = y;
    for (int yy = 0; yy < hit->h; yy++) {
        int dy = y0 + yy;
        if (dy < 0 || dy >= H) continue;
        for (int xx = 0; xx < hit->w; xx++) {
            int dx = x0 + xx;
            if (dx < 0 || dx >= W) continue;
            uint32_t sp = hit->argb[yy * hit->w + xx];
            uint32_t a = sp >> 24;
            if (a == 0) continue;
            uint32_t dp = fb[dy * W + dx];
            uint32_t rb = ((sp & 0x00ff00ff) * a +
                           (dp & 0x00ff00ff) * (255 - a)) / 255;
            uint32_t g = ((sp & 0x0000ff00) * a +
                          (dp & 0x0000ff00) * (255 - a)) / 255;
            fb[dy * W + dx] = 0xff000000 | (rb & 0x00ff00ff) |
                              (g & 0x0000ff00);
        }
    }
#else
    (void)fb; (void)W; (void)H; (void)x; (void)y; (void)max_w;
    (void)utf8; (void)argb; (void)active;
#endif
}

void _ssd_paint(_wl_state_t *st, _wl_surf_t *s) {
    if (!s->ssd || !s->toplevel) return;
    /* fullscreen windows have no decorations — the client covers the
     * whole output (a painted title band would cover app content) */
    if (s->toplevel->fullscreen) return;
    int fx, fy, fw, fh;
    _ssd_frame_geom(s, &fx, &fy, &fw, &fh);
    bool active = (st->kbd_focus == s);
    /* NEUTRAL focus styling: the active border is a slightly LIGHTER
     * shade of the same graphite family, never a saturated accent —
     * the old bright blue (0xff4f9adc) read as a glowing blue aura
     * around active windows ("Mirage has a blue glow around the
     * window"). Focus is conveyed by the title bar + text instead. */
    uint32_t border = active ? 0xff3d4148 : 0xff26282e;
    uint32_t bar    = active ? 0xff2b2f36 : 0xff1a1c22;
    uint32_t fg     = active ? 0xffeceef0 : 0xff909399;
    uint32_t *fb = st->fb;
    int W = st->out_w, H = st->out_h;
    /* title bar + borders (opaque writes; the client covers the middle) */
    for (int y = fy; y < fy + _WL_SSD_TITLE + _WL_SSD_BORDER; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = fx; x < fx + fw; x++) {
            if (x < 0 || x >= W) continue;
            fb[y * W + x] = bar;
        }
    }
    for (int y = fy + _WL_SSD_TITLE + _WL_SSD_BORDER;
         y < fy + fh - _WL_SSD_BORDER; y++) {
        for (int k = 0; k < 2; k++) {
            int xl = fx + k * (fw - _WL_SSD_BORDER);
            if (y < 0 || y >= H || xl < 0 || xl >= W) continue;
            fb[y * W + xl] = border;
        }
    }
    for (int x = fx; x < fx + fw; x++) {
        int yb = fy + fh - _WL_SSD_BORDER;
        if (yb < 0 || yb >= H || x < 0 || x >= W) continue;
        fb[yb * W + x] = border;
    }
    /* Pending-resize backdrop: while the client has not committed a
     * buffer matching the configured size yet (interactive resize in
     * flight), the interior would otherwise show the PREVIOUS frame's
     * pixels shifted around (smear). Fill the client area with the
     * title-bar color; the buffer-bounded client blit then covers the
     * top-left region of it. Zero cost in the steady state, where the
     * committed buffer already matches the configured size. */
    if (s->buf_w < s->w || s->buf_h < s->h) {
        for (int y = fy + _WL_SSD_TITLE + _WL_SSD_BORDER;
             y < fy + fh - _WL_SSD_BORDER; y++) {
            if (y < 0 || y >= H) continue;
            for (int x = fx + _WL_SSD_BORDER; x < fx + fw - _WL_SSD_BORDER;
                 x++) {
                if (x < 0 || x >= W) continue;
                fb[y * W + x] = bar;
            }
        }
    }
    /* window buttons: [–][▢][×] at the right end (drawn as shapes) */
    int bx = fx + fw - _WL_SSD_BORDER - _WL_SSD_BTN;
    int by = fy + 3;
    struct { char kind; uint32_t col; } btns[3] = {
        { 'x', 0xffe05a5a }, { 'm', 0xff4f9adc }, { 'n', 0xff7ac860 },
    };
    for (int i = 0; i < 3; i++) {
        int x0 = bx - i * (_WL_SSD_BTN + 2);
        int cy = by + (_WL_SSD_TITLE - 12) / 2;
        for (int yy = by + 2; yy < by + _WL_SSD_TITLE - 4; yy++)
            for (int xx = x0 + 2; xx < x0 + _WL_SSD_BTN - 2; xx++) {
                if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                fb[yy * W + xx] = (bar & 0xffffff00u) | 0x33;
            }
        for (int k = 0; k < 6; k++) {
            switch (btns[i].kind) {
            case 'x':   /* × glyph */
                if (k < 6) {
                    int px = x0 + 8 + k, py = cy + k;
                    if (px >= 0 && px < W && py >= 0 && py < H)
                        fb[py * W + px] = btns[i].col;
                    px = x0 + 13 - k;
                    if (px >= 0 && px < W && py >= 0 && py < H)
                        fb[py * W + px] = btns[i].col;
                }
                break;
            case 'm': { /* ▢ glyph */
                int px = x0 + 7 + k, py = cy;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    fb[py * W + px] = btns[i].col;
                int qx = x0 + 7, qy = cy + k;
                if (qx >= 0 && qx < W && qy >= 0 && qy < H)
                    fb[qy * W + qx] = btns[i].col;
                int rx2 = x0 + 12, ry2 = cy + k;
                if (rx2 >= 0 && rx2 < W && ry2 >= 0 && ry2 < H)
                    fb[ry2 * W + rx2] = btns[i].col;
                int sx = x0 + 7 + k, sy = cy + 5;
                if (sx >= 0 && sx < W && sy >= 0 && sy < H)
                    fb[sy * W + sx] = btns[i].col;
                break;
            }
            default: {  /* – glyph */
                int px = x0 + 7 + k, py = cy + 2;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    fb[py * W + px] = btns[i].col;
                break;
            }
            }
        }
    }
    /* title text: our own FreeType rasterizer (fontconfig "sans"),
     * rendered once per title+color and cached — titles change rarely */
    if (s->toplevel->title && *s->toplevel->title) {
        _ssd_title_paint(fb, W, H, fx + 8, fy + 2, bx - fx - 12,
                         s->toplevel->title, fg, active);
    }
}

void _paint_background(_wl_state_t *st) {
    /* the SAME wallpaper the X11 desktop shows (config [wallpaper]):
     * rendered once into bg_pix at startup, blitted every frame.
     * Previously this was a flat hardcoded gray — the user's "blue
     * background is fucked on Wayland" was exactly this divergence. */
    if (st->bg_pix) {
        memcpy(st->fb, st->bg_pix,
               sizeof(uint32_t) * (size_t)st->out_w * (size_t)st->out_h);
        return;
    }
    for (int y = 0; y < st->out_h; y++) {
        uint32_t row = 0xff1a1a1a;
        for (int x = 0; x < st->out_w; x++)
            st->fb[y * st->out_w + x] = row;
    }
}

/* ---------------------------------------------------- toplevel pool */
/* One allocation path for WM-visible windows: xdg toplevels AND
 * Xwayland X windows both need a stable id + title + app_id for the
 * taskbar/pager/IPC — the X11-app path creates synthetic toplevels. */
_xdg_toplevel_t *_toplevel_new(_wl_state_t *st, _wl_surf_t *s,
                               const char *title, const char *app_id) {
    _xdg_toplevel_t *t = vt_malloc0(sizeof(*t));
    if (!t) return NULL;
    t->surf = s;
    t->id = ++st->next_win_id;
    t->title = vt_strdup(title ? title : "");
    t->app_id = vt_strdup(app_id ? app_id : "");
    if (s) s->toplevel = t;
    return t;
}

void _toplevel_free(_xdg_toplevel_t *t) {
    if (!t) return;
    if (t->surf && t->surf->toplevel == t) t->surf->toplevel = NULL;
    vt_free(t->title);
    vt_free(t->app_id);
    vt_free(t);
}

/* ---------------------------------------------------- stacking rules */
/* Order (bottom→top): background/bottom layer surfaces → toplevels →
 * top/overlay layer surfaces → popups. The old code pushed everything
 * to the very top, which stacked new windows above docked panels. */
void _stack_insert(_wl_surf_t *s) {
    _wl_state_t *st = _wls;
    if (!st) return;
    if (s->popup) {
        wl_list_insert(st->surfaces.prev, &s->link);
        return;
    }
    if (s->layer) {
        bool low = _layer_is_bottom(s);
        _wl_surf_t *it;
        if (low) {
            /* below the first toplevel (background panels) */
            wl_list_for_each(it, &st->surfaces, link) {
                if (it->toplevel) {
                    wl_list_insert(it->link.prev, &s->link);
                    return;
                }
            }
        } else {
            /* above toplevels, below the first popup (top panels) */
            wl_list_for_each(it, &st->surfaces, link) {
                if (it->popup) {
                    wl_list_insert(it->link.prev, &s->link);
                    return;
                }
            }
        }
        wl_list_insert(st->surfaces.prev, &s->link);
        return;
    }
    /* toplevel (or Xwayland window): above other toplevels but below
     * top/overlay layer surfaces (docked panels stay on top) */
    _wl_surf_t *it;
    wl_list_for_each(it, &st->surfaces, link) {
        if (_layer_is_top(it) && it->mapped) {
            wl_list_insert(it->link.prev, &s->link);
            return;
        }
    }
    wl_list_insert(st->surfaces.prev, &s->link);
}

void _stack_resort(_wl_state_t *st) {
    if (!st) return;
    _wl_surf_t *s, *tmp;
    wl_list_for_each_safe(s, tmp, &st->surfaces, link) {
        if (!s->layer) continue;
        wl_list_remove(&s->link);
        _stack_insert(s);
    }
}

/* workspace event emission (panel + WM both subscribe) */
static void _emit_ws(_wl_state_t *st) {
    vt_backend_wl_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = VT_BACKEND_WL_EVENT_WORKSPACE;
    ev.window_id = (uint64_t)(unsigned)st->ws_cur;
    ev.title = NULL;
    ev.app_id = NULL;
    vt_backend_emit_event(st->backend_self, &ev);
}

/* alpha-blend an ARGB sprite over the XRGB framebuffer.
 * stride is in uint32 units and may exceed sw (cursor_img is a
 * 64x64 cell with the image in the top-left corner; client cursor
 * surfaces follow the client's own row padding).
 * The source is PREMULTIPLIED (wl_shm ARGB8888 and Xcursor images both
 * are): the correct "over" is sp + dp*(1-a). The old straight-alpha
 * form (sp*a + dp*(1-a)) double-multiplied the antialiased edges and
 * the cursor's soft shadow — over the dark panel that darkened fringe
 * read as a colored aura/halo around the pointer (the "blue glow over
 * the bar": the compositor arrow looked fine on the hardware plane —
 * drm cursor bo's are premultiplied and copied verbatim — but the
 * client cursor from the panel is software-blended). */
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
                /* premultiplied source over XRGB destination:
                 * out = sp + dp * (1 - a). The RGB channels of sp
                 * already carry their own alpha weight. */
                uint32_t ia = 255 - a;
                uint32_t rb = (sp & 0x00ff00ff) +
                              ((dp & 0x00ff00ff) * ia) / 255;
                uint32_t g = (sp & 0x0000ff00) +
                             ((dp & 0x0000ff00) * ia) / 255;
                out = 0xff000000 | (rb & 0x00ff00ff) | (g & 0x0000ff00);
            }
            st->fb[dy * st->out_w + dx] = out;
        }
    }
}

/* blit client pixels into the framebuffer. `blend` = premultiplied
 * source-over (CSD windows and popups: the buffer contains real alpha
 * — soft shadows, rounded corners); `clip` = only the window/content
 * rect (SSD windows: the compositor frame owns everything outside the
 * content, so the client's leftover CSD margins never bleed under the
 * title band). The raw copy that preceded this wrote semi-transparent
 * shadow pixels OPAQUELY into the XRGB framebuffer — every CSD app
 * got solid black bands around it (GTK4: 14-15px; the reported
 * "mirage black region"). */
static void _blit_client(_wl_state_t *st, const _wl_surf_t *s,
                         int x, int y, bool blend, bool clip) {
    int cw = s->buf_w < s->w ? s->buf_w : s->w;
    int ch = s->buf_h < s->h ? s->buf_h : s->h;
    int x0 = 0, y0 = 0;
    if (clip && s->have_win_geo) {
        x0 = s->win_gx < 0 ? 0 : s->win_gx;
        y0 = s->win_gy < 0 ? 0 : s->win_gy;
        int gx1 = x0 + (s->win_gw > 0 ? s->win_gw : 0);
        int gy1 = y0 + (s->win_gh > 0 ? s->win_gh : 0);
        if (gx1 < cw) cw = gx1;
        if (gy1 < ch) ch = gy1;
    }
    for (int sy = y0; sy < ch; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= st->out_h) continue;
        for (int sx = x0; sx < cw; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= st->out_w) continue;
            uint32_t sp = s->pixels[sy * s->stride + sx];
            if (!blend) {
                st->fb[dy * st->out_w + dx] = sp;
                continue;
            }
            uint32_t a = sp >> 24;
            if (a == 0) continue;
            if (a == 0xff) {
                st->fb[dy * st->out_w + dx] = 0xff000000 | sp;
                continue;
            }
            uint32_t dp = st->fb[dy * st->out_w + dx];
            uint32_t ia = 255 - a;
            uint32_t rb = (sp & 0x00ff00ff) +
                          ((dp & 0x00ff00ff) * ia) / 255;
            uint32_t g = (sp & 0x0000ff00) +
                         ((dp & 0x0000ff00) * ia) / 255;
            st->fb[dy * st->out_w + dx] =
                0xff000000 | (rb & 0x00ff00ff) | (g & 0x0000ff00);
        }
    }
}

static void _paint(void) {
    _wl_state_t *st = _wls;
    _ftrace(st && st->dirty ? 'P' : 'p');
    if (!st || !st->dirty) return;
    _paint_background(st);
    /* surfaces bottom→top (client windows; cursor surfaces skipped;
     * windows on other workspaces or minimized are hidden). Subsurfaces
     * paint directly above their parent, at their relative position.
     * Popups paint at parent + rel (menus follow their anchor window). */
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (!s->mapped || !s->pixels || s->is_cursor) continue;
        if (s->parent) continue;            /* painted with the parent */
        if (s->minimized) continue;
        if (s->toplevel && s->ws != st->ws_cur) continue;
        if (s->toplevel) {
            /* our SSD frame first (client paints inside it) */
            _ssd_paint(st, s);
            _wl_surf_t *sub;
            wl_list_for_each(sub, &s->subs, sub_link) {
                if (!sub->mapped || !sub->pixels || sub->minimized) continue;
                _blit_client(st, sub, s->x + sub->dx, s->y + sub->dy,
                             true, false);
            }
        }
        int x = s->x, y = s->y;
        if (s->popup) {
            /* popup: position = parent origin + relative offset */
            _wl_surf_t *ps = s->popup->parent;
            if (ps) {
                x = ps->x + s->popup->rel_x;
                y = ps->y + s->popup->rel_y;
            }
            /* popup children (submenus): paint with this popup */
            _wl_surf_t *sub;
            wl_list_for_each(sub, &s->subs, sub_link) {
                if (!sub->mapped || !sub->pixels) continue;
                _blit_client(st, sub, x + sub->dx, y + sub->dy,
                             true, false);
            }
        }
        /* Client content. SSD windows: clipped to the content rect
         * (opaque copy — the frame surrounds it). Everything else
         * (CSD toplevels, popups): full buffer, alpha-BLENDED — CSD
         * shadow margins are real alpha and must composite over the
         * background instead of being stamped in as solid black. */
        _blit_client(st, s, x, y,
                     !(s->ssd && s->toplevel), s->ssd && s->toplevel);
    }
    /* (frame callbacks fire from _wl_dispatch — see
     * _fire_frame_callbacks) */
    /* Software cursor sprite — ONLY when the hardware cursor plane
     * is not already showing it. Drawing BOTH (the old "always blend
     * the sprite" belt-and-braces) double-blends the cursor's
     * antialiased fringe and shadow on real GPUs: the hardware plane
     * composites the same image over a framebuffer that already
     * contains the blended copy, and the doubled soft edges read as a
     * persistent colored glow/aura around the pointer (bluish over
     * dark navy backgrounds). The sprite remains the fallback for
     * headless/software renderers and any drmModeSetCursor failure,
     * and VANTAGE_WL_SOFTWARE_CURSOR=1 forces it for debugging. */
    bool hw_cursor_on = st->kms && vt_kms_cursor_active(st->kms) &&
                        !_env_flag("VANTAGE_WL_SOFTWARE_CURSOR");
    if (!hw_cursor_on) {
    if (st->cur_shape == 0 &&
        st->cur_client_set && st->cursor_surf && st->cursor_surf->pixels) {
        /* buffer-bounded dims (identical in the steady state; keeps
         * the sprite read on the committed allocation if a cursor
         * surface is ever re-configured before its next commit) */
        _blend_sprite(st, st->cursor_surf->pixels,
                      st->cursor_surf->stride ? st->cursor_surf->stride
                                              : st->cursor_surf->buf_w,
                      st->cursor_surf->buf_w, st->cursor_surf->buf_h,
                      st->cursor_surf->hotspot_x,
                      st->cursor_surf->hotspot_y);
    } else {
        _blend_sprite(st, st->cursor_img, 64,
                      st->cur_img_w, st->cur_img_h,
                      st->cur_img_hx, st->cur_img_hy);
    }
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

/* screenshot on SIGUSR1 (testing hook). The write itself is DEFERRED
 * to the main loop (after _paint/_present complete): writing the PPM
 * from inside the signal handler could interrupt the paint loop
 * MID-BLIT, and a dump of that half-painted framebuffer showed windows
 * cut off at arbitrary rows — harness pixel checks flaked on exactly
 * that (a window whose blob was 66px of 220). */
static volatile sig_atomic_t _shot_pending = 0;

static void _screenshot_req(int sig) {
    (void)sig;
    _shot_pending = 1;
}

static void _screenshot_maybe(void) {
    if (!_shot_pending) return;
    _shot_pending = 0;
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
            if (s->xwl) {
                /* X11 window: WM_DELETE_WINDOW through the X display */
                _xwl_close(s);
                return 0;
            }
            if (s->toplevel->res)
                xdg_toplevel_send_close(s->toplevel->res);
            return 0;
        }
    }
    return -1;
}

static int _wl_maximize_window(vt_backend_t *self, uint64_t id, bool on) {
    _wl_state_t *st = self->priv;
    if (!st) return -1;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel->id == id) {
            if (s->xwl) {
                _xwl_maximize(s, on);
                return 0;
            }
            _wl_maximize_apply(s->toplevel, on);
            return 0;
        }
    }
    return -1;
}

static int _wl_fullscreen_window(vt_backend_t *self, uint64_t id, bool on) {
    _wl_state_t *st = self->priv;
    if (!st) return -1;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel->id == id) {
            if (s->xwl) {
                _xwl_fullscreen(s, on);
                return 0;
            }
            _wl_fullscreen_apply(s->toplevel, on);
            return 0;
        }
    }
    return -1;
}

static int _wl_minimize_window(vt_backend_t *self, uint64_t id, bool on) {
    _wl_state_t *st = self->priv;
    if (!st) return -1;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->toplevel && s->toplevel->id == id) {
            if (s->xwl) {
                _xwl_minimize(s, on);
                return 0;
            }
            /* xdg: mirror the set_minimized protocol path (hidden from
             * compositing, restorable from the taskbar). RESTORE was a
             * silent no-op before — a minimized native Wayland window
             * could never be brought back (taskbar click did nothing,
             * the window stayed invisible forever). xdg-shell has no
             * un-minimize event: showing it again is compositor
             * policy, so clear the flags, repaint, tell the WM, and
             * hand the window the keyboard back. */
            if (!on) {
                if (!s->minimized && !s->toplevel->minimized)
                    return 0;          /* nothing to restore */
                s->minimized = false;
                s->toplevel->minimized = false;
                if (st->kbd_focus == NULL)
                    _kbd_enter_focus(st, s);
                st->dirty = true;
                _emit_win(st, VT_BACKEND_WL_EVENT_WIN_STATE, s->toplevel);
                _activate_toplevel(st, s->toplevel);
                return 0;
            }
            s->toplevel->minimized = true;
            s->minimized = true;
            if (st->kbd_focus == s) st->kbd_focus = NULL;
            st->dirty = true;
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_STATE, s->toplevel);
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
    wl_list_init(&st->frame_cbs);
    wl_list_init(&st->retired_cbs);
    wl_list_init(&st->ptr_reses);
    wl_list_init(&st->kbd_reses);
    wl_list_init(&st->data_devs);
    wl_list_init(&st->primary_devs);

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
    /* The keymap is compiled ALWAYS — xkbcommon needs no devices. Even
     * headless (CI) clients bind wl_keyboard and require a REAL
     * wl_keyboard.keymap event; without it every launched app gets a
     * marshal error and its connection killed. */
    bool xkb_ok = _xkb_init(st);
    if (!st->headless && st->seat) {
        bool li_ok = _input_init(st);
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
        if (xkb_ok)
            vt_logi("wayland: headless input — xkb keymap compiled "
                    "without devices (clients still get a real keymap)");
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
        &wl_compositor_interface, 6, NULL, _bind_compositor);
    st->seat_g = wl_global_create(st->display, &wl_seat_interface, 9, NULL,
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
    /* xdg-decoration: CSD apps stay undecorated (client mode is the
     * default); clients that WANT our frame request server mode */
    st->decor_g = wl_global_create(
        st->display, &zxdg_decoration_manager_v1_interface, 1, NULL,
        _bind_decor_mgr);
    /* zwp_primary_selection_device_manager_v1 — the selection
     * clipboard every terminal (foot, kitty, GTK) drives */
    st->primary_sel_g = wl_global_create(
        st->display, &zwp_primary_selection_device_manager_v1_interface,
        1, NULL, _bind_primary_sel);
    if (!st->primary_sel_g)
        vt_logw("wayland: primary-selection global creation failed");
    if (!st->decor_g)
        vt_logw("wayland: xdg-decoration global creation failed "
                "(clients cannot request server-side decorations)");
    if (!st->subcomp_g || !st->ddm_g)
        vt_logw("wayland: wl_subcompositor/wl_data_device_manager "
                "global creation failed");
    if (!st->compositor_g || !st->seat_g || !st->output_g || !st->xdg_g) {
        _stage_fail(12, "failed to create globals");
        goto fail_no_kms;
    }
    st->client_created.notify = _client_destroyed;
    wl_display_add_client_created_listener(st->display, &st->client_created);
    /* deterministic socket name: the session manager sets
     * VANTAGE_WAYLAND_SOCKET so it can hand children the exact
     * WAYLAND_DISPLAY without polling the runtime dir */
    const char *want_sock = getenv("VANTAGE_WAYLAND_SOCKET");
    const char *sock = NULL;
    if (want_sock && *want_sock) {
        if (wl_display_add_socket(st->display, want_sock) == 0)
            sock = want_sock;
        else
            vt_logw("wayland: cannot bind socket '%s' — falling back to "
                    "an automatic name", want_sock);
    }
    if (!sock)
        sock = wl_display_add_socket_auto(st->display);
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

    signal(SIGUSR1, _screenshot_req);
    _wls = st;
    st->dirty = true;

    /* ---- workspace model + layer-shell + Xwayland ---------------- */
    st->ws_count = 4;
    st->ws_cur = 0;
    /* the panel is a CLIENT now: docked through wlr-layer-shell (the
     * same protocol every independent panel uses) — not code baked
     * into the compositor */
    _layer_shell_global_create(st);
    if (!st->layer_shell_g)
        vt_logw("wayland: layer-shell global creation failed — docked "
                "panels cannot attach");
    /* staging protocols: cursor-shape, xdg-activation,
     * fractional-scale, toplevel-icon */
    _proto_globals_create(st);
    /* linux-dmabuf: GPU client buffers (EGL import + readback). On real
     * hardware this is what lets browsers/games render on the actual
     * GPU — and what unblocks Xwayland glamor (hardware GLX for X11
     * apps). Runs BEFORE _xwl_start so Xwayland sees the global at
     * spawn time. */
    _dmabuf_globals_create(st);
    /* Xwayland: X11 apps become first-class windows of this session */
    st->xwl_enabled = _xwl_start(st);

    /* ---- stage 15/15: desktop (first frame) -------------------------- */
    _stage_begin(14, "first frame");
    /* the desktop background = the user's [wallpaper] config, exactly
     * what the X11 desktop shows (same engine, same file) */
    st->wall = vt_wallpaper_new();
    vt_wallpaper_config_load(st->wall);
    st->bg_pix = vt_malloc(sizeof(uint32_t) *
                           (size_t)st->out_w * (size_t)st->out_h);
    if (st->bg_pix) {
        vt_wallpaper_render_argb(st->wall, st->bg_pix, st->out_w, st->out_h);
        vt_logi("wayland: desktop background from [wallpaper] config "
                "(mode=%s)",
                st->wall->kind == VT_WALLPAPER_GRADIENT ? "gradient" :
                st->wall->kind == VT_WALLPAPER_IMAGE ? "image" :
                st->wall->kind == VT_WALLPAPER_COLOR ? "color" : "video");
    }
    _paint();
    _present();
    _dmabuf_log_stats();
    _stage_ok(14, "desktop painted %dx%d (%s), wallpaper background, "
              "cursor software sprite, layer-shell + Xwayland ready",
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
    _xwl_stop(st);
    _layer_shell_global_destroy(st);
    _proto_globals_destroy(st);
    _dmabuf_globals_destroy(st);
    _title_cache_free_all();
#if defined(VT_HAVE_FREETYPE)
    _ssd_title_font_fini();
#endif
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
    /* Destroy the CLIENTS first: wl_display_destroy alone frees the
     * display's own bookkeeping but NOT the connected clients — every
     * remaining surface (with its copied pixel buffer), popup, layer
     * surface, xdg resource and pointer/keyboard resource would leak
     * (their destructors never run). destroy_clients runs the normal
     * resource teardown exactly as a client disconnect does. */
    wl_display_destroy_clients(st->display);
    wl_display_destroy(st->display);
    if (st->wall) vt_wallpaper_free(st->wall);
    vt_free(st->bg_pix);
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
    /* Xwayland window events (MapRequest/properties/…) arrive on the
     * xcb connection, not the wayland loop — drain them here */
    _xwl_dispatch();
    /* PAINT FIRST, then marshal frame callbacks, then FLUSH: every
     * event generated this iteration (buffer releases during request
     * dispatch, callback done below) hits the socket before the
     * iteration ends. The previous order (flush → paint) left fired
     * callbacks buffered for a full 20ms tick — and a race could lose
     * them entirely, freezing frame-callback-driven clients (browsers)
     * after their first frame. */
    _paint();
    _fire_frame_callbacks(st);
    wl_display_flush_clients(st->display);
    _present();
    /* deferred screenshot: the framebuffer is COMPLETE here (a dump
     * taken mid-paint photographed half-blitted windows) */
    _screenshot_maybe();
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
    /* burst n=N x0=A y0=B x1=C y1=D: N motion events sweeping between
     * the two points, delivered in ONE IPC call — a full-speed drag at
     * real pointer hardware rates (a 1000 Hz mouse moving for a second
     * is 1000 geometry broadcasts in one dispatch cycle). This is the
     * reproducer class for the "fast window moves lock the DE"
     * livelock: the broadcast path must survive its own event flood. */
    if (strncmp(spec, "burst ", 6) == 0) {
        int n = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (sscanf(spec, "burst n=%d x0=%d y0=%d x1=%d y1=%d",
                   &n, &x0, &y0, &x1, &y1) == 5 && n > 0 && n <= 5000) {
            for (int i = 1; i <= n; i++) {
                double t = (double)i / (double)n;
                int bx = (int)(x0 + (x1 - x0) * t);
                int by = (int)(y0 + (y1 - y0) * t);
                _pointer_motion(st, (double)(bx - st->cursor_x),
                                (double)(by - st->cursor_y));
            }
            return 0;
        }
        return -1;
    }
    if (sscanf(spec, "press b=%d", &b) == 1 && b >= 1 && b <= 3) {
        _pointer_button(st, b == 2 ? 0x112 : b == 3 ? 0x111 : 0x110,
                        true);
        return 0;
    }
    if (sscanf(spec, "release b=%d", &b) == 1 && b >= 1 && b <= 3) {
        _pointer_button(st, b == 2 ? 0x112 : b == 3 ? 0x111 : 0x110,
                        false);
        return 0;
    }
    if (sscanf(spec, "axis d=%d", &d) == 1 && d != 0) {
        _pointer_axis(st, (double)(d * 15), true, false);
        return 0;
    }
    /* key <name>: inject through the REAL keyboard path (the same
     * _kbd_key handler libinput feeds), so integration harnesses can
     * type into panel search bars and drive menus. Uppercase names
     * (e.g. "A", "F1") auto-apply Shift. */
    if (strncmp(spec, "keydown ", 8) == 0 ||
        strncmp(spec, "keyup ", 6) == 0) {
        /* hold/release a modifier for multi-event gestures (e.g.
         * Super+right-drag resize): "key" taps, these stay held */
        bool down = spec[0] == 'k' && spec[3] == 'd';
        const char *name = spec + (down ? 8 : 6);
        while (*name == ' ') name++;
        if (!*name) return -1;
#if defined(VT_HAVE_XKBCOMMON)
        xkb_keysym_t sym = xkb_keysym_from_name(name, 0);
        if (sym == XKB_KEY_NoSymbol) {
            vt_logw("wayland: test-input: unknown key '%s'", name);
            return -1;
        }
        for (xkb_keycode_t kc = 8; kc < 256; kc++) {
            if (xkb_state_key_get_one_sym(st->xkb_st, kc) == sym) {
                _kbd_key(st, (uint32_t)(kc - 8), down);
                return 0;
            }
        }
        vt_logw("wayland: test-input: no keycode for '%s'", name);
        return -1;
#else
        (void)name;
        return -1;
#endif
    }
    if (strncmp(spec, "key ", 4) == 0) {
        const char *name = spec + 4;
        while (*name == ' ') name++;
        if (!*name) return -1;
#if defined(VT_HAVE_XKBCOMMON)
        bool shift = false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", name);
        if (strlen(buf) == 1 && buf[0] >= 'A' && buf[0] <= 'Z') {
            buf[0] = (char)(buf[0] - 'A' + 'a');
            shift = true;
        }
        xkb_keysym_t sym = xkb_keysym_from_name(buf, 0);
        if (sym == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(name, 0);
        if (sym == XKB_KEY_NoSymbol) {
            vt_logw("wayland: test-input: unknown key '%s'", name);
            return -1;
        }
        /* find the keycode that produces this keysym */
        for (xkb_keycode_t kc = 8; kc < 256; kc++) {
            if (xkb_state_key_get_one_sym(st->xkb_st, kc) == sym) {
                if (shift) {
                    _kbd_key(st, 42, true);   /* KEY_LEFTSHIFT */
                }
                _kbd_key(st, (uint32_t)(kc - 8), true);
                _kbd_key(st, (uint32_t)(kc - 8), false);
                if (shift) {
                    _kbd_key(st, 42, false);
                }
                return 0;
            }
        }
        vt_logw("wayland: test-input: no keycode for '%s'", name);
        return -1;
#else
        (void)name;
        return -1;
#endif
    }
    /* click x,y: motion + press + release convenience */
    if (sscanf(spec, "click %d,%d", &x, &y) == 2) {
        _pointer_motion(st, (double)(x - st->cursor_x),
                        (double)(y - st->cursor_y));
        _pointer_button(st, 0x110, true);
        _pointer_button(st, 0x110, false);
        return 0;
    }
    vt_logw("wayland: test-input: unrecognized spec '%s'", spec);
    return -1;
}

/* focus the top-most window on the CURRENT workspace (used after
 * workspace switches — every WM focuses something on the desktop you
 * switch to; leaving focus on an invisible window means keystrokes go
 * nowhere and the pager shows no focused miniature) */
void _focus_top_on_ws(_wl_state_t *st) {
    if (!st) return;
    _wl_surf_t *s;
    wl_list_for_each_reverse(s, &st->surfaces, link) {
        if (!s->mapped || s->is_cursor || s->minimized || !s->toplevel)
            continue;
        if (s->ws != st->ws_cur) continue;
        if (st->kbd_focus != s) {
            st->kbd_focus = s;
            st->focused_toplevel = s->toplevel;
            _kbd_enter_focus(st, s);
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, s->toplevel);
        }
        return;
    }
    /* nothing on this workspace: clear the focus */
    if (st->kbd_focus && st->kbd_focus->res) {
        _kbd_res_t *kr;
        wl_list_for_each(kr, &st->kbd_reses, link) {
            if (wl_resource_get_client(kr->res) ==
                wl_resource_get_client(st->kbd_focus->res))
                wl_keyboard_send_leave(kr->res, ++st->serial,
                                       st->kbd_focus->res);
        }
    }
    st->kbd_focus = NULL;
    st->focused_toplevel = NULL;
}

static int _wl_set_workspace(vt_backend_t *self, int ws) {
    _wl_state_t *st = (self && self->priv && self->priv == (void *)_wls)
                      ? _wls : NULL;
    if (!st || ws < 0 || ws >= st->ws_count || ws == st->ws_cur) return -1;
    st->ws_cur = ws;
    _emit_ws(st);
    _focus_top_on_ws(st);
    _xwl_workspace_changed(st);
    st->dirty = true;
    vt_logi("wayland: workspace -> %d (remote)", ws + 1);
    return 0;
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
    b->maximize_window = _wl_maximize_window;
    b->fullscreen_window = _wl_fullscreen_window;
    b->minimize_window = _wl_minimize_window;
    b->test_input = _wl_test_input;
    b->set_workspace = _wl_set_workspace;
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

/* XWayland display environment for IPC consumers (panel launcher,
 * session autostart): the compositor's own setenv(DISPLAY) cannot
 * cross process boundaries — siblings that spawn X11 apps must ASK. */
bool _vt_backend_wl_xwl_env_impl(char *d, size_t dn, char *a, size_t an) {
    _wl_state_t *st = _wls;
    if (!st || !st->xwl_enabled) return false;
    int disp = _xwl_display();
    if (disp < 0) return false;
    if (d && dn) snprintf(d, dn, ":%d", disp);
    const char *af = _xwl_auth_file();
    if (a && an) snprintf(a, an, "%s", af ? af : "");
    return true;
}
