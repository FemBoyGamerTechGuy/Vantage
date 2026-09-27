/*
 * vt-wl-xwayland.c — Xwayland support: spawn, shell protocol, WM
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * X11 applications run inside the native Wayland session through
 * Xwayland (the standard rootless X server for Wayland). This module:
 *
 *   1. spawns `Xwayland -rootless -displayfd N -auth <cookie>` as a
 *      child of the compositor and connects to the new X display via
 *      xcb — the compositor becomes THE window manager of that display
 *      (SubstructureRedirect), exactly like it manages the X11 session
 *      backend, so X11 apps get real decorations, focus, move/resize,
 *      workspaces and taskbar entries — not a floating unmanaged
 *      surface.
 *
 *   2. implements xwayland_shell_v1 (wayland-protocols 1.36+,
 *      Xwayland 24.1+): Xwayland associates each X window's wl_surface
 *      explicitly via set_serial — no size/position guesswork.
 *
 *   3. honors _MOTIF_WM_HINTS: CSD X11 apps (browsers with their own
 *      headerbar) are NEVER double-decorated.
 *
 *   4. exposes the display number + Xauthority path so the session,
 *      the panel and IPC clients can set DISPLAY for apps that still
 *      need X11.
 *
 * The X11 window management here deliberately mirrors the semantics of
 * src/wm/vt-wm-x11.c so both backends feel like the same desktop.
 */

#define VT_LOG_DOMAIN "backend-wayland"
#include "vt-wl-internal.h"

#if defined(VT_HAVE_WAYLAND) && defined(VT_HAVE_XCB_XWAYLAND)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <poll.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

/* ------------------------------------------------------------ types */

typedef struct _xwl_win {
    xcb_window_t xwin;
    _wl_surf_t *surf;               /* associated wl_surface (may lag) */
    _xdg_toplevel_t *toplevel;      /* synthetic toplevel for the WM */
    uint32_t create_serial;         /* X sequence for set_serial match */
    bool mapped;                    /* X window is mapped+managed */
    bool override_redirect;         /* OR windows: popups, never framed */
    bool motif_csd;                 /* _MOTIF_WM_HINTS decorations=0 */
    int x, y, w, h;
    char *title;
    char *class;
    bool waiting_surface;           /* mapped, xwayland_surface pending */
    struct wl_resource *xwl_res;    /* xwayland_surface_v1 */
    struct _xwl_win *next, *prev;
} _xwl_win_t;

static struct {
    bool started;
    pid_t pid;
    int display;                    /* display number (e.g. 2 → :2) */
    char auth_file[256];
    xcb_connection_t *xc;
    _xwl_win_t *wins;               /* list head */
    _wl_state_t *st;
    /* atoms */
    xcb_atom_t a_wm_name, a_net_wm_name, a_wm_class, a_wm_protocols,
                a_wm_delete, a_wm_take_focus, a_wm_normal_hints,
                a_motif_hints, a_net_wm_state, a_net_wm_state_fullscr,
                a_net_wm_state_maxv, a_net_wm_state_maxh, a_net_wm_icon,
                a_net_wm_window_type, a_net_active, a_net_close,
                a_net_current_desktop, a_net_number_desktops,
                a_net_client_list, a_net_supporting_check, a_net_supported,
                a_net_workarea, a_net_wm_desktop, a_utf8_string,
                a_net_wm_ping, a_wm_change_state, a_net_wm_visible_name;
    int wm_screen_width, wm_screen_height;
} X;

/* ------------------------------------------------------------- utils */

static void _xwl_free_win(_xwl_win_t *w) {
    if (!w) return;
    if (w->prev) w->prev->next = w->next;
    else if (X.wins == w) X.wins = w->next;
    if (w->next) w->next->prev = w->prev;
    if (w->surf) w->surf->xwl = NULL;
    if (w->toplevel) {
        _wl_state_t *st = X.st;
        if (st) _emit_win(st, VT_BACKEND_WL_EVENT_WIN_UNMAP, w->toplevel);
        _toplevel_free(w->toplevel);
    }
    vt_free(w->title);
    vt_free(w->class);
    vt_free(w);
}

static _xwl_win_t *_xwl_find(xcb_window_t xwin) {
    for (_xwl_win_t *w = X.wins; w; w = w->next)
        if (w->xwin == xwin) return w;
    return NULL;
}

static _xwl_win_t *_xwl_find_by_surf(_wl_surf_t *s) {
    return s ? s->xwl : NULL;
}

/* -------------------------------------------------------- properties */

static void _xwl_read_title(_xwl_win_t *w) {
    if (!X.xc || !w) return;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        X.xc, xcb_get_property(X.xc, false, w->xwin, X.a_net_wm_name,
                               X.a_utf8_string, 0, 512), NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        int len = xcb_get_property_value_length(r);
        char *buf = vt_malloc((size_t)len + 1);
        if (buf) {
            memcpy(buf, xcb_get_property_value(r), (size_t)len);
            buf[len] = 0;
            vt_free(w->title);
            w->title = buf;
        }
    }
    if (r) free(r);
    if (!w->title || !*w->title) {
        xcb_get_property_reply_t *r2 = xcb_get_property_reply(
            X.xc, xcb_get_property(X.xc, false, w->xwin, X.a_wm_name,
                                   XCB_GET_PROPERTY_TYPE_ANY, 0, 512),
            NULL);
        if (r2 && xcb_get_property_value_length(r2) > 0) {
            int len = xcb_get_property_value_length(r2);
            char *buf = vt_malloc((size_t)len + 1);
            if (buf) {
                memcpy(buf, xcb_get_property_value(r2), (size_t)len);
                buf[len] = 0;
                vt_free(w->title);
                w->title = buf;
            }
        }
        if (r2) free(r2);
    }
    if (w->toplevel && w->title) {
        vt_free(w->toplevel->title);
        w->toplevel->title = vt_strdup(w->title);
        if (X.st)
            _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_TITLE, w->toplevel);
    }
}

