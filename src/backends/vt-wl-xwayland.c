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
#if defined(VT_HAVE_XCB_COMPOSITE)
#include <xcb/composite.h>
#endif

/* ------------------------------------------------------------ types */

typedef struct _xwl_win {
    xcb_window_t xwin;
    _wl_surf_t *surf;               /* associated wl_surface (may lag) */
    _xdg_toplevel_t *toplevel;      /* synthetic toplevel for the WM */
    uint64_t win_serial;            /* the window's association serial
                                     * (WL_SURFACE_SERIAL client message) */
    bool have_win_serial;
    uint64_t surf_serial;           /* the surface's association serial
                                     * (xwayland_surface_v1.set_serial) */
    bool have_surf_serial;
    uint32_t create_serial;         /* X sequence (legacy diagnostics) */
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
    struct wl_client *client;       /* the Xwayland wayland client —
                                       needed to resolve legacy
                                       WL_SURFACE_ID resource ids */
    /* atoms */
    xcb_atom_t a_wm_name, a_net_wm_name, a_wm_class, a_wm_protocols,
                a_wm_delete, a_wm_take_focus, a_wm_normal_hints,
                a_motif_hints, a_net_wm_state, a_net_wm_state_fullscr,
                a_net_wm_state_maxv, a_net_wm_state_maxh, a_net_wm_icon,
                a_net_wm_window_type, a_net_active, a_net_close,
                a_net_current_desktop, a_net_number_desktops,
                a_net_client_list, a_net_supporting_check, a_net_supported,
                a_net_workarea, a_net_wm_desktop, a_utf8_string,
                a_net_wm_ping, a_wm_change_state, a_net_wm_visible_name,
                a_wl_surface_id, a_wl_surface_serial;
    int wm_screen_width, wm_screen_height;
    /* legacy pairing: WL_SURFACE_ID messages that arrived BEFORE the
     * window's CreateNotify crossed the X socket, or BEFORE the
     * wl_surface resource itself was created on the wayland socket
     * (the X socket routinely runs AHEAD: Xwayland sends the client
     * message right after creating the surface, and the compositor may
     * read the X event first — without the stash the association was
     * silently DROPPED and the window's surface stranded at +0+0) */
    struct { xcb_window_t win; uint32_t surface_id; }
        pending_ids[16];
    size_t n_pending_ids;
    /* WL_SURFACE_SERIAL messages that arrived BEFORE the window's
     * CreateNotify crossed the X socket (same socket-ordering hazard,
     * same stash-then-retry cure) */
    struct { xcb_window_t win; uint64_t serial; }
        pending_serials[16];
    size_t n_pending_serials;
    /* association placeholders created by get_xwayland_surface: they
     * are NOT in X.wins (no X window yet) and their wl_surface may not
     * have committed yet (so it is not in st->surfaces either — the
     * serial reconcile used to iterate st->surfaces and could not see
     * a placeholder whose first commit was still in flight; the
     * pairing then NEVER happened and the surface rendered at +0+0
     * behind the panel forever) */
    _xwl_win_t *placeholders;
} X;

/* ------------------------------------------------------------- utils */

