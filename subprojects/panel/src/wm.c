/*
 * wm.c — window/workspace model backed by the WM IPC
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#include "wm.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static gboolean _parse_query(vp_wm_t *wm, const char *payload) {
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
    g_ptr_array_free(wm->wins, TRUE);
    wm->wins = nw;
    return TRUE;
}

static gboolean _parse_ws(vp_wm_t *wm, const char *payload) {
    int count = 0, cur = 0;
    const char *p = payload;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char buf[128];
        if (llen >= sizeof(buf)) llen = sizeof(buf) - 1;
        memcpy(buf, p, llen);
        buf[llen] = 0;
        if (strncmp(buf, "count=", 6) == 0) count = atoi(buf + 6);
        else if (strncmp(buf, "current=", 8) == 0) cur = atoi(buf + 8);
        p = eol ? eol + 1 : NULL;
    }
    if (count < 1) count = 1;
    if (cur < 0 || cur >= count) cur = 0;
    if (count != wm->ws_count || cur != wm->ws_cur) {
        wm->ws_count = count;
        wm->ws_cur = cur;
        return TRUE;
    }
    return FALSE;
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
            changed = TRUE;
        }
        return changed;
    }
    /* drain pending events first (they explain what changed) */
    for (int i = 0; i < 64; i++) {
        uint32_t id = 0;
        char *ev = vp_ipc_poll_event(wm->ipc, &id);
        if (!ev) break;
        changed = TRUE;   /* any event → re-query below */
        free(ev);
    }
    char *q = vp_ipc_call(wm->ipc, VP_IPC_WM_QUERY, "", 1500);
    if (q) {
        _parse_query(wm, q);
        free(q);
        changed = TRUE;
        wm->misses = 0;
    } else if (++wm->misses >= 3) {
        /* three consecutive dead calls: the WM is gone (logout), not
         * merely busy — drop the connection and go back to retry mode
         * in case the session restarts it */
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
    if (vp_wm_refresh(wm) && wm->on_change)
        wm->on_change(wm->user);
    return G_SOURCE_CONTINUE;
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
    if (wm->have_ipc)
        vp_ipc_subscribe(wm->ipc);
    vp_wm_refresh(wm);
    wm->poll_id = g_timeout_add(400, _on_poll, wm);
    return wm;
}

void vp_wm_free(vp_wm_t *wm) {
    if (!wm) return;
    if (wm->poll_id) g_source_remove(wm->poll_id);
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