static void _xwl_read_class(_xwl_win_t *w) {
    if (!X.xc || !w) return;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        X.xc, xcb_get_property(X.xc, false, w->xwin, X.a_wm_class,
                               XCB_GET_PROPERTY_TYPE_ANY, 0, 512), NULL);
    if (r && xcb_get_property_value_length(r) > 1) {
        int len = xcb_get_property_value_length(r);
        const char *v = xcb_get_property_value(r);
        /* WM_CLASS = "instance\0class\0"; use the class (2nd string) */
        const char *cls = NULL;
        const char *p = memchr(v, 0, (size_t)len);
        if (p && p + 1 < v + len) {
            int rest = (int)((v + len) - (p + 1));
            char *buf = vt_malloc((size_t)rest + 1);
            if (buf) {
                memcpy(buf, p + 1, (size_t)rest);
                buf[rest] = 0;
                vt_free(w->class);
                w->class = buf;
                cls = buf;
            }
        }
        if (w->toplevel && cls) {
            vt_free(w->toplevel->app_id);
            w->toplevel->app_id = vt_strdup(cls);
            if (X.st)
                _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_TITLE, w->toplevel);
        }
    }
    if (r) free(r);
}

static void _xwl_read_motif(_xwl_win_t *w) {
    if (!X.xc || !w) return;
    bool csd = false;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        X.xc, xcb_get_property(X.xc, false, w->xwin, X.a_motif_hints,
                               XCB_GET_PROPERTY_TYPE_ANY, 0, 20), NULL);
    if (r && r->type != XCB_ATOM_NONE &&
        xcb_get_property_value_length(r) >= 8) {
        /* struct { u32 flags, functions, decorations, input, status } */
        const uint32_t *v = xcb_get_property_value(r);
        if (v[0] & 2)                 /* MWM_HINTS_DECORATIONS present */
            csd = (v[2] == 0);        /* decorations == 0 → client side */
    }
    if (r) free(r);
    if (w->motif_csd != csd) {
        w->motif_csd = csd;
        if (w->surf) w->surf->ssd = !csd && !w->override_redirect;
        if (X.st) X.st->dirty = true;
        vt_logi("xwayland: window 0x%x decorations: %s",
                (unsigned)w->xwin, csd ? "client-side (CSD)" : "ours");
    }
}

/* ------------------------------------------------------ X operations */

static void _xwl_configure(_xwl_win_t *w, int x, int y, int wd, int ht) {
    if (!X.xc || !w) return;
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                    XCB_CONFIG_WINDOW_BORDER_WIDTH;
    uint32_t vals[5] = { (uint32_t)x, (uint32_t)y,
                         (uint32_t)(wd > 0 ? wd : 1),
                         (uint32_t)(ht > 0 ? ht : 1), 0 };
    xcb_configure_window(X.xc, w->xwin, mask, vals);
    w->x = x;
    w->y = y;
    if (wd > 0) w->w = wd;
    if (ht > 0) w->h = ht;
}

static void _xwl_focus(_xwl_win_t *w) {
    if (!X.xc) return;
    if (w && w->mapped && !w->override_redirect) {
        /* WM_TAKE_FOCUS + XSetInputFocus — the standard EWMH focus */
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w->xwin;
        ev.type = X.a_wm_protocols;
        ev.data.data32[0] = X.a_wm_take_focus;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(X.xc, false, w->xwin,
                       XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
        xcb_set_input_focus(X.xc, XCB_INPUT_FOCUS_POINTER_ROOT,
                            w->xwin, XCB_CURRENT_TIME);
    } else {
        xcb_set_input_focus(X.xc, XCB_INPUT_FOCUS_POINTER_ROOT,
                            XCB_NONE, XCB_CURRENT_TIME);
    }
}

static void _xwl_close_win(_xwl_win_t *w) {
    if (!X.xc || !w) return;
    bool has_delete = false;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        X.xc, xcb_get_property(X.xc, false, w->xwin, X.a_wm_protocols,
                               XCB_ATOM_ATOM, 0, 64), NULL);
    if (r) {
        int n = xcb_get_property_value_length(r) / 4;
        const xcb_atom_t *v = xcb_get_property_value(r);
        for (int i = 0; i < n; i++)
            if (v[i] == X.a_wm_delete) { has_delete = true; break; }
        free(r);
    }
    if (has_delete) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = w->xwin;
        ev.type = X.a_wm_protocols;
        ev.data.data32[0] = X.a_wm_delete;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(X.xc, false, w->xwin, XCB_EVENT_MASK_NO_EVENT,
                       (const char *)&ev);
    } else {
        xcb_kill_client(X.xc, w->xwin);
    }
}

