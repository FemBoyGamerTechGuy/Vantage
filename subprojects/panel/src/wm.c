/*
 * wm.c — window/workspace model backed by the WM IPC
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
/* setenv() needs POSIX 2001 visibility */
#define _POSIX_C_SOURCE 200112L

#include "wm.h"
#include "ipc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* WM_QUERY line format (stable, documented in vantage-wm.c):
 *   id \t title \t ws \t flags \t class \t app_id \t x \t y \t w \t h
 * flags: F(focused) M(inimized) X(maximized) S(fullscreen) U(rgent) */
static void _free_win(gpointer p) {
    vp_win_t *w = p;
    if (!w) return;
    g_free(w->title);
    g_free(w->app_id);
    g_free(w->cls);
    g_free(w);
}

/* parse a WM_QUERY payload into a FRESH window array (does not touch
 * wm->wins — the caller swaps after comparing) */
static GPtrArray *_parse_query_new(const char *payload) {
    GPtrArray *nw = g_ptr_array_new_with_free_func(_free_win);
    const char *line = payload;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        char buf[2048];
        if (llen >= sizeof(buf)) llen = sizeof(buf) - 1;
        memcpy(buf, line, llen);
        buf[llen] = 0;
        /* Split on tabs WITHOUT collapsing runs: strtok_r treats
         * consecutive tabs as one delimiter, so a window with NO
         * flags (the common case — no F/M/X/S) produced an empty
         * flags column that was silently skipped, shifting every
         * later column left and DROPPING the window (it looked like
         * 9 columns). g_strsplit keeps empty fields in place. */
        gchar **f = g_strsplit(buf, "\t", 0);
        guint nf = g_strv_length(f);
        vp_win_t *w = NULL;
        if (nf >= 10 && f[0] && *f[0]) {
            unsigned long id = strtoul(f[0], NULL, 10);
            if (id != 0) {
                w = g_new0(vp_win_t, 1);
                w->id = (guint32)id;
                w->title = g_strdup(f[1] ? f[1] : "");
                w->ws = (gint)strtol(f[2] ? f[2] : "0", NULL, 10);
                if (f[3]) {
                    if (strchr(f[3], 'F')) w->focused = TRUE;
                    if (strchr(f[3], 'M')) w->minimized = TRUE;
                    if (strchr(f[3], 'X')) w->maximized = TRUE;
                    if (strchr(f[3], 'S')) w->fullscreen = TRUE;
                    if (strchr(f[3], 'D')) w->dock = TRUE;
                    if (strchr(f[3], 'K')) w->desktop = TRUE;
                }
                w->cls = g_strdup(f[4] ? f[4] : "");
                w->app_id = g_strdup(f[5] ? f[5] : "");
                w->x = (gint)strtol(f[6] ? f[6] : "0", NULL, 10);
                w->y = (gint)strtol(f[7] ? f[7] : "0", NULL, 10);
                w->w = (gint)strtol(f[8] ? f[8] : "0", NULL, 10);
                w->h = (gint)strtol(f[9] ? f[9] : "0", NULL, 10);
                g_ptr_array_add(nw, w);
            }
        }
        g_strfreev(f);
        line = eol ? eol + 1 : NULL;
    }
    return nw;
}

/* Did the window model ACTUALLY change? The 400 ms poll used to
 * report "changed" whenever WM_QUERY succeeded — even when every
 * field was identical. The panel then rebuilt the whole taskbar every
 * cycle: hovered taskbar buttons lost their hover state 2.5x/second
 * (the reported active/inactive FLICKER), and the rebuild DESTROYED
 * the button a popover menu was parented to, closing the dropdown
 * before a menu item could be clicked. Only a REAL difference may
 * count as a change. */
static gboolean _wins_equal(const vp_wm_t *a, const vp_wm_t *b) {
    if (a->wins->len != b->wins->len) return FALSE;
    for (guint i = 0; i < a->wins->len; i++) {
        const vp_win_t *x = g_ptr_array_index(a->wins, i);
        const vp_win_t *y = g_ptr_array_index(b->wins, i);
        if (x->id != y->id || x->ws != y->ws ||
            x->focused != y->focused || x->minimized != y->minimized ||
            x->maximized != y->maximized || x->fullscreen != y->fullscreen ||
            x->dock != y->dock || x->desktop != y->desktop ||
            x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h)
            return FALSE;
        if (g_strcmp0(x->title, y->title) != 0 ||
            g_strcmp0(x->cls, y->cls) != 0 ||
            g_strcmp0(x->app_id, y->app_id) != 0)
            return FALSE;
    }
    return TRUE;
}