static void _xwl_free_win(_xwl_win_t *w) {
    if (!w) return;
    if (w->prev) w->prev->next = w->next;
    else if (X.wins == w) X.wins = w->next;
    if (w->next) w->next->prev = w->prev;
    if (w->surf) w->surf->xwl = NULL;
    /* The xwayland_surface_v1 resource MUST die with its record: its
     * destructor dereferences user_data — a resource left alive here
     * keeps a dangling pointer that the NEXT wl_display_destroy_clients
     * (or the client's own teardown) dereferences and WRITES through,
     * corrupting the heap ("corrupted size vs. prev_size", SIGABRT at
     * shutdown — reproduced with any X11 app mapped once). Destroying
     * it now runs the destructor on a LIVE record; the guard below is
     * pre-emptied so the destructor becomes a no-op. */
    if (w->xwl_res) {
        struct wl_resource *r = w->xwl_res;
        w->xwl_res = NULL;
        wl_resource_destroy(r);
    }
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

/* placeholder list plumbing: placeholders live in X.placeholders (NOT
 * X.wins) so the serial reconcile can find them even before their
 * wl_surface has committed (an uncommitted surface is in no list at
 * all — that invisibility was the lost-association root cause) */
static void _xwl_placeholder_link(_xwl_win_t *ph) {
    if (!ph) return;
    ph->prev = NULL;
    ph->next = X.placeholders;
    if (X.placeholders) X.placeholders->prev = ph;
    X.placeholders = ph;
}

static void _xwl_placeholder_unlink(_xwl_win_t *ph) {
    if (!ph) return;
    if (ph->prev) ph->prev->next = ph->next;
    else if (X.placeholders == ph) X.placeholders = ph->next;
    if (ph->next) ph->next->prev = ph->prev;
    ph->next = ph->prev = NULL;
}

/* free a placeholder that has been adopted into a real window record
 * (the caller has already moved its resource/serial to the record and
 * detached the surface) */
static void _xwl_placeholder_free(_xwl_win_t *ph) {
    if (!ph) return;
    _xwl_placeholder_unlink(ph);
    vt_free(ph);
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
    /* Move-only when wd/ht are 0: the interactive DRAG path calls
     * _xwl_move_resize(x, y, 0, 0) for every motion step, and the old
     * unconditional WIDTH|HEIGHT mask sent width=1 height=1 (the
     * "wd > 0 ? wd : 1" fallback) — ONE title-bar drag collapsed the
     * X window to 1x1 and its content vanished ("the application is
     * extremely small or effectively invisible"). Sizes join the mask
     * only when the caller actually passes them. */
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_BORDER_WIDTH;
    uint32_t vals[5] = { (uint32_t)x, (uint32_t)y, 0, 0, 0 };
    int vi = 2;
    if (wd > 0) {
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        vals[vi++] = (uint32_t)wd;
        w->w = wd;
    }
    if (ht > 0) {
        mask |= XCB_CONFIG_WINDOW_HEIGHT;
        vals[vi++] = (uint32_t)ht;
        w->h = ht;
    }
    xcb_configure_window(X.xc, w->xwin, mask, vals);
    w->x = x;
    w->y = y;
    /* Reposition the surface IMMEDIATELY: Xwayland does not commit a
     * buffer for WM-initiated moves (nothing changed client-side), so
     * the commit-driven _xwl_win_geom sync may never run — the surface
     * kept painting at its pre-drag position and the window appeared
     * stranded until the client happened to redraw. The X window's
     * position IS the surface's position by design ("the surface
     * paints exactly where the X window is"). Sizes follow the same
     * rule: the MODEL must show the configured geometry right away
     * (the pager/taskbar read it); the paint bounds stay min(buffer,
     * configured) so a not-yet-redrawn client simply paints its old
     * content in the top-left of the new frame. */
    if (w->surf) {
        w->surf->x = x;
        w->surf->y = y;
        if (wd > 0) w->surf->w = wd;
        if (ht > 0) w->surf->h = ht;
    }
    /* keep the toplevel's geometry mirror current (used by events
     * while the window has no surface yet) */
    if (w->toplevel) {
        w->toplevel->x = w->x;
        w->toplevel->y = w->y;
        w->toplevel->w = w->w;
        w->toplevel->h = w->h;
    }
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

static void _xwl_sever(_xwl_win_t *w);
static void _xwl_serial_reconcile(void);
static void _xwl_pending_ids_resolve(void);
static void _xwl_late_pair(_xwl_win_t *w, _wl_surf_t *s);

/* Resolve stashed legacy WL_SURFACE_ID associations: each entry holds
 * (X window, wl_surface resource id) for a message that crossed the X
 * socket before the wl_surface resource existed compositor-side (or
 * before the window's CreateNotify). Retried whenever a new surface
 * appears (get_xwayland_surface, surface commit) and whenever a new
 * window record is created. */
static void _xwl_pending_ids_resolve(void) {
    if (!X.client || !X.st) return;
    for (size_t i = 0; i < X.n_pending_ids; ) {
        _xwl_win_t *w = _xwl_find(X.pending_ids[i].win);
        if (!w) { i++; continue; }   /* CreateNotify still in flight */
        struct wl_resource *r = wl_client_get_object(
            X.client, X.pending_ids[i].surface_id);
        _wl_surf_t *s = (r &&
                         wl_resource_instance_of(
                             r, &wl_surface_interface, NULL))
                            ? wl_resource_get_user_data(r) : NULL;
        if (!s) { i++; continue; }   /* surface still in flight */
        if (w->surf != s) {
            vt_logd("xwayland: stashed WL_SURFACE_ID resolved: 0x%x",
                    (unsigned)w->xwin);
            _xwl_late_pair(w, s);
        }
        X.pending_ids[i] = X.pending_ids[X.n_pending_ids - 1];
        X.n_pending_ids--;
    }
}

static void _xwl_surface_destroy_req(struct wl_client *cli,
                                     struct wl_resource *res) {
    (void)cli;
    _xwl_win_t *w = wl_resource_get_user_data(res);
    _xwl_sever(w);   /* drops w->xwl_res too: destructor becomes a no-op */
    /* a PLACEHOLDER dying with its wrapper has no X window to wait
     * for — detach the resource's user_data (the destructor below must
     * not read a freed record) and free it */
    if (w && w->xwin == 0) {
        wl_resource_set_user_data(res, NULL);
        _xwl_placeholder_free(w);
    }
    wl_resource_destroy(res);
}

static void _xwl_surface_set_serial(struct wl_client *cli,
                                    struct wl_resource *res,
                                    uint32_t serial_lo,
                                    uint32_t serial_hi) {
    /* NOTE the arg order: the protocol request is set_serial(lo, hi)
     * — libwayland passes wire order, so the FIRST parameter is the
     * low word. (The old code named them (hi, lo) and composed
     * (hi<<32)|lo from swapped halves — serial 1 read as 2^32.) */
    (void)cli;
    _xwl_win_t *w = wl_resource_get_user_data(res);
    if (!w) return;
    uint64_t serial = ((uint64_t)serial_hi << 32) | serial_lo;
    w->surf_serial = serial;
    w->have_surf_serial = true;
    vt_logd("xwayland: surface serial %llu (record 0x%x)",
            (unsigned long long)serial, (unsigned)w->xwin);
    _xwl_serial_reconcile();
    /* the reconcile above may have just adopted this placeholder into
     * a real window record — nothing else to do either way */
}

static const struct xwayland_surface_v1_interface _xwl_surface_impl = {
    .destroy     = _xwl_surface_destroy_req,
    .set_serial  = _xwl_surface_set_serial,
};

/* Sever the surface↔window pairing WITHOUT killing the X window:
 * Xwayland recycles a window's wl_surface (pixmap format changes)
 * by destroying the xwayland_surface wrapper and the wl_surface,
 * then pairing a NEW surface with the SAME X window. The toplevel's
 * taskbar identity must move FULLY to the X record for the gap:
 * leaving s->toplevel set made the wl_surface's destructor FREE the
 * toplevel while w->toplevel still pointed at it — every later X
 * event (unmap/destroy) read freed memory and _xwl_free_win
 * double-freed it (the shutdown heap corruption). A later surface
 * re-links via _xwl_late_pair. */
static void _xwl_sever(_xwl_win_t *w) {
    if (!w) return;
    if (w->surf) {
        _wl_surf_t *s = w->surf;
        if (s->xwl == w) s->xwl = NULL;
        if (s->toplevel && s->toplevel == w->toplevel)
            s->toplevel = NULL;
        w->surf = NULL;
    }
    if (w->toplevel && w->toplevel->surf)
        w->toplevel->surf = NULL;
    w->xwl_res = NULL;
    /* NOTE: a placeholder whose wrapper died is freed by the CALLERS
     * (after the resource destructor can no longer read the record) */
}

static void _xwl_surface_res_destroy(struct wl_resource *res) {
    _xwl_win_t *w = wl_resource_get_user_data(res);
    if (w && w->xwl_res == res) {
        _xwl_sever(w);
        /* a placeholder whose wrapper died from the CLIENT side
         * (disconnect) would leak in X.placeholders forever */
        if (w->xwin == 0) {
            wl_resource_set_user_data(res, NULL);
            _xwl_placeholder_free(w);
        }
    }
}

/* The wl surface can arrive AFTER the X window was already mapped —
 * Xwayland creates it when the window's first pixmap is ready. The
 * WIN_MAP event fired back then with no surface attached (the WM
 * model fell back to 1x1 at +0+0 and the surface rendered at +0+0,
 * title bar behind the panel). Once the surface pairs up with the
 * window, backlink the toplevel, seed the surface's geometry and
 * announce the real position. */
static void _xwl_late_pair(_xwl_win_t *w, _wl_surf_t *s) {
    if (!w || !s) return;
    /* the surface may currently be paired with a PLACEHOLDER record
     * that get_xwayland_surface created while waiting for the X
     * window — adopt its resource and free it instead of orphaning
     * it: an orphaned placeholder's resource destructor would later
     * NULL s->xwl out from under THIS window (the pairing silently
     * breaks; geometry sync and teardown go through s->xwl). */
    _xwl_win_t *ph = s->xwl;
    if (ph && ph != w && ph->xwin == 0) {
        if (ph->xwl_res && !w->xwl_res) {
            w->xwl_res = ph->xwl_res;
            wl_resource_set_user_data(w->xwl_res, w);
        }
        ph->xwl_res = NULL;
        ph->surf = NULL;
        _xwl_placeholder_free(ph);   /* unlink + free */
    }
    w->surf = s;
    s->xwl = w;
    /* SEED THE GEOMETRY UNCONDITIONALLY: the surface's position up to
     * this point was the placeholder's +0+0 (a placeholder carries no
     * X geometry). The old code only seeded inside the toplevel
     * backlink branch — a window whose toplevel already had ANOTHER
     * surface (racing re-association) kept rendering at +0+0. */
    s->x = w->x;
    s->y = w->y;
    if (w->w > 0) s->w = w->w;
    if (w->h > 0) s->h = w->h;
    s->ws = X.st ? X.st->ws_cur : 0;
    /* the SSD MODE too: the first commit already ran while the surface
     * was still placeholder-paired (_xwl_win_geom returns early there),
     * so nobody ever told the surface it is server-decorated — no SSD
     * frame rendered, no title-bar hit-box, the window could not be
     * dragged or closed through OUR chrome */
    s->ssd = !w->motif_csd && !w->override_redirect;
    if (w->toplevel && !w->toplevel->surf) {
        w->toplevel->surf = s;
        s->toplevel = w->toplevel;
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
    }
    if (X.st) X.st->dirty = true;
    vt_logd("xwayland: surface paired late with 0x%x (geometry "
            "re-announced at +%d+%d)", (unsigned)w->xwin, w->x, w->y);
}

/* ------------------------------------------------- serial association
 * xwayland_shell_v1's EXACT window↔surface match. The same unique
 * serial arrives on the wl_surface (set_serial) and on the X window
 * (WL_SURFACE_SERIAL client message, l[0]=lo l[1]=hi). This runs after
 * every serial event and:
 *
 *   1. pairs unpaired PLACEHOLDERS (xwin == 0) with the window whose
 *      win_serial equals the placeholder's surf_serial;
 *   2. CORRECTS a wrong pairing: if a window's surf_serial (learned
 *      from set_serial on the surface it currently holds) differs
 *      from its own win_serial, the surface belongs to the window
 *      with the matching serial — swap/move it there. This repairs
 *      pairings made by the legacy guess before the serials arrived.
 */
static _xwl_win_t *_xwl_win_by_serial(uint64_t serial) {
    for (_xwl_win_t *w = X.wins; w; w = w->next)
        if (w->have_win_serial && w->win_serial == serial)
            return w;
    return NULL;
}

static void _xwl_serial_reconcile(void) {
    if (!X.st) return;
    /* 1) pair placeholders by exact serial. Iterate the PLACEHOLDER
     * LIST, not st->surfaces: an uncommitted wl_surface sits in NO
     * list until its first commit, so a surface whose set_serial
     * preceded its first commit was invisible here — with the X
     * socket running ahead of the wayland socket (Xwayland sends the
     * WL_SURFACE_SERIAL client message immediately after creating
     * the surface) the pairing silently NEVER happened and the window
     * rendered at +0+0 behind the panel, full buffer size, forever
     * (the "racing XWayland windows swapped" failure on real
     * hardware). */
    for (_xwl_win_t *ph = X.placeholders; ph; ) {
        _xwl_win_t *next_ph = ph->next;
        if (!ph->surf || !ph->have_surf_serial) {
            ph = next_ph;
            continue;
        }
        _xwl_win_t *w = _xwl_win_by_serial(ph->surf_serial);
        if (w && !w->surf) {
            /* adopt the placeholder into the real window and attach.
             * Detach s->xwl BEFORE the free: _xwl_late_pair reads it
             * (its own placeholder-merge step) — leaving the dangling
             * pointer was a heap-use-after-free under ASan. */
            _wl_surf_t *s = ph->surf;
            if (ph->xwl_res) {
                w->xwl_res = ph->xwl_res;
                wl_resource_set_user_data(w->xwl_res, w);
                ph->xwl_res = NULL;
            }
            w->have_surf_serial = true;
            w->surf_serial = ph->surf_serial;
            ph->surf = NULL;
            s->xwl = NULL;
            _xwl_placeholder_free(ph);
            _xwl_late_pair(w, s);
            vt_logd("xwayland: serial %llu paired surface with 0x%x",
                    (unsigned long long)w->surf_serial, (unsigned)w->xwin);
        }
        ph = next_ph;
    }
    /* 2) repair wrong pairings (win_serial vs surf_serial mismatch) */
    for (_xwl_win_t *w = X.wins; w; w = w->next) {
        if (!w->surf || !w->have_win_serial || !w->have_surf_serial)
            continue;
        if (w->win_serial == w->surf_serial)
            continue;              /* consistent */
        _xwl_win_t *w2 = _xwl_win_by_serial(w->surf_serial);
        if (!w2 || w2 == w)
            continue;              /* unknown owner: wait for more serials */
        /* the surface W holds belongs to W2. SWAP the surfaces so both
         * windows get their own content back, keeping each toplevel
         * (taskbar identity) with its X window. */
        _wl_surf_t *sa = w->surf, *sb = w2->surf;
        struct wl_resource *ra = w->xwl_res, *rb = w2->xwl_res;
        vt_logd("xwayland: serial mismatch on 0x%x (win=%llu surf=%llu)"
                " — repairing pairing with 0x%x",
                (unsigned)w->xwin,
                (unsigned long long)w->win_serial,
                (unsigned long long)w->surf_serial, (unsigned)w2->xwin);
        w->surf = sb;  w2->surf = sa;
        w->xwl_res = rb; w2->xwl_res = ra;
        if (ra) wl_resource_set_user_data(ra, w2);
        if (rb) wl_resource_set_user_data(rb, w);
        if (sa) {
            sa->xwl = w2;
            if (sa->toplevel && sa->toplevel == w->toplevel)
                sa->toplevel = NULL;
            if (w->toplevel && w->toplevel->surf == sa)
                w->toplevel->surf = NULL;
            if (w2->toplevel && !w2->toplevel->surf) {
                w2->toplevel->surf = sa;
                sa->toplevel = w2->toplevel;
            }
            sa->x = w2->x; sa->y = w2->y;
            if (w2->w > 0) sa->w = w2->w;
            if (w2->h > 0) sa->h = w2->h;
        }
        if (sb) {
            sb->xwl = w;
            if (sb->toplevel && sb->toplevel == w2->toplevel)
                sb->toplevel = NULL;
            if (w2->toplevel && w2->toplevel->surf == sb)
                w2->toplevel->surf = NULL;
            if (w->toplevel && !w->toplevel->surf) {
                w->toplevel->surf = sb;
                sb->toplevel = w->toplevel;
            }
            sb->x = w->x; sb->y = w->y;
            if (w->w > 0) sb->w = w->w;
            if (w->h > 0) sb->h = w->h;
        }
        /* re-learn the surface serials: each window now holds a
         * surface whose serial equals its own win_serial (that is
         * what the repair guarantees when both sides have serials) */
        if (sa && w2->have_win_serial) {
            w2->have_surf_serial = true;
            w2->surf_serial = w2->win_serial;
        }
        if (sb && w->have_win_serial) {
            w->have_surf_serial = true;
            w->surf_serial = w->win_serial;
        }
        if (w->toplevel)
            _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
        if (w2->toplevel)
            _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w2->toplevel);
        if (X.st) X.st->dirty = true;
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
    /* Xwayland creates the surface BEFORE the X window is fully wired.
     * Record a PLACEHOLDER (xwin == 0, never linked into X.wins) and
     * let the SERIAL reconcile it with the right window —
     * xwayland_shell_v1 delivers the same unique serial on the
     * wl_surface (set_serial) and on the X window (WL_SURFACE_SERIAL
     * client message), so the pairing is EXACT. The old
     * adopt-the-newest-waiting-window GUESS swapped surfaces between
     * racing windows (a group leader + several toplevels mapping in
     * one burst): each window then rendered at ANOTHER window's
     * geometry — "the app is effectively invisible" with the pager
     * showing a tiny misplaced window. */
    _xwl_win_t *w = vt_malloc0(sizeof(*w));
    if (!w) { wl_client_post_no_memory(cli); return; }
    w->surf = s;
    s->xwl = w;
    _xwl_placeholder_link(w);
    struct wl_resource *r = wl_resource_create(
        cli, &xwayland_surface_v1_interface, 1, id);
    if (!r) { s->xwl = NULL; _xwl_placeholder_free(w); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_xwl_surface_impl, w,
                                   _xwl_surface_res_destroy);
    w->xwl_res = r;
    vt_logd("xwayland: get_xwayland_surface request arrived");
    /* maybe the serials are already here (both sides raced ahead) */
    _xwl_serial_reconcile();
    /* a stashed legacy WL_SURFACE_ID for this surface (the X message
     * crossed before the wl_surface resource existed) can resolve
     * now that the surface is created */
    _xwl_pending_ids_resolve();
    /* Legacy Xwayland (no serials at all, ever): the placeholder is
     * reconciled by _xwl_win_geom's commit-time fallback below. */
    (void)w;
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
    vt_logd("xwayland: client bound xwayland_shell_v1");
    /* whoever binds the shell IS the Xwayland server's wayland client;
     * kept for resolving legacy WL_SURFACE_ID resource ids (some
     * Xwayland builds bind the shell but still use the legacy path) */
    X.client = cli;
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
    if (!w || w->xwin == 0) {
        /* The surface is still tied to a PLACEHOLDER (or nothing): its
         * X window may simply not have been paired YET. Run the serial
         * reconcile first — with the X socket running ahead of the
         * wayland socket the WL_SURFACE_SERIAL client message can
         * precede BOTH get_xwayland_surface and the first commit, and
         * this commit is the first moment all three sides coexist. */
        _xwl_serial_reconcile();
        _xwl_pending_ids_resolve();
        w = s ? s->xwl : NULL;
        if (w && w->xwin == 0 && !w->have_surf_serial) {
            /* Legacy Xwayland (no serials, ever): an UNPAIRED
             * placeholder reaching its first commit falls back to the
             * recency guess — adopt the newest waiting window. Modern
             * Xwayland is reconciled by serial above. */
            _xwl_win_t *ph = w;
            for (_xwl_win_t *cand = X.wins; cand; cand = cand->next) {
                if (cand->waiting_surface && !cand->surf) {
                    if (ph->xwl_res) {
                        cand->xwl_res = ph->xwl_res;
                        wl_resource_set_user_data(cand->xwl_res, cand);
                        ph->xwl_res = NULL;
                    }
                    ph->surf = NULL;
                    s->xwl = NULL;   /* detach before free (UAF guard) */
                    _xwl_placeholder_free(ph);
                    _xwl_late_pair(cand, s);
                    vt_logd("xwayland: commit-time fallback pairing for "
                            "0x%x (no serials)", (unsigned)cand->xwin);
                    w = cand;
                    break;
                }
            }
        }
        if (!w || w->xwin == 0) {
            /* STILL a placeholder: applying its geometry would park a
             * real window at +0+0 behind the panel (the exact
             * "racing windows swapped" symptom). Leave the surface
             * where it is and wait for the real pairing. */
            return;
        }
    }
    int ox = s->x, oy = s->y, ow = s->w, oh = s->h;
    s->x = w->x;
    s->y = w->y;
    if (w->w > 0) s->w = w->w;
    if (w->h > 0) s->h = w->h;
    s->ssd = !w->motif_csd && !w->override_redirect;
    /* The WIN_MAP event fired before any buffer existed — the WM model
     * fell back to a 1x1 window (invisible in the pager, useless in
     * the taskbar tooltip). The first commit that carries the real
     * geometry must propagate it. Emit ONLY on change: this runs on
     * every redraw, and a per-commit event would recreate the event
     * flood the broadcast path just learned to survive. */
    if (w->toplevel && X.st &&
        (s->x != ox || s->y != oy || s->w != ow || s->h != oh))
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
}

/* first commit of a mapped Xwayland surface: the generic attach path
 * already sized the surface from the buffer BEFORE _xwl_win_geom's
 * change detection could see the 0→N transition — the WM model would
 * keep its 1x1 fallback forever. Announce once per mapping. */
void _xwl_announce_geom(_wl_surf_t *s) {
    _xwl_win_t *w = s ? s->xwl : NULL;
    if (w && w->toplevel && X.st && s->w > 0 && s->h > 0)
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
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

    /* a WL_SURFACE_ID message may have arrived BEFORE this record
     * existed — pair any stashed association now */
    for (size_t i = 0; i < X.n_pending_ids; i++) {
        if (X.pending_ids[i].win != e->window) continue;
        if (X.client && X.st) {
            struct wl_resource *r = wl_client_get_object(
                X.client, X.pending_ids[i].surface_id);
            _wl_surf_t *s = (r &&
                             wl_resource_instance_of(
                                 r, &wl_surface_interface, NULL))
                                ? wl_resource_get_user_data(r) : NULL;
            if (s && w->surf != s) _xwl_late_pair(w, s);
        }
        X.pending_ids[i] = X.pending_ids[X.n_pending_ids - 1];
        X.n_pending_ids--;
        break;
    }

    /* a WL_SURFACE_SERIAL message may have arrived BEFORE this record
     * existed (the X socket runs ahead of the wayland socket on real
     * hardware) — apply the stashed serial now, before the link below
     * lets _xwl_serial_reconcile() see the record */
    for (size_t i = 0; i < X.n_pending_serials; i++) {
        if (X.pending_serials[i].win != e->window) continue;
        w->win_serial = X.pending_serials[i].serial;
        w->have_win_serial = true;
        X.pending_serials[i] = X.pending_serials[X.n_pending_serials - 1];
        X.n_pending_serials--;
        vt_logd("xwayland: stashed WL_SURFACE_SERIAL applied to 0x%x",
                (unsigned)w->xwin);
        break;
    }

    /* LATE WINDOW: get_xwayland_surface may already have paired the
     * wl surface with a PLACEHOLDER record (xwin == 0) — the wayland
     * request was read before this CreateNotify crossed the X socket.
     * Without adoption the surface keeps the placeholder forever: it
     * renders at +0+0 (title bar BEHIND the top panel — "apps open
     * with the bar above the panel"), never learns its geometry, and
     * the WM model shows a 1x1 window at 0,0. Adopt the placeholder's
     * surface + resource into this real record — but ONLY when the
     * serials agree (or no serial is known on either side yet): with
     * racing windows a recency guess swaps surfaces between windows.
     * When serials are pending, _xwl_serial_reconcile() does the
     * exact pairing as soon as they arrive. */
    if (X.st) {
        for (_xwl_win_t *ph = X.placeholders; ph; ph = ph->next) {
            _wl_surf_t *s = ph->surf;
            if (!s || !ph->xwl_res)
                continue;   /* not an unadopted pairing placeholder */
            if (ph->have_surf_serial)
                continue;   /* serial known: wait for the exact match */
            w->surf = s;
            w->xwl_res = ph->xwl_res;
            wl_resource_set_user_data(w->xwl_res, w);
            s->xwl = w;
            ph->surf = NULL;
            ph->xwl_res = NULL;
            _xwl_placeholder_free(ph);
            w->waiting_surface = false;
            vt_logd("xwayland: late X window 0x%x adopted an already-"
                    "paired surface", (unsigned)w->xwin);
            break;
        }
    }

    w->next = X.wins;
    w->prev = NULL;
    if (X.wins) X.wins->prev = w;
    X.wins = w;
    vt_logd("xwayland: X window 0x%x created %dx%d +%d+%d (OR=%d)",
            (unsigned)w->xwin, w->w, w->h, w->x, w->y,
            (int)w->override_redirect);
    /* a WL_SURFACE_SERIAL message may have arrived before this
     * CreateNotify crossed the X socket — pair by serial now */
    _xwl_serial_reconcile();
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
    {
        int wx, wy, ww, wh;
        _layer_workarea(st, &wx, &wy, &ww, &wh);
        if (w->x == 0 && w->y == 0) {
            w->x = wx + (ww - w->w) / 2;
            w->y = wy + (wh - w->h) / 2;
        }
        /* X11 apps position THEMSELVES (USPosition, session restore,
         * -geometry) — y=0 is extremely common there. Under a top
         * panel that put the title bar BEHIND the panel with nothing
         * left to grab ("apps open with the bar above the panel").
         * Clamp any requested position into the usable workarea —
         * clamping the FRAME: the SSD title band sits ABOVE the X
         * window, so the client must clear the workarea top by the
         * full title height or the grab bar hides behind the panel. */
        int tbar = (!w->motif_csd && !w->override_redirect)
                       ? (_WL_SSD_TITLE + _WL_SSD_BORDER) : 0;
        if (w->y < wy + tbar) w->y = wy + tbar;
        if (w->x < wx) w->x = wx;
        if (w->x + w->w > wx + ww && wx + ww > w->w)
            w->x = wx + ww - w->w;
        if (w->y + w->h > wy + wh && wy + wh > w->h)
            w->y = wy + wh - w->h;
        if (w->x < wx) w->x = wx;
        if (w->y < wy + tbar) w->y = wy + tbar;
    }
    _xwl_configure(w, w->x, w->y, w->w, w->h);
    xcb_map_window(X.xc, w->xwin);
    if (w->surf) {
        w->surf->ssd = !w->motif_csd;
        w->surf->x = w->x;
        w->surf->y = w->y;
    }
    if (!w->toplevel) {
        /* _toplevel_new strdups title AND app_id from the class — the
         * old code overwrote app_id with a SECOND strdup here and
         * leaked the first (2 B per X11 window under LSan) */
        w->toplevel = _toplevel_new(st, w->surf, w->title,
                                    w->class ? w->class : "");
        if (w->toplevel) {
            _wl_surf_t *s = w->surf;
            if (s) {
                s->x = w->x;
                s->y = w->y;
                s->ws = st->ws_cur;
            }
            /* geometry mirror + focus state: the WIN_MAP event fires
             * BEFORE any surface exists — without the mirror the WM
             * model sees a 0x0 window at +0+0 ("tiny window in the
             * corner" in the pager); without activated the taskbar
             * never shows the new window as focused. */
            w->toplevel->x = w->x;
            w->toplevel->y = w->y;
            w->toplevel->w = w->w;
            w->toplevel->h = w->h;
            w->toplevel->activated = true;
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
    /* _xwl_free_win destroys the pairing resource (guard-pre-empted
     * destructor) and unlinks + frees the record */
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
    /* honor client requests but keep windows on screen — and the grab
     * bar reachable: same frame clamp as _xwl_map (an app that moves
     * itself to y=0 mid-session must not strand its title bar behind
     * the top panel) */
    if (st) {
        int wx, wy, ww, wh;
        _layer_workarea(st, &wx, &wy, &ww, &wh);
        int tbar = (!w->motif_csd && !w->override_redirect)
                       ? (_WL_SSD_TITLE + _WL_SSD_BORDER) : 0;
        if (nx < 0) nx = 0;
        if (ny < wy + tbar) ny = wy + tbar;
        if (nx + nw > st->out_w) nx = st->out_w - nw;
        if (ny + nh > st->out_h) ny = st->out_h - nh;
        if (ny < wy + tbar) ny = wy + tbar;
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

    vt_logd("xwayland: client msg win=0x%x type=0x%x "
            "l=[%u,%u,%u,%u,%u]", (unsigned)e->window, (unsigned)e->type,
            e->data.data32[0], e->data.data32[1], e->data.data32[2],
            e->data.data32[3], e->data.data32[4]);

    if (e->type == X.a_wl_surface_serial && st) {
        /* xwayland_shell_v1's X-side association: the SAME unique
         * serial the wl_surface announced via set_serial, delivered on
         * the X window (l[0] = lo bits, l[1] = hi bits). This is what
         * makes the window↔surface pairing EXACT — see
         * _xwl_serial_reconcile(). */
        _xwl_win_t *w = _xwl_find(e->window);
        if (w) {
            w->win_serial = ((uint64_t)e->data.data32[1] << 32) |
                            (uint64_t)e->data.data32[0];
            w->have_win_serial = true;
            _xwl_serial_reconcile();
        } else {
            /* the CreateNotify is still in flight on this same socket
             * (both events are ordered server-side, but the poll loop
             * can hand us the message in the same batch it reads the
             * create — no: events are queued in order, yet a
             * different client's flood may interleave; stash instead
             * of dropping: a dropped association strands the surface
             * at +0+0 forever) */
            if (X.n_pending_serials < 16) {
                X.pending_serials[X.n_pending_serials].win = e->window;
                X.pending_serials[X.n_pending_serials].serial =
                    ((uint64_t)e->data.data32[1] << 32) |
                    (uint64_t)e->data.data32[0];
                X.n_pending_serials++;
                vt_logd("xwayland: WL_SURFACE_SERIAL stashed for late "
                        "window 0x%x", (unsigned)e->window);
            }
        }
        return;
    }
    if (e->type == X.a_wl_surface_id && st && X.client) {
        /* LEGACY association: Xwayland announces the wl_surface
         * resource id that belongs to an X window (the pre-shell
         * protocol — still what many Xwayland builds actually use;
         * Debian's 24.1.6 binds xwayland_shell_v1 yet sends these).
         * Without this the surface never pairs with the window: it
         * renders at +0+0 (title bar BEHIND the top panel — "apps
         * open with the bar above the panel") and the WM model shows
         * a 1x1 window at 0,0. */
        uint32_t sid = (uint32_t)e->data.data32[0];
        struct wl_resource *r =
            wl_client_get_object(X.client, sid);
        _wl_surf_t *s = (r &&
                         wl_resource_instance_of(
                             r, &wl_surface_interface, NULL))
                            ? wl_resource_get_user_data(r) : NULL;
        _xwl_win_t *w = _xwl_find(e->window);
        if (w && s && w->surf != s) {
            _xwl_late_pair(w, s);
        } else if (!s) {
            /* the wl_surface resource does not exist compositor-side
             * YET: Xwayland sends this message right after creating
             * the surface, and the X socket can be read before the
             * wayland socket delivers create_surface. Stash (both
             * the window-record-missing and the surface-missing
             * cases) and retry when the missing side appears — the
             * old code silently DROPPED the w-exists/s-missing case
             * and that window's surface never paired. */
            if (X.n_pending_ids < 16) {
                X.pending_ids[X.n_pending_ids].win = e->window;
                X.pending_ids[X.n_pending_ids].surface_id = sid;
                X.n_pending_ids++;
                vt_logd("xwayland: WL_SURFACE_ID stashed (surface or "
                        "window not yet known) for 0x%x",
                        (unsigned)e->window);
            }
        } else if (!w && s) {
            /* surface id arrived before the CreateNotify: stash and
             * pair when the window record exists */
            if (X.n_pending_ids < 16) {
                X.pending_ids[X.n_pending_ids].win = e->window;
                X.pending_ids[X.n_pending_ids].surface_id = sid;
                X.n_pending_ids++;
            }
        }
        return;
    }
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

void _xwl_learn_client(struct wl_client *cli) {
    if (!cli || !X.started || X.client) return;
    /* wl_client_get_credentials: pid is valid for local (unix socket)
     * clients — Xwayland always is */
    pid_t cpid = 0;
    uid_t cuid = 0;
    gid_t cgid = 0;
    wl_client_get_credentials(cli, &cpid, &cuid, &cgid);
    if (X.pid > 0 && cpid == X.pid) {
        X.client = cli;
        vt_logd("xwayland: client identified by pid %d (surface path)",
                (int)cpid);
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
    } else if (w->xwin == 0) {
        /* a placeholder that never met its X window dies with the
         * surface (it is in X.placeholders, not X.wins; leaving it
         * leaks the record and its pairing resource outlives it).
         * Destroy the resource FIRST — its destructor reads the
         * user_data record, which must still be alive (the
         * xwl_res==res guard makes the sever a no-op). */
        s->xwl = NULL;
        if (w->xwl_res) {
            struct wl_resource *r = w->xwl_res;
            w->xwl_res = NULL;
            wl_resource_destroy(r);
        }
        _xwl_placeholder_free(w);
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
        /* the FRAME must fill the workarea (not the client): our SSD
         * title band sits ABOVE the X window, so a client at the bare
         * workarea origin parks its grab bar behind the top panel and
         * the maximized window "goes over the panel". Inset the client
         * by the SSD extents for framed windows. */
        if (w->toplevel) {
            w->toplevel->prev_x = w->x;
            w->toplevel->prev_y = w->y;
            w->toplevel->prev_w = w->w;
            w->toplevel->prev_h = w->h;
        }
        bool ssd = !w->motif_csd && !w->override_redirect;
        int tbar = ssd ? (_WL_SSD_TITLE + _WL_SSD_BORDER) : 0;
        int brd = ssd ? _WL_SSD_BORDER : 0;
        _xwl_configure(w, wx + brd, wy + tbar,
                       ww - 2 * brd, wh - tbar - brd);
    } else if (w->toplevel && w->toplevel->prev_w > 0) {
        _xwl_configure(w, w->toplevel->prev_x, w->toplevel->prev_y,
                       w->toplevel->prev_w, w->toplevel->prev_h);
    } else {
        _xwl_configure(w, wx + (ww - w->w) / 2, wy + (wh - w->h) / 2,
                       w->w, w->h);
    }
    if (w->toplevel) {
        w->toplevel->maximized = on;
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE, w->toplevel);
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
    }
    X.st->dirty = true;
}

void _xwl_fullscreen(_wl_surf_t *s, bool on) {
    _xwl_win_t *w = _xwl_find_by_surf(s);
    if (!w || !w->mapped || !X.st) return;
    if (on) {
        if (w->toplevel) {
            w->toplevel->prev_x = w->x;
            w->toplevel->prev_y = w->y;
            w->toplevel->prev_w = w->w;
            w->toplevel->prev_h = w->h;
        }
        _xwl_configure(w, 0, 0, X.st->out_w, X.st->out_h);
    } else if (w->toplevel && w->toplevel->prev_w > 0) {
        _xwl_configure(w, w->toplevel->prev_x, w->toplevel->prev_y,
                       w->toplevel->prev_w, w->toplevel->prev_h);
    } else {
        int wx, wy, ww, wh;
        _layer_workarea(X.st, &wx, &wy, &ww, &wh);
        _xwl_configure(w, wx + (ww - w->w) / 2, wy + (wh - w->h) / 2,
                       w->w, w->h);
    }
    if (w->toplevel) {
        w->toplevel->fullscreen = on;
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_STATE, w->toplevel);
        _emit_win(X.st, VT_BACKEND_WL_EVENT_WIN_GEOMETRY, w->toplevel);
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
        int wst = 0;
        if (waitpid(pid, &wst, WNOHANG) != 0) {
            if (WIFEXITED(wst) && WEXITSTATUS(wst) == 127) {
                vt_logw("xwayland: executable not found in PATH "
                        "(execvp failed) — X11 apps stay unavailable. "
                        "Install Xwayland (xorg-xwayland / "
                        "xwayland / xserver-xorg-xwayland)");
            } else if (WIFSIGNALED(wst)) {
                vt_logw("xwayland: killed by signal %d before "
                        "reporting a display number",
                        WTERMSIG(wst));
            } else {
                vt_logw("xwayland: process exited (status %d) before "
                        "reporting a display number",
                        WIFEXITED(wst) ? WEXITSTATUS(wst) : wst);
            }
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
    X.a_wl_surface_id = _atom(xc, "WL_SURFACE_ID");
    X.a_wl_surface_serial = _atom(xc, "WL_SURFACE_SERIAL");

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

#if defined(VT_HAVE_XCB_COMPOSITE)
    /* 6b. THE visibility switch: redirect every root child MANUAL.
     * Xwayland only creates a wl_surface (and sends the association
     * message + buffers) for windows whose redirectDraw is MANUAL —
     * which happens for children of a Subwindows-redirected root.
     * Without this call X11 apps under the Wayland session render
     * NOTHING: Xwayland creates the window's shm pixmap but never
     * attaches it to any surface (verified by wire trace; weston and
     * wlroots issue the same request from their XWM). */
    xcb_composite_redirect_subwindows(xc, scr->root,
                                      XCB_COMPOSITE_REDIRECT_MANUAL);
#endif

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