/* -------------------------------------------------------- shell v1 */

static void _xwl_surface_destroy_req(struct wl_client *cli,
                                     struct wl_resource *res) {
    (void)cli;
    _xwl_win_t *w = wl_resource_get_user_data(res);
    if (w) {
        w->xwl_res = NULL;
        if (w->surf) w->surf->xwl = NULL;
        w->surf = NULL;
    }
    wl_resource_destroy(res);
}

static void _xwl_surface_set_serial(struct wl_client *cli,
                                    struct wl_resource *res,
                                    uint32_t serial_hi,
                                    uint32_t serial_lo) {
    (void)cli;
    _xwl_win_t *w = wl_resource_get_user_data(res);
    if (!w) return;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    w->create_serial = (uint32_t)serial;
    vt_logd("xwayland: surface serial %llu for X window 0x%x",
            (unsigned long long)serial, (unsigned)w->xwin);
}

static const struct xwayland_surface_v1_interface _xwl_surface_impl = {
    .destroy     = _xwl_surface_destroy_req,
    .set_serial  = _xwl_surface_set_serial,
};

static void _xwl_surface_res_destroy(struct wl_resource *res) {
    _xwl_win_t *w = wl_resource_get_user_data(res);
    if (w && w->xwl_res == res) {
        w->xwl_res = NULL;
        if (w->surf) w->surf->xwl = NULL;
        w->surf = NULL;
    }
}

static void _xwl_get_xwayland_surface(struct wl_client *cli,
                                      struct wl_resource *res,
                                      uint32_t id,
                                      struct wl_resource *surface) {
    _wl_surf_t *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (!s) {
        wl_resource_post_error(res, 1, "get_xwayland_surface on a dead "
                                    "surface");
        return;
    }
    /* Xwayland creates the surface BEFORE the X window is fully wired;
     * we record the pairing and wait for the window to appear (either
     * it already exists from CreateNotify, or it arrives later). */
    _xwl_win_t *w = vt_malloc0(sizeof(*w));
    if (!w) { wl_client_post_no_memory(cli); return; }
    w->surf = s;
    s->xwl = w;
    struct wl_resource *r = wl_resource_create(
        cli, &xwayland_surface_v1_interface, 1, id);
    if (!r) { vt_free(w); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_xwl_surface_impl, w,
                                   _xwl_surface_res_destroy);
    w->xwl_res = r;
    /* try to find the X window that was created just before this
     * surface: newest waiting window without a surface */
    for (_xwl_win_t *cand = X.wins; cand; cand = cand->next) {
        if (cand->waiting_surface && !cand->xwl_res) {
            /* adopt it: move surface pointer */
            cand->surf = s;
            s->xwl = cand;
            w->surf = NULL;
            s->xwl->xwl_res = r;
            /* fix user data of the resource to the adopted window */
            wl_resource_set_user_data(r, cand);
            vt_free(w);
            w = cand;
            break;
        }
    }
}

static void _xwl_shell_destroy(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli; (void)res;
}

static const struct xwayland_shell_v1_interface _xwl_shell_impl = {
    .get_xwayland_surface = _xwl_get_xwayland_surface,
    .destroy = _xwl_shell_destroy,
};

static void _bind_xwl_shell(struct wl_client *cli, void *data,
                            uint32_t version, uint32_t id) {
    (void)data; (void)version;
    struct wl_resource *res = wl_resource_create(
        cli, &xwayland_shell_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_xwl_shell_impl, NULL, NULL);
}

/* --------------------------------------------------------- X events */

/* sync a surface's paint geometry from the X window (called when the
 * surface commits a new buffer) */
void _xwl_win_geom(_wl_surf_t *s) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w || !s) return;
    s->x = w->x;
    s->y = w->y;
    if (w->w > 0) s->w = w->w;
    if (w->h > 0) s->h = w->h;
    s->ssd = !w->motif_csd && !w->override_redirect;
}

static void _xwl_handle_create(xcb_create_notify_event_t *e) {
    if (_xwl_find(e->window)) return;
    _xwl_win_t *w = vt_malloc0(sizeof(*w));
    if (!w) return;
    w->xwin = e->window;
    w->override_redirect = e->override_redirect;
    w->create_serial = e->sequence;
    w->x = e->x;
    w->y = e->y;
    w->w = e->width;
    w->h = e->height;
    w->waiting_surface = true;
    w->next = X.wins;
    w->prev = NULL;
    if (X.wins) X.wins->prev = w;
    X.wins = w;
    vt_logd("xwayland: X window 0x%x created %dx%d +%d+%d (OR=%d)",
            (unsigned)w->xwin, w->w, w->h, w->x, w->y,
            (int)w->override_redirect);
}