static gboolean _parse_ws(vp_wm_t *wm, const char *payload) {
    int count = 0, cur = 0;
    bool have_count = false;
    const char *p = payload;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char buf[128];
        if (llen >= sizeof(buf)) llen = sizeof(buf) - 1;
        memcpy(buf, p, llen);
        buf[llen] = 0;
        if (strncmp(buf, "count=", 6) == 0) {
            count = atoi(buf + 6);
            have_count = true;
        }
        else if (strncmp(buf, "current=", 8) == 0) cur = atoi(buf + 8);
        p = eol ? eol + 1 : NULL;
    }
    /* A response WITHOUT a real count= field is NOT workspace data —
     * it is a frame misalignment (a stale WM_QUERY window list read
     * as a WS_QUERY answer after a timed-out call). Clamping that to
     * 1 collapsed the pager to a single cell mid-drag until the next
     * poll restored 4 ("bugs out the pager ... shows one pager").
     * Ignore it entirely; the next poll resynchronizes. */
    if (!have_count || count < 1) return FALSE;
    if (cur < 0 || cur >= count) cur = 0;
    if (count != wm->ws_count || cur != wm->ws_cur) {
        wm->ws_count = count;
        wm->ws_cur = cur;
        return TRUE;
    }
    return FALSE;
}

/* event-driven model updates: geometry events carry the numbers, so
 * the model updates in place (no WM_QUERY round-trip) and the pager
 * redraws at the event rate. Returns TRUE when a geometry event was
 * applied. */