static void _xwl_map(_xwl_win_t *w) {
    _wl_state_t *st = X.st;
    if (!st || !w) return;
    _xwl_read_title(w);
    _xwl_read_class(w);
    _xwl_read_motif(w);
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(
        X.xc, xcb_get_geometry(X.xc, w->xwin), NULL);
    if (g) {
        w->w = (int)g->width;
        w->h = (int)g->height;
        free(g);
    }
    if (w->override_redirect) {
        /* OR windows (X11 menus/tooltips): position where X says, no
         * frame, always on top, no taskbar entry */
        xcb_map_window(X.xc, w->xwin);
        if (w->surf) {
            w->surf->x = w->x;
            w->surf->y = w->y;
        }
        return;
    }
    /* manage: frame geometry from the request, placed like a real
     * window (center of the workarea on first map) */
    if (w->x == 0 && w->y == 0) {
        int wx, wy, ww, wh;
        _layer_workarea(st, &wx, &wy, &ww, &wh);
        w->x = wx + (ww - w->w) / 2;
        w->y = wy + (wh - w->h) / 2;
        if (w->x < wx) w->x = wx;
        if (w->y < wy) w->y = wy;
    }
    _xwl_configure(w, w->x, w->y, w->w, w->h);
    xcb_map_window(X.xc, w->xwin);
    if (w->surf) {
        w->surf->ssd = !w->motif_csd;
        w->surf->x = w->x;
        w->surf->y = w->y;
    }
    if (!w->toplevel) {
        w->toplevel = _toplevel_new(st, w->surf, w->title,
                                    w->class ? w->class : "");
        if (w->toplevel) {
            _wl_surf_t *s = w->surf;
            w->toplevel->app_id = vt_strdup(w->class ? w->class : "");
            if (s) {
                s->x = w->x;
                s->y = w->y;
                s->ws = st->ws_cur;
            }
            _emit_win(st, VT_BACKEND_WL_EVENT_WIN_MAP, w->toplevel);
            vt_logi("xwayland: window 0x%x '%s' mapped %dx%d at +%d+%d",
                    (unsigned)w->xwin, w->title ? w->title : "(untitled)",
                    w->w, w->h, w->x, w->y);
        }
    }
    /* taskbar-visible focus */
    _xwl_focus(w);
    st->dirty = true;
}

static void _xwl_handle_map_request(xcb_map_request_event_t *e) {
    _xwl_win_t *w = _xwl_find(e->window);
    if (!w) return;
    if (w->mapped) { xcb_map_window(X.xc, e->window); return; }
    w->mapped = true;
    _xwl_map(w);
}

static void _xwl_handle_destroy(xcb_destroy_notify_event_t *e) {
    _xwl_win_t *w = _xwl_find(e->window);
    if (!w) return;
    if (w->xwl_res)
        wl_resource_destroy(w->xwl_res);
    _xwl_free_win(w);
}

static void _xwl_handle_unmap(xcb_unmap_notify_event_t *e) {
    _xwl_win_t *w = _xwl_find(e->window);
    if (!w || !w->mapped) return;
    w->mapped = false;
    if (w->toplevel)
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE, w->toplevel);
}

static void _xwl_handle_property(xcb_property_notify_event_t *e) {
    _xwl_win_t *w = _xwl_find(e->window);
    if (!w) return;
    if (e->atom == X.a_wm_name || e->atom == X.a_net_wm_name)
        _xwl_read_title(w);
    else if (e->atom == X.a_wm_class)
        _xwl_read_class(w);
    else if (e->atom == X.a_motif_hints)
        _xwl_read_motif(w);
}

static void _xwl_handle_config_req(xcb_configure_request_event_t *e) {
    _xwl_win_t *w = _xwl_find(e->window);
    _wl_state_t *st = X.st;
    if (!w) {
        /* unknown window configuring itself: allow it as-is */
        uint16_t mask = 0;
        uint32_t vals[7] = {0};
        int vi = 0;
        if (e->value_mask & XCB_CONFIG_WINDOW_X)
            { mask |= XCB_CONFIG_WINDOW_X; vals[vi++] = e->x; }
        if (e->value_mask & XCB_CONFIG_WINDOW_Y)
            { mask |= XCB_CONFIG_WINDOW_Y; vals[vi++] = e->y; }
        if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            { mask |= XCB_CONFIG_WINDOW_WIDTH; vals[vi++] = e->width; }
        if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            { mask |= XCB_CONFIG_WINDOW_HEIGHT; vals[vi++] = e->height; }
        if (mask)
            xcb_configure_window(X.xc, e->window, mask, vals);
        return;
    }
    int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
    if (e->value_mask & XCB_CONFIG_WINDOW_X) nx = e->x;
    if (e->value_mask & XCB_CONFIG_WINDOW_Y) ny = e->y;
    if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) nw = e->width;
    if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) nh = e->height;
    if (w->override_redirect || !w->mapped) {
        _xwl_configure(w, nx, ny, nw, nh);
        return;
    }
    /* honor client requests but keep windows on screen */
    if (st) {
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + nw > st->out_w) nx = st->out_w - nw;
        if (ny + nh > st->out_h) ny = st->out_h - nh;
    }
    _xwl_configure(w, nx, ny, nw, nh);
    if (w->surf) {
        w->surf->x = w->x;
        w->surf->y = w->y;
        w->surf->w = w->w;   /* Xwayland will commit a matching buffer */
        w->surf->h = w->h;
    }
    if (w->toplevel)
        _emit_win(st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
    if (st) st->dirty = true;
}

static void _xwl_handle_client_msg(xcb_client_message_event_t *e) {
    _wl_state_t *st = X.st;
    if (e->type == X.a_net_wm_state && st) {
        _xwl_win_t *w = _xwl_find(e->window);
        if (!w) return;
        xcb_atom_t action = e->data.data32[0];
        xcb_atom_t prop = e->data.data32[1];
        bool on = (action == 1 /* _NET_WM_STATE_ADD */);
        if (prop == X.a_net_wm_state_fullscr)
            _xwl_fullscreen(w->surf, on);
        else if (prop == X.a_net_wm_state_maxv ||
                 prop == X.a_net_wm_state_maxh)
            _xwl_maximize(w->surf, on);
    } else if (e->type == X.a_net_active) {
        _xwl_win_t *w = _xwl_find(e->window);
        if (w && st) {
            _xwl_focus(w);
            if (w->toplevel) {
                st->focused_toplevel = w->toplevel;
                if (w->surf) _kbd_enter_focus(st, w->surf);
                _emit_win(st, VT_BACKEND_WL_EVENT_WIN_FOCUS, w->toplevel);
            }
        }
    } else if (e->type == X.a_net_close) {
        _xwl_win_t *w = _xwl_find(e->window);
        if (w) _xwl_close_win(w);
    } else if (e->type == X.a_net_current_desktop && st) {
        int d = (int)e->data.data32[0];
        _xwl_ws_switch(d);
    } else if (e->type == X.a_wm_change_state) {
        _xwl_win_t *w = _xwl_find(e->window);
        if (w && e->data.data32[0] == 3 /* IconicState */)
            _xwl_minimize(w->surf, true);
    }
}

void _xwl_dispatch(void) {
    if (!X.started || !X.xc) return;
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(X.xc))) {
        switch (ev->response_type & 0x7f) {
        case XCB_CREATE_NOTIFY:  _xwl_handle_create((void *)ev); break;
        case XCB_MAP_REQUEST:    _xwl_handle_map_request((void *)ev); break;
        case XCB_DESTROY_NOTIFY: _xwl_handle_destroy((void *)ev); break;
        case XCB_UNMAP_NOTIFY:   _xwl_handle_unmap((void *)ev); break;
        case XCB_PROPERTY_NOTIFY:_xwl_handle_property((void *)ev); break;
        case XCB_CONFIGURE_REQUEST:
            _xwl_handle_config_req((void *)ev); break;
        case XCB_CLIENT_MESSAGE: _xwl_handle_client_msg((void *)ev); break;
        default: break;
        }
        free(ev);
    }
    xcb_flush(X.xc);
}

/* ------------------------------------------------- backend-core hooks */

void _xwl_surface_destroyed(_wl_surf_t *s) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w) return;
    w->surf = NULL;
    if (w->toplevel) {
        /* the wl_surface is gone (client died): the X window follows */
        xcb_destroy_window(X.xc, w->xwin);
        _xwl_free_win(w);
    }
}

void _xwl_focus_changed(_wl_state_t *st, _wl_surf_t *s) {
    (void)st;
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (w) {
        if (!w->mapped) return;
        _xwl_focus(w);
        if (w->toplevel) w->toplevel->activated = true;
    }
}

void _xwl_move_resize(_wl_surf_t *s, int x, int y, int w, int h) {
    _xwl_win_t *win = _xwl_find_by_surf(s);
    if (!win || !win->mapped) return;
    _xwl_configure(win, x, y, w, h);
    if (win->toplevel && X.st)
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, win->toplevel);
}

void _xwl_close(_wl_surf_t *s) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (w) _xwl_close_win(w);
}

void _xwl_maximize(_wl_surf_t *s, bool on) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w || !w->mapped || !X.st) return;
    int wx, wy, ww, wh;
    _layer_workarea(X.st, &wx, &wy, &ww, &wh);
    if (on) {
        w->h = 0; /* remember nothing: X11 WMs usually save geometry;
                     we keep it simple: restore = recenter */
        _xwl_configure(w, wx, wy, ww, wh);
    } else {
        _xwl_configure(w, wx + (ww - w->w) / 2, wy + (wh - w->h) / 2,
                       w->w, w->h);
    }
    if (w->toplevel) {
        w->toplevel->maximized = on;
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE, w->toplevel);
    }
    X.st->dirty = true;
}

void _xwl_fullscreen(_wl_surf_t *s, bool on) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w || !w->mapped || !X.st) return;
    if (on)
        _xwl_configure(w, 0, 0, X.st->out_w, X.st->out_h);
    else {
        int wx, wy, ww, wh;
        _layer_workarea(X.st, &wx, &wy, &ww, &wh);
        _xwl_configure(w, wx + (ww - w->w) / 2, wy + (wh - w->h) / 2,
                       w->w, w->h);
    }
    if (w->toplevel) {
        w->toplevel->fullscreen = on;
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE, w->toplevel);
    }
    X.st->dirty = true;
}

void _xwl_minimize(_wl_surf_t *s, bool on) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w || !w->mapped) return;
    if (on) {
        xcb_unmap_window(X.xc, w->xwin);
        w->mapped = false;
    } else {
        xcb_map_window(X.xc, w->xwin);
        w->mapped = true;
        _xwl_focus(w);
    }
    if (w->toplevel) {
        w->toplevel->minimized = on;
        if (X.st) _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE,
                            w->toplevel);
    }
}

void _xwl_set_workspace(_wl_surf_t *s, int ws) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (w && w->surf) w->surf->ws = ws;
}

int _xwl_ws_switch(int ws) {
    if (!X.st) return -1;
    return 0;   /* the backend core owns the switch; we only observe */
}