static gboolean _apply_geometry_event(vp_wm_t *wm, const char *payload) {
    /* "window-geometry id=%u x=%d y=%d w=%d h=%d ws=%d" */
    if (strncmp(payload, "window-geometry ", 16) != 0) return FALSE;
    guint32 id = 0;
    gint x = 0, y = 0, w = 0, h = 0, ws = 0;
    const char *p = payload + 16;
    while (*p) {
        if (sscanf(p, "id=%u", &id) == 1) {}
        else if (sscanf(p, "x=%d", &x) == 1) {}
        else if (sscanf(p, "y=%d", &y) == 1) {}
        else if (sscanf(p, "w=%d", &w) == 1) {}
        else if (sscanf(p, "h=%d", &h) == 1) {}
        else if (sscanf(p, "ws=%d", &ws) == 1) {}
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    if (!id) return FALSE;
    for (guint i = 0; i < wm->wins->len; i++) {
        vp_win_t *win = g_ptr_array_index(wm->wins, i);
        if (win->id != id) continue;
        win->x = x; win->y = y;
        if (w > 0) win->w = w;
        if (h > 0) win->h = h;
        win->ws = ws;
        return TRUE;
    }
    return FALSE;   /* unknown window: full refresh will adopt it */
}

/* Drain event socket ONLY: geometry events update the model in place
 * and redraw the pager at the event rate (the fd watch calls this the
 * moment the WM writes); any OTHER event merely schedules one
 * coalesced full refresh — a drag at 30 fps must not cost 30
 * window-list round-trips per second. */
static void _schedule_full_refresh(vp_wm_t *wm);
static void _attach_event_watch(vp_wm_t *wm);
static void _detach_event_watch(vp_wm_t *wm);

static void _drain_events(vp_wm_t *wm) {
    if (!wm->have_ipc) return;
    gboolean geo = FALSE, other = FALSE;
    for (int i = 0; i < 4096; i++) {
        uint32_t id = 0;
        char *ev = vp_ipc_poll_event(wm->ipc, &id);
        if (!ev) break;
        if (_apply_geometry_event(wm, ev))
            geo = TRUE;
        else
            other = TRUE;
        free(ev);
    }
    if (geo && wm->on_geometry)
        wm->on_geometry(wm->geo_user);
    if (other)
        _schedule_full_refresh(wm);
}

gboolean vp_wm_refresh(vp_wm_t *wm) {
    gboolean changed = FALSE;
    if (!wm->have_ipc) {
        /* the panel starts BEFORE the WM has created its socket (the
         * session launches them back to back) — keep retrying until
         * it appears instead of staying blind forever */
        wm->ipc = vp_ipc_connect(NULL);
        if (wm->ipc) {
            wm->have_ipc = TRUE;
            vp_ipc_subscribe(wm->ipc);
            _attach_event_watch(wm);
            changed = TRUE;
        }
        return changed;
    }
    /* pending events first — they explain what changed (and geometry
     * events already landed in the model above via the fd watch) */
    _drain_events(wm);
    char *q = vp_ipc_call(wm->ipc, VP_IPC_WM_QUERY, "", 1500);
    if (q) {
        /* parse into a fresh array, COMPARE with the current model,
         * then swap — a successful query alone is NOT a change (see
         * _wins_equal: the no-op rebuild was the taskbar flicker and
         * the self-closing dropdown menu) */
        GPtrArray *nw = _parse_query_new(q);
        free(q);
        if (nw) {
            vp_wm_t old = *wm;      /* shallow copy: wins points at the OLD array */
            wm->wins = nw;
            changed = !_wins_equal(&old, wm);
            g_ptr_array_free(old.wins, TRUE);
        }
        wm->misses = 0;
    } else if (++wm->misses >= 3) {
        /* three consecutive dead calls: the WM is gone (logout), not
         * merely busy — drop the connection and go back to retry mode
         * in case the session restarts it */
        _detach_event_watch(wm);
        vp_ipc_free(wm->ipc);
        wm->ipc = NULL;
        wm->have_ipc = FALSE;
        wm->misses = 0;
        return FALSE;
    }
    char *wq = vp_ipc_call(wm->ipc, VP_IPC_WM_WS_QUERY, "", 1500);
    if (wq) {
        changed |= _parse_ws(wm, wq);
        free(wq);
    }
    return changed;
}

static gboolean _on_poll(gpointer user) {
    vp_wm_t *wm = user;
    /* VANTAGE_PANEL_NO_POLL=1: diagnostic switch that freezes the WM
     * model (isolates panel-side repaints from backend event storms
     * when debugging taskbar/pager behavior) */
    if (getenv("VANTAGE_PANEL_NO_POLL"))
        return G_SOURCE_CONTINUE;
    if (vp_wm_refresh(wm) && wm->on_change)
        wm->on_change(wm->user);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------- event socket fd watch */

typedef struct {
    GSource src;
    GPollFD fd;
} _wm_watch_t;

static gboolean _watch_prepare(GSource *src, gint *timeout) {
    (void)src;
    *timeout = -1;
    return FALSE;
}
static gboolean _watch_check(GSource *src) {
    _wm_watch_t *w = (_wm_watch_t *)src;
    return (w->fd.revents & (G_IO_IN | G_IO_HUP)) != 0;
}
static gboolean _watch_dispatch(GSource *src, GSourceFunc cb, gpointer ud) {
    vp_wm_t *wm = ud;
    (void)cb;
    _wm_watch_t *w = (_wm_watch_t *)src;
    if (w->fd.revents & (G_IO_HUP | G_IO_ERR)) {
        /* the WM died (logout): drop the watch so the main loop does
         * not spin on a permanent HUP; the poll timer's retry path
         * rebuilds the connection if a session restarts the WM */
        _detach_event_watch(wm);
        return G_SOURCE_REMOVE;
    }
    _drain_events(wm);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs _watch_funcs = {
    _watch_prepare, _watch_check, _watch_dispatch, NULL, NULL, NULL
};

static void _attach_event_watch(vp_wm_t *wm) {
    if (wm->watch_attached || !wm->have_ipc || !wm->ipc) return;
    _wm_watch_t *w = (_wm_watch_t *)g_source_new(
        (GSourceFuncs *)&_watch_funcs, sizeof(_wm_watch_t));
    w->fd.fd = vp_ipc_fd(wm->ipc);
    w->fd.events = G_IO_IN | G_IO_HUP;
    w->fd.revents = 0;
    g_source_add_poll((GSource *)w, &w->fd);
    g_source_set_callback((GSource *)w, NULL, wm, NULL);
    g_source_attach((GSource *)w, NULL);   /* default main context */
    wm->watch_id = (gpointer)(GSource *)w;
    wm->watch_attached = 1;
}

static void _detach_event_watch(vp_wm_t *wm) {
    if (!wm->watch_attached || !wm->watch_id) return;
    g_source_destroy((GSource *)wm->watch_id);
    g_source_unref((GSource *)wm->watch_id);
    wm->watch_id = NULL;
    wm->watch_attached = 0;
}

void vp_wm_set_geometry_cb(vp_wm_t *wm, vp_wm_geometry_cb cb,
                            gpointer user) {
    if (!wm) return;
    wm->on_geometry = cb;
    wm->geo_user = user;
}

/* coalesced full refresh: one WM_QUERY per burst of structural
 * events, never one per event */
static gboolean _on_coalesced_refresh(gpointer user) {
    vp_wm_t *wm = user;
    wm->coalesce_id = 0;
    if (vp_wm_refresh(wm) && wm->on_change)
        wm->on_change(wm->user);
    return G_SOURCE_REMOVE;
}

static void _schedule_full_refresh(vp_wm_t *wm) {
    if (!wm->coalesce_id)
        wm->coalesce_id = g_timeout_add(60, _on_coalesced_refresh, wm);
}

/* --------------------------------------------- XWayland env */

gboolean vp_wm_xwl_env_apply(vp_wm_t *wm) {
    if (!wm || !wm->have_ipc || !wm->ipc) return FALSE;
    /* cheap guard: once a display is exported AND confirmed working we
     * never ask again; until then every launch re-checks (Xwayland may
     * still be starting when the first app is launched) */
    if (wm->xwl_confirmed) return TRUE;
    char *r = vp_ipc_call(wm->ipc, VP_IPC_WM_XWL_ENV, "", 800);
    if (!r) return FALSE;
    char disp[64] = "", auth[512] = "";
    for (const char *line = r; line && *line;) {
        const char *eol = strchr(line, '\n');
        size_t n = eol ? (size_t)(eol - line) : strlen(line);
        char buf[512];
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        if (!strncmp(buf, "display=", 8) && buf[8])
            snprintf(disp, sizeof(disp), "%s", buf + 8);
        else if (!strncmp(buf, "xauthority=", 11) && buf[11])
            snprintf(auth, sizeof(auth), "%s", buf + 11);
        line = eol ? eol + 1 : NULL;
    }
    free(r);
    if (!disp[0]) return FALSE;
    setenv("DISPLAY", disp, 1);
    if (auth[0]) setenv("XAUTHORITY", auth, 1);
    wm->xwl_confirmed = 1;
    g_debug("launcher: Xwayland env applied: DISPLAY=%s", disp);
    return TRUE;
}

vp_wm_t *vp_wm_new(vp_wm_changed_cb cb, gpointer user) {
    vp_wm_t *wm = g_new0(vp_wm_t, 1);
    wm->wins = g_ptr_array_new_with_free_func(_free_win);
    wm->ws_count = 4;
    wm->ws_cur = 0;
    wm->on_change = cb;
    wm->user = user;
    wm->ipc = vp_ipc_connect(NULL);
    wm->have_ipc = wm->ipc != NULL;
    if (wm->have_ipc) {
        vp_ipc_subscribe(wm->ipc);
        _attach_event_watch(wm);
    }
    vp_wm_refresh(wm);
    wm->poll_id = g_timeout_add(400, _on_poll, wm);
    return wm;
}

void vp_wm_free(vp_wm_t *wm) {
    if (!wm) return;
    if (wm->poll_id) g_source_remove(wm->poll_id);
    if (wm->coalesce_id) g_source_remove(wm->coalesce_id);
    _detach_event_watch(wm);
    if (wm->ipc) vp_ipc_free(wm->ipc);
    g_ptr_array_free(wm->wins, TRUE);
    g_free(wm);
}

static void _act(vp_wm_t *wm, uint32_t msg, const char *payload) {
    if (!wm->have_ipc) return;
    char *r = vp_ipc_call(wm->ipc, msg, payload ? payload : "", 800);
    free(r);
    vp_wm_refresh(wm);
    if (wm->on_change) wm->on_change(wm->user);
}

void vp_wm_focus(vp_wm_t *wm, guint32 id) {
    char p[64];
    snprintf(p, sizeof(p), "id=%u", (unsigned)id);
    _act(wm, VP_IPC_WM_FOCUS, p);
}

void vp_wm_close(vp_wm_t *wm, guint32 id) {
    char p[64];
    snprintf(p, sizeof(p), "id=%u", (unsigned)id);
    _act(wm, VP_IPC_WM_CLOSE, p);
}

void vp_wm_minimize(vp_wm_t *wm, guint32 id) {
    char p[64];
    snprintf(p, sizeof(p), "id=%u", (unsigned)id);
    _act(wm, VP_IPC_WM_MINIMIZE, p);
}

void vp_wm_restore(vp_wm_t *wm, guint32 id) {
    char p[64];
    snprintf(p, sizeof(p), "id=%u", (unsigned)id);
    _act(wm, VP_IPC_WM_RESTORE, p);
}

void vp_wm_move_to_ws(vp_wm_t *wm, guint32 id, gint ws) {
    char p[64];
    snprintf(p, sizeof(p), "id=%u\nws=%d", (unsigned)id, ws);
    _act(wm, VP_IPC_WM_WS_MOVE, p);
}

void vp_wm_switch_ws(vp_wm_t *wm, gint ws) {
    char p[64];
    snprintf(p, sizeof(p), "ws=%d", ws);
    _act(wm, VP_IPC_WM_WS_SWITCH, p);
}

void vp_session_action(const char *action) {
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/vantage-session.sock", rd);
    vp_ipc_t *ipc = vp_ipc_connect(path);
    if (!ipc) {
        /* no session manager: fall back to asking the WM directly —
         * a standalone compositor run ends cleanly that way */
        ipc = vp_ipc_connect(NULL);
        if (!ipc) return;
    }
    char *r = vp_ipc_call(ipc, VP_IPC_WM_LOGOUT,
                          action ? action : "", 1500);
    free(r);
    vp_ipc_free(ipc);
}