void _xwl_workspace_changed(_wl_state_t *st) {
    (void)st;
    if (!X.xc || !X.started) return;
    /* hide X windows not on the current workspace, show the others */
    for (_xwl_win_t *w = X.wins; w; w = w->next) {
        if (!w->toplevel || w->override_redirect) continue;
        bool on_ws = !w->surf || w->surf->ws == X.st->ws_cur;
        if (on_ws && w->toplevel->minimized) continue;
        if (on_ws != w->mapped) {
            if (on_ws) {
                xcb_map_window(X.xc, w->xwin);
                w->mapped = true;
            } else {
                xcb_unmap_window(X.xc, w->xwin);
                w->mapped = false;
            }
        }
    }
    xcb_change_property(X.xc, XCB_PROP_MODE_REPLACE,
                        xcb_setup_roots_iterator(xcb_get_setup(X.xc)).data
                            ->root,
                        X.a_net_current_desktop, XCB_ATOM_CARDINAL, 32, 1,
                        &(uint32_t){ (uint32_t)X.st->ws_cur });
    xcb_flush(X.xc);
}

/* ----------------------------------------------------------- startup */

static xcb_atom_t _atom(xcb_connection_t *xc, const char *name) {
    size_t namelen = strlen(name);
    xcb_intern_atom_cookie_t c = xcb_intern_atom(
        xc, 0, (uint16_t)(namelen > 512 ? 512 : namelen), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(xc, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    if (r) free(r);
    return a;
}

/* write a minimal Xauthority file (wildcard family) */
static bool _xwl_write_auth(const char *path, uint32_t display) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    unsigned char cookie[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, cookie, sizeof(cookie)) != (ssize_t)sizeof(cookie)) {
        for (size_t i = 0; i < sizeof(cookie); i++)
            cookie[i] = (unsigned char)(rand() ^ (i << 3));
    }
    if (fd >= 0) close(fd);
    (void)display;
    /* record: family(u16) addr-len(u16) addr num-len(u16) num
     * name-len(u16) name val-len(u16) val */
    uint16_t family = 0xFFFF;             /* FamilyWild */
    uint16_t alen = 0, nlen = 1, namelen = 18, vlen = 16;
    char num[2] = { (char)('0' + (display % 10)), 0 };
    fwrite(&family, 2, 1, f);
    fwrite(&alen, 2, 1, f);
    fwrite(&nlen, 2, 1, f);
    fwrite(num, 1, 1, f);
    fwrite(&namelen, 2, 1, f);
    fwrite("MIT-MAGIC-COOKIE-1", 1, 18, f);
    fwrite(&vlen, 2, 1, f);
    fwrite(cookie, 1, 16, f);
    fclose(f);
    return true;
}

bool _xwl_start(_wl_state_t *st) {
    if (X.started) return true;
    X.st = st;
    /* Xwayland is real functionality, not a test knob: run it whenever
     * a real session runs (skip only when the caller explicitly
     * disabled it or in a no-fork environment) */
    const char *env = getenv("VANTAGE_XWAYLAND");
    if (env && *env == '0') {
        vt_logi("xwayland: disabled by VANTAGE_XWAYLAND=0");
        return false;
    }

    /* xwayland_shell_v1 global FIRST: Xwayland enumerates the registry
     * while we wait for its display number (the pump below dispatches
     * its connection), and rootless Xwayland refuses to run without
     * this protocol — it must be advertised before it connects. */
    st->xwl_shell_g = wl_global_create(
        st->display, &xwayland_shell_v1_interface, 1, NULL,
        _bind_xwl_shell);
    if (!st->xwl_shell_g) {
        vt_logw("xwayland: cannot create the xwayland_shell_v1 global");
        return false;
    }

    /* 1. Xauthority cookie */
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) rd = "/tmp";
    snprintf(X.auth_file, sizeof(X.auth_file), "%s/vantage-xwayland.Xauth",
             rd);
    if (!_xwl_write_auth(X.auth_file, 0)) {
        vt_logw("xwayland: cannot write %s — X11 apps may fail to "
                "connect", X.auth_file);
    }

    /* Xwayland's socket directory (it only creates it when running as
     * root); best effort — Xwayland falls back to abstract sockets */
    mkdir("/tmp/.X11-unix", 01777);

    /* 2. display fd pipe + spawn */
    int dispfd[2];
    if (pipe(dispfd) != 0) {
        vt_logw("xwayland: pipe() failed: %s", strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        /* child: Xwayland. It inherits our socket env (WAYLAND_DISPLAY)
         * and connects back to us as a normal client. */
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        /* Reset inherited signal dispositions the compositor changed:
         * SIGCHLD=SIG_IGN (we auto-reap launched apps) would make
         * Xwayland's popen()-based xkbcomp invocations un-reapable, so
         * Pclose() fails and XKB keymap compilation aborts — the whole
         * "Keyboard initialization failed" cascade. exec() keeps IGNORED
         * dispositions, so this MUST be reset here. */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        if (getenv("VANTAGE_DEBUG_XWAYLAND"))
            setenv("WAYLAND_DEBUG", "1", 1);
        for (int i = 3; i < 1024; i++) {
            if (i != dispfd[1]) close(i);
        }
        char fdstr[8];
        snprintf(fdstr, sizeof(fdstr), "%d", dispfd[1]);
        /* -nokeymap: Xwayland compiles its own keymap from the SAME
         * environment defaults instead of translating ours through the
         * xkbcomp stdin → .xkm load path. That path fails in some
         * environments (xkbcomp clips evdev keycodes > 255, ignores the
         * affected symbol rows, and the resulting .xkm then fails
         * XkmReadFile's section checks) — with -nokeymap Xwayland's own
         * RMLVO compile, proven to work here, is used instead. Raw
         * keycodes still arrive through wl_seat, so typing works; both
         * sides pick identical layouts for default configurations. */
        char *const argv[] = {
            (char *)"Xwayland",
            (char *)"-displayfd", fdstr,
            (char *)"-rootless",
            (char *)"-nokeymap",
            (char *)"-auth", X.auth_file,
            (char *)"-noreset",
            NULL
        };
        execvp("Xwayland", argv);
        _exit(127);
    }
    close(dispfd[1]);
    if (pid < 0) {
        close(dispfd[0]);
        vt_logw("xwayland: fork failed: %s", strerror(errno));
        return false;
    }
    X.pid = pid;
    /* 3. read the display number (one byte ASCII). While waiting, PUMP
     * the compositor's own event loop: Xwayland connects back to us and
     * completes its registry handshake DURING this wait — without the
     * pump both sides deadlock (Xwayland times out, we see no display
     * number) and X11 apps never launch. */
    int display = -1;
    struct pollfd pfd = { .fd = dispfd[0], .events = POLLIN };
    int nread = 0;
    char dbuf[8] = {0};
    for (int waited = 0; waited < 8000; waited += 20) {
        wl_event_loop_dispatch(st->loop, 0);
        wl_display_flush_clients(st->display);
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 20);
        if (pr == 1 && (pfd.revents & POLLIN)) {
            char c;
            ssize_t r = read(dispfd[0], &c, 1);
            if (r <= 0) break;
            if (c == '\n') break;
            if (nread < (int)sizeof(dbuf) - 1 && c >= '0' && c <= '9')
                dbuf[nread++] = c;
            continue;
        }
        if (pr < 0 && errno != EINTR) break;
        if (waitpid(pid, NULL, WNOHANG) != 0) {
            vt_logw("xwayland: process exited before reporting a "
                    "display number");
            close(dispfd[0]);
            X.started = false;
            return false;
        }
    }
    close(dispfd[0]);
    display = nread > 0 ? atoi(dbuf) : -1;
    if (display < 0) {
        vt_logw("xwayland: no display number within 8s — X11 apps "
                "stay unavailable this session");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return false;
    }
    X.display = display;
    X.started = true;

    /* 4. connect as the window manager of that display */
    char disp[32];
    snprintf(disp, sizeof(disp), ":%d", display);
    setenv("DISPLAY", disp, 1);
    setenv("XAUTHORITY", X.auth_file, 1);
    X.xc = xcb_connect(disp, NULL);
    if (xcb_connection_has_error(X.xc)) {
        vt_logw("xwayland: cannot connect to the new X display %s",
                disp);
        xcb_disconnect(X.xc);
        X.xc = NULL;
        return false;
    }

    /* 5. atoms */
    xcb_connection_t *xc = X.xc;
    X.a_wm_name        = _atom(xc, "WM_NAME");
    X.a_net_wm_name    = _atom(xc, "_NET_WM_NAME");
    X.a_wm_class       = _atom(xc, "WM_CLASS");
    X.a_wm_protocols   = _atom(xc, "WM_PROTOCOLS");
    X.a_wm_delete      = _atom(xc, "WM_DELETE_WINDOW");
    X.a_wm_take_focus  = _atom(xc, "WM_TAKE_FOCUS");
    X.a_wm_normal_hints= _atom(xc, "WM_NORMAL_HINTS");
    X.a_motif_hints    = _atom(xc, "_MOTIF_WM_HINTS");
    X.a_net_wm_state   = _atom(xc, "_NET_WM_STATE");
    X.a_net_wm_state_fullscr = _atom(xc, "_NET_WM_STATE_FULLSCREEN");
    X.a_net_wm_state_maxv = _atom(xc, "_NET_WM_STATE_MAXIMIZED_VERT");
    X.a_net_wm_state_maxh = _atom(xc, "_NET_WM_STATE_MAXIMIZED_HORZ");
    X.a_net_wm_icon    = _atom(xc, "_NET_WM_ICON");
    X.a_net_wm_window_type = _atom(xc, "_NET_WM_WINDOW_TYPE");
    X.a_net_active     = _atom(xc, "_NET_ACTIVE_WINDOW");
    X.a_net_close      = _atom(xc, "_NET_CLOSE_WINDOW");
    X.a_net_current_desktop = _atom(xc, "_NET_CURRENT_DESKTOP");
    X.a_net_number_desktops = _atom(xc, "_NET_NUMBER_OF_DESKTOPS");
    X.a_net_client_list   = _atom(xc, "_NET_CLIENT_LIST");
    X.a_net_supporting_check = _atom(xc, "_NET_SUPPORTING_WM_CHECK");
    X.a_net_supported  = _atom(xc, "_NET_SUPPORTED");
    X.a_net_workarea   = _atom(xc, "_NET_WORKAREA");
    X.a_net_wm_desktop = _atom(xc, "_NET_WM_DESKTOP");
    X.a_utf8_string    = _atom(xc, "UTF8_STRING");
    X.a_net_wm_ping    = _atom(xc, "_NET_WM_PING");
    X.a_wm_change_state = _atom(xc, "WM_CHANGE_STATE");
    X.a_net_wm_visible_name = _atom(xc, "_NET_WM_VISIBLE_NAME");

    /* 6. become the WM: SubstructureRedirect on the root */
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(xc)).data;
    X.wm_screen_width = (int)scr->width_in_pixels;
    X.wm_screen_height = (int)scr->height_in_pixels;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(
        xc, scr->root, XCB_CW_EVENT_MASK,
        (const uint32_t[]){ XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                            XCB_EVENT_MASK_PROPERTY_CHANGE });
    xcb_generic_error_t *err = xcb_request_check(xc, ck);
    if (err) {
        vt_logw("xwayland: another WM already manages %s", disp);
        free(err);
        return false;
    }

    /* 7. EWMH presence on the nested display (so apps see a real WM) */
    xcb_window_t wmwin = xcb_generate_id(xc);
    xcb_create_window(xc, XCB_COPY_FROM_PARENT, wmwin, scr->root,
                      0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT, 0, NULL);
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmwin,
                        X.a_net_supporting_check, XCB_ATOM_WINDOW, 32, 1,
                        &wmwin);
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmwin,
                        XCB_ATOM_WM_NAME, X.a_utf8_string, 8, 7, "Vantage");
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, scr->root,
                        X.a_net_supporting_check, XCB_ATOM_WINDOW, 32, 1,
                        &wmwin);
    {
        xcb_atom_t supported[] = {
            X.a_net_wm_name, X.a_net_wm_state, X.a_net_wm_state_fullscr,
            X.a_net_wm_state_maxv, X.a_net_wm_state_maxh, X.a_net_active,
            X.a_net_close, X.a_net_current_desktop,
            X.a_net_number_desktops, X.a_net_client_list,
            X.a_net_wm_window_type, X.a_net_wm_icon, X.a_net_wm_desktop,
            X.a_wm_protocols, X.a_wm_take_focus, X.a_wm_delete,
        };
        xcb_change_property(xc, XCB_PROP_MODE_REPLACE, scr->root,
                            X.a_net_supported, XCB_ATOM_ATOM, 32,
                            sizeof(supported) / sizeof(supported[0]),
                            supported);
    }
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, scr->root,
                        X.a_net_number_desktops, XCB_ATOM_CARDINAL, 32, 1,
                        &(uint32_t){ 4 });
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, scr->root,
                        X.a_net_current_desktop, XCB_ATOM_CARDINAL, 32, 1,
                        &(uint32_t){ 0 });
    xcb_flush(xc);

    vt_logi("xwayland: ready — DISPLAY=%s (Xwayland pid %d, auth %s)",
            disp, (int)pid, X.auth_file);
    return true;
}

void _xwl_stop(_wl_state_t *st) {
    if (!X.started) return;
    if (st && st->xwl_shell_g) {
        wl_global_destroy(st->xwl_shell_g);
        st->xwl_shell_g = NULL;
    }
    if (X.xc) {
        xcb_disconnect(X.xc);
        X.xc = NULL;
    }
    if (X.pid > 0) {
        kill(X.pid, SIGTERM);
        waitpid(X.pid, NULL, WNOHANG);
    }
    _xwl_win_t *w = X.wins;
    while (w) {
        _xwl_win_t *n = w->next;
        _xwl_free_win(w);
        w = n;
    }
    unlink(X.auth_file);
    X.started = false;
    X.pid = 0;
    X.display = -1;
}

int _xwl_display(void) {
    return X.started ? X.display : -1;
}

const char *_xwl_auth_file(void) {
    return X.started ? X.auth_file : NULL;
}

#else /* !VT_HAVE_WAYLAND || !VT_HAVE_XCB_XWAYLAND */

/* stubs keep the backend core compiling without xcb */
bool _xwl_start(_wl_state_t *st) { (void)st; return false; }
void _xwl_stop(_wl_state_t *st) { (void)st; }
void _xwl_dispatch(void) {}
int _xwl_display(void) { return -1; }
const char *_xwl_auth_file(void) { return NULL; }
void _xwl_surface_destroyed(_wl_surf_t *s) { (void)s; }
void _xwl_focus_changed(_wl_state_t *st, _wl_surf_t *s) { (void)st; (void)s; }
void _xwl_move_resize(_wl_surf_t *s, int x, int y, int w, int h) { (void)s; (void)x; (void)y; (void)w; (void)h; }
void _xwl_close(_wl_surf_t *s) { (void)s; }
void _xwl_maximize(_wl_surf_t *s, bool on) { (void)s; (void)on; }
void _xwl_fullscreen(_wl_surf_t *s, bool on) { (void)s; (void)on; }
void _xwl_minimize(_wl_surf_t *s, bool on) { (void)s; (void)on; }
void _xwl_set_workspace(_wl_surf_t *s, int ws) { (void)s; (void)ws; }
int _xwl_ws_switch(int ws) { (void)ws; return -1; }
void _xwl_workspace_changed(_wl_state_t *st) { (void)st; }

#endif
