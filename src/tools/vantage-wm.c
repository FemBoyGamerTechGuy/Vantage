/*
 * vantage-wm.c — Vantage window manager + compositor server
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Runs the WM engine, the XRender composite manager (when available and
 * enabled), and the WM IPC server that panel/desktop/settings/CLI tools
 * talk to. Payload format:
 *
 *   WM_QUERY response — one line per window, tab-separated:
 *     <id>\t<title>\t<workspace>\t<flags>\t<class>\t<app-id>
 *     flags: combination of F(focused) M(inimized) X(maximized)
 *            S(fullscreen) U(urgent) D(dock) K(desktop)
 *   commands — single line, tab- or =-separated:
 *     id=<window-id>   ws=<n>   mode=<left|right|top|bottom|max|full>
 *     cmd=<command line to launch>
 */

#define VT_LOG_DOMAIN "wm"
#include <vantage/vt-core.h>
#include <vantage/vt-paths.h>
#include <vantage/vt-wm.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-compositor.h>
#include <vantage/vt-config.h>
#include <vantage/vt-ipc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void _print_usage(FILE *fp, const char *argv0) {
    fprintf(fp,
"Vantage %s — window manager / compositor server\n"
"\n"
"Usage:\n"
"  %s [OPTION]...\n"
"\n"
"Backend selection:\n"
"  --wayland        run as the native Wayland compositor\n"
"  --x11            manage the X server named by $DISPLAY\n"
"  (none)           automatic: X11 when $DISPLAY is set, else Wayland\n"
"\n"
"Options:\n"
"  --no-composite   disable the compositing manager\n"
"  -v, --verbose    verbose logging\n"
"  -h, --help       show this help and exit\n"
"  -V, --version    show version and exit\n",
            vt_paths_version(), argv0);
}

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

typedef struct {
    vt_wm_t      *wm;
    vt_compositor_t *comp;
    vt_ipc_t     *ipc;
} _ctx_t;

/* ---------------------------------------------------------- util */
static long _payload_long(const vt_ipc_msg_t *m, const char *key, long dflt) {
    if (!m || !m->payload || !key) return dflt;
    const char *p = (const char *)m->payload;
    size_t klen = strlen(key);
    for (const char *line = p; line && *line; ) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        if (llen > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char buf[32] = {0};
            size_t vlen = llen - klen - 1;
            if (vlen >= sizeof(buf)) vlen = sizeof(buf) - 1;
            memcpy(buf, line + klen + 1, vlen);
            long v;
            return vt_parse_int(buf, &v) ? v : dflt;
        }
        line = eol ? eol + 1 : NULL;
    }
    return dflt;
}

static const char *_payload_str(const vt_ipc_msg_t *m, const char *key) {
    static char out[512];
    if (!m || !m->payload || !key) return NULL;
    const char *p = (const char *)m->payload;
    size_t klen = strlen(key);
    for (const char *line = p; line && *line; ) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        if (llen > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
            size_t vlen = llen - klen - 1;
            if (vlen >= sizeof(out)) vlen = sizeof(out) - 1;
            memcpy(out, line + klen + 1, vlen);
            out[vlen] = 0;
            return out;
        }
        line = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------- events */
static void _broadcast_win(_ctx_t *ctx, vt_window_t *w, const char *evname) {
    if (!ctx->ipc || !w) return;
    char *line = vt_strprintf("window-%s id=%u title=%s", evname, w->id,
                              w->title ? w->title : "");
    vt_ipc_broadcast(ctx->ipc, VT_IPC_MSG_WM_EVENT, line,
                     (uint32_t)strlen(line));
    vt_free(line);
}

static void _on_win_event(vt_wm_t *wm, vt_window_t *w, vt_wm_event_t ev) {
    _ctx_t *ctx = wm->hooks_ud;
    if (!ctx) return;
    switch (ev) {
    case VT_WM_EVENT_OPEN:          _broadcast_win(ctx, w, "opened"); break;
    case VT_WM_EVENT_CLOSE:         _broadcast_win(ctx, w, "closed"); break;
    case VT_WM_EVENT_CLOSE_REQUEST: break;
    case VT_WM_EVENT_FOCUS:         _broadcast_win(ctx, w, "focused"); break;
    case VT_WM_EVENT_TITLE:         _broadcast_win(ctx, w, "title"); break;
    case VT_WM_EVENT_STATE:         _broadcast_win(ctx, w, "state"); break;
    case VT_WM_EVENT_GEOMETRY:      _broadcast_win(ctx, w, "geometry"); break;
    default: break;
    }
}

static void _on_desktop(vt_wm_t *wm, int d) {
    _ctx_t *ctx = wm->hooks_ud;
    if (!ctx) return;
    char *line = vt_strprintf("workspace-changed %d", d);
    vt_ipc_broadcast(ctx->ipc, VT_IPC_MSG_WM_WS_EVENT, line,
                     (uint32_t)strlen(line));
    vt_free(line);
}

/* ------------------------------------------------ wayland event sink */

/* Mirror xdg_shell windows from the Wayland backend into the WM model so
 * vantage-remote list / focus / close work identically on both backends. */
static vt_window_t *_wl_mirror_new(vt_wm_t *wm,
                                   const vt_backend_wl_event_t *ev) {
    vt_window_t *w = vt_malloc0(sizeof(*w));
    w->id = (uint32_t)ev->window_id;
    w->title = vt_strdup(ev->title ? ev->title : "");
    w->app_id = vt_strdup(ev->app_id ? ev->app_id : "");
    w->class_str = vt_strdup("wayland");
    w->x = ev->x; w->y = ev->y;
    w->w = ev->w > 0 ? ev->w : 1;
    w->h = ev->h > 0 ? ev->h : 1;
    w->workspace = vt_wm_workspace_current(wm);
    w->focused = ev->focused;
    w->maximized = ev->maximized;
    w->fullscreen = ev->fullscreen;
    vt_vec_push(&wm->windows, &w);
    return w;
}

static vt_window_t *_wl_mirror_find(vt_wm_t *wm, uint64_t id) {
    return vt_wm_lookup(wm, (uint32_t)id);
}

static void _wl_event_sink(void *ud, void *event) {
    _ctx_t *ctx = ud;
    vt_wm_t *wm = ctx->wm;
    const vt_backend_wl_event_t *ev = event;
    if (!wm || !ev) return;
    switch (ev->kind) {
    case VT_BACKEND_WL_EVENT_WIN_MAP: {
        vt_window_t *w = _wl_mirror_new(wm, ev);
        vt_logi("wm: wayland window %u '%s' mapped", w->id,
                w->title ? w->title : "");
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_OPEN);
        break;
    }
    case VT_BACKEND_WL_EVENT_WIN_UNMAP: {
        vt_window_t *w = _wl_mirror_find(wm, ev->window_id);
        if (!w) break;
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_CLOSE);
        vt_wm_remove_window(wm, w);
        vt_free(w->title); vt_free(w->app_id);
        vt_free(w->class_str); vt_free(w);
        break;
    }
    case VT_BACKEND_WL_EVENT_WIN_TITLE: {
        vt_window_t *w = _wl_mirror_find(wm, ev->window_id);
        if (!w) break;
        vt_free(w->title);
        w->title = vt_strdup(ev->title ? ev->title : "");
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_TITLE);
        break;
    }
    case VT_BACKEND_WL_EVENT_WIN_STATE: {
        vt_window_t *w = _wl_mirror_find(wm, ev->window_id);
        if (!w) break;
        w->maximized = ev->maximized;
        w->fullscreen = ev->fullscreen;
        w->minimized = ev->minimized;
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_STATE);
        break;
    }
    case VT_BACKEND_WL_EVENT_WIN_GEOMETRY: {
        vt_window_t *w = _wl_mirror_find(wm, ev->window_id);
        if (!w) break;
        w->x = ev->x; w->y = ev->y;
        if (ev->w > 0) w->w = ev->w;
        if (ev->h > 0) w->h = ev->h;
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_GEOMETRY);
        break;
    }
    case VT_BACKEND_WL_EVENT_WIN_FOCUS: {
        vt_window_t *w = _wl_mirror_find(wm, ev->window_id);
        if (!w || w->focused) break;
        for (size_t i = 0; i < wm->windows.size; i++) {
            vt_window_t *p = *(vt_window_t **)vt_vec_at(&wm->windows, i);
            if (p) p->focused = (p == w);
        }
        if (wm->on_window_event)
            wm->on_window_event(wm, w, VT_WM_EVENT_FOCUS);
        break;
    }
    case VT_BACKEND_WL_EVENT_WORKSPACE: {
        /* compositor panel switched workspaces: sync the WM model and
         * tell subscribers exactly like an X11 desktop switch does */
        int d = (int)ev->window_id;
        if (d >= 0 && d < (int)wm->workspaces.size) {
            wm->cur_ws = d;
            if (wm->on_desktop_changed)
                wm->on_desktop_changed(wm, d);
            vt_logi("wm: wayland desktop -> %d (compositor panel)", d + 1);
        }
        break;
    }
    default:
        break;
    }
}

/* compositor hotkey hook: consult the WM shortcut table. The WM pointer
 * travels through the backend's user_data (set after vt_wm_new). */
static bool _wl_hotkey(vt_backend_t *backend, const char *combo) {
    if (!backend || !backend->get_user_data) return false;
    vt_wm_t *wm = backend->get_user_data(backend);
    if (!wm) return false;
    return vt_wm_shortcut_handle(wm, combo);
}

/* ---------------------------------------------------------- handlers */
static int _h_wm_query(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                       vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req;
    _ctx_t *ctx = ud;
    vt_wm_t *wm = ctx->wm;
    vt_strbuilder_t sb;
    vt_strbuilder_init(&sb, 4096);
    size_t out_len = 0;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (!w) continue;
        char flags[8] = {0};
        int fi = 0;
        if (w->focused) flags[fi++] = 'F';
        if (w->minimized) flags[fi++] = 'M';
        if (w->maximized) flags[fi++] = 'X';
        if (w->fullscreen) flags[fi++] = 'S';
        if (w->urgent) flags[fi++] = 'U';
        vt_strbuilder_appendf(&sb, "%u\t%s\t%d\t%s\t%s\t%s\n",
                              w->id,
                              w->title ? w->title : "",
                              w->workspace,
                              flags,
                              w->class_str ? w->class_str : "",
                              w->app_id ? w->app_id : "");
    }
    resp->payload = (uint8_t *)vt_strbuilder_finish(&sb, &out_len);
    resp->len = (uint32_t)out_len;
    return 0;
}

static int _h_wm_ws_query(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                           vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req;
    _ctx_t *ctx = ud;
    char *s = vt_strprintf("count=%d\ncurrent=%d",
                           vt_wm_workspace_count(ctx->wm),
                           vt_wm_workspace_current(ctx->wm));
    resp->payload = (uint8_t *)s;
    resp->len = (uint32_t)strlen(s);
    return 0;
}

static vt_window_t *_win_from_payload(_ctx_t *ctx, const vt_ipc_msg_t *req) {
    long id = _payload_long(req, "id", 0);
    if (id == 0) {
        const char *ids = _payload_str(req, "win");
        if (ids) id = atol(ids);
    }
    return id ? vt_wm_lookup(ctx->wm, (uint32_t)id) : NULL;
}

#define H_DEFINE(name, expr) \
    static int name(vt_ipc_t *ipc, const vt_ipc_msg_t *req, \
                    vt_ipc_msg_t *resp, void *ud) { \
        (void)ipc; (void)resp; \
        _ctx_t *ctx = ud; \
        vt_window_t *w = _win_from_payload(ctx, req); \
        if (!w) return -1; \
        expr; \
        return 0; \
    }

H_DEFINE(_h_focus,   vt_wm_focus(ctx->wm, w))
H_DEFINE(_h_close,   vt_wm_close(ctx->wm, w))
H_DEFINE(_h_minimize, vt_wm_minimize(ctx->wm, w, true))
H_DEFINE(_h_restore, vt_wm_minimize(ctx->wm, w, false))
H_DEFINE(_h_maximize, vt_wm_maximize(ctx->wm, w, true))
H_DEFINE(_h_unmaximize, vt_wm_maximize(ctx->wm, w, false))
H_DEFINE(_h_fullscr, vt_wm_fullscreen(ctx->wm, w, true))
H_DEFINE(_h_unfullscr, vt_wm_fullscreen(ctx->wm, w, false))

static int _h_tile(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                   vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _ctx_t *ctx = ud;
    vt_window_t *w = _win_from_payload(ctx, req);
    if (!w) return -1;
    const char *mode = _payload_str(req, "mode");
    vt_wm_tile_t t = VT_WM_TILE_NONE;
    if (mode) {
        if (vt_streq(mode, "left")) t = VT_WM_TILE_LEFT;
        else if (vt_streq(mode, "right")) t = VT_WM_TILE_RIGHT;
        else if (vt_streq(mode, "top")) t = VT_WM_TILE_TOP;
        else if (vt_streq(mode, "bottom")) t = VT_WM_TILE_BOTTOM;
        else if (vt_streq(mode, "max")) t = VT_WM_TILE_MAX;
        else if (vt_streq(mode, "full")) t = VT_WM_TILE_FULLSCREEN;
    }
    if (t != VT_WM_TILE_NONE) vt_wm_tile(ctx->wm, w, t);
    return 0;
}

static int _h_move(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                   vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _ctx_t *ctx = ud;
    vt_window_t *w = _win_from_payload(ctx, req);
    if (!w) return -1;
    vt_wm_move(ctx->wm, w, (int)_payload_long(req, "x", w->x),
               (int)_payload_long(req, "y", w->y));
    return 0;
}

static int _h_resize(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                     vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _ctx_t *ctx = ud;
    vt_window_t *w = _win_from_payload(ctx, req);
    if (!w) return -1;
    vt_wm_resize(ctx->wm, w, (int)_payload_long(req, "w", w->w),
                 (int)_payload_long(req, "h", w->h));
    return 0;
}

static int _h_ws_switch(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                        vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _ctx_t *ctx = ud;
    long ws = _payload_long(req, "ws", -1);
    if (ws < 0) {
        const char *dir = _payload_str(req, "dir");
        if (dir && vt_streq(dir, "next"))
            ws = (vt_wm_workspace_current(ctx->wm) + 1) %
                 vt_wm_workspace_count(ctx->wm);
        else if (dir && vt_streq(dir, "prev"))
            ws = (vt_wm_workspace_current(ctx->wm) -
                  1 + vt_wm_workspace_count(ctx->wm)) %
                 vt_wm_workspace_count(ctx->wm);
    }
    if (ws >= 0) vt_wm_workspace_switch(ctx->wm, (int)ws);
    return 0;
}

static int _h_ws_move(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                      vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _ctx_t *ctx = ud;
    vt_window_t *w = _win_from_payload(ctx, req);
    if (!w) return -1;
    vt_wm_workspace_move(ctx->wm, w, (int)_payload_long(req, "ws", 0));
    return 0;
}

static int _h_launch(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                     vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp; (void)ud;
    const char *cmd = _payload_str(req, "cmd");
    if (!cmd || !*cmd) return -1;
    vt_proc_spawn_detached(cmd);
    return 0;
}

/* Headless test hook: inject a pointer event through the backend's
 * REAL input pipeline (see vt-backend.h). Only the Wayland backend
 * implements it; X11 answers "unsupported" because its input comes
 * from the X server directly. */
static int _h_test_input(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                         vt_ipc_msg_t *resp, void *ud) {
    (void)ipc;
    _ctx_t *ctx = ud;
    const char *spec = _payload_str(req, "spec");
    if (!spec || !*spec) return -1;
    vt_wm_t *wm = ctx ? ctx->wm : NULL;
    if (!wm || !wm->backend || !wm->backend->test_input) {
        resp->payload = (uint8_t *)vt_strdup("unsupported");
        resp->len = 11;
        return 0;
    }
    int rc = wm->backend->test_input(wm->backend, spec);
    char *out = vt_strprintf("%s", rc == 0 ? "ok" : "rejected");
    resp->payload = (uint8_t *)out;
    resp->len = (uint32_t)strlen(out) + 1;
    return 0;
}

static int _h_ping(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                   vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req; (void)ud;
    const char *pong = "pong";
    resp->payload = (uint8_t *)vt_strdup(pong);
    resp->len = 4;
    return 0;
}

/* Clean logout for a standalone compositor run (no session manager):
 * stop the main loop — teardown restores the CRTC, returns the VT to
 * text mode and exits 0. No SIGKILL anywhere. */
static int _h_logout(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                     vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req; (void)resp;
    _ctx_t *ctx = ud;
    const char *action = "logout";
    if (req && req->payload && req->len)
        action = (const char *)req->payload;
    vt_logi("wm: logout requested (%s) — shutting down cleanly", action);
    if (ctx && ctx->wm)
        vt_logi("wm: policy: graceful stop (SIGTERM-style unwind; "
                "logout never uses SIGKILL)");
    _stop = 1;
    return 0;
}

/* ---------------------------------------------------------- hotkeys */
static void _spawn_term(vt_wm_t *wm, void *ud) {
    (void)wm; (void)ud;
    const char *term = getenv("TERMINAL");
    if (!term || !*term || !vt_proc_find_in_path(term)) {
        static const char *fallbacks[] = {
            "xterm", "x-terminal-emulator", "weston-terminal",
            "alacritty", "kitty", "foot", "st", NULL
        };
        term = NULL;
        for (int i = 0; fallbacks[i] && !term; i++)
            if (vt_proc_find_in_path(fallbacks[i])) term = fallbacks[i];
    }
    if (term) vt_proc_spawn_detached(term);
    else vt_logw("wm: no terminal emulator found in PATH");
}

static void _hk_cycle(vt_wm_t *wm, void *ud) {
    /* Alt+Tab: cycle focus through windows on this workspace.
     * wm->windows is MRU order (most recent last), so the cycle picks
     * the entry just below the focused one, wrapping to the top. */
    (void)ud;
    vt_window_t *cur = vt_wm_focused(wm);
    vt_window_t *cand[64];
    int nc = 0;
    for (size_t i = 0; i < wm->windows.size && nc < 64; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (w->minimized) continue;
        if (!w->sticky && w->workspace != vt_wm_workspace_current(wm)) continue;
        cand[nc++] = w;
    }
    if (nc == 0) return;
    if (nc == 1) { vt_wm_focus(wm, cand[0]); return; }
    int idx = 0;
    for (int i = 0; i < nc; i++) if (cand[i] == cur) { idx = i; break; }
    vt_wm_focus(wm, cand[(idx + nc - 1) % nc]);
}

static void _hk_close_focused(vt_wm_t *wm, void *ud) {
    (void)ud;
    vt_window_t *w = vt_wm_focused(wm);
    if (w) vt_wm_close(wm, w);
}

static void _hk_ws_n(vt_wm_t *wm, void *ud) {
    long n = (long)(intptr_t)ud;
    vt_wm_workspace_switch(wm, (int)n - 1);
}

static void _hk_ws_next(vt_wm_t *wm, void *ud) {
    (void)ud;
    int n = vt_wm_workspace_count(wm);
    if (n > 0) vt_wm_workspace_switch(wm, (vt_wm_workspace_current(wm) + 1) % n);
}
static void _hk_ws_prev(vt_wm_t *wm, void *ud) {
    (void)ud;
    int n = vt_wm_workspace_count(wm);
    if (n > 0) vt_wm_workspace_switch(wm, (vt_wm_workspace_current(wm) - 1 + n) % n);
}
static void _hk_logout(vt_wm_t *wm, void *ud) {
    (void)wm; (void)ud;
    vt_logi("wm: logout hotkey — shutting down cleanly (graceful "
            "unwind, exit 0)");
    _stop = 1;
}

static void _hk_tile_focused(vt_wm_t *wm, void *ud, vt_wm_tile_t t) {
    (void)ud;
    vt_window_t *w = vt_wm_focused(wm);
    if (w) vt_wm_tile(wm, w, t);
}

static void _hk_tile_left(vt_wm_t *wm, void *ud)  { _hk_tile_focused(wm, ud, VT_WM_TILE_LEFT); }
static void _hk_tile_right(vt_wm_t *wm, void *ud) { _hk_tile_focused(wm, ud, VT_WM_TILE_RIGHT); }
static void _hk_tile_top(vt_wm_t *wm, void *ud)   { _hk_tile_focused(wm, ud, VT_WM_TILE_TOP); }
static void _hk_tile_bottom(vt_wm_t *wm, void *ud){ _hk_tile_focused(wm, ud, VT_WM_TILE_BOTTOM); }
static void _hk_maximize(vt_wm_t *wm, void *ud) {
    (void)ud;
    vt_window_t *w = vt_wm_focused(wm);
    if (w) vt_wm_maximize(wm, w, !w->maximized);
}
static void _hk_fullscreen(vt_wm_t *wm, void *ud) {
    (void)ud;
    vt_window_t *w = vt_wm_focused(wm);
    if (w) vt_wm_fullscreen(wm, w, !w->fullscreen);
}

static void _register_defaults(vt_wm_t *wm) {
    vt_wm_shortcut_register(wm, "Alt+Tab",     _hk_cycle, NULL);
    vt_wm_shortcut_register(wm, "Alt+F4",      _hk_close_focused, NULL);
    vt_wm_shortcut_register(wm, "Ctrl+Alt+T",  _spawn_term, NULL);
    vt_wm_shortcut_register(wm, "Ctrl+Alt+Right", _hk_ws_next, NULL);
    vt_wm_shortcut_register(wm, "Ctrl+Alt+Left",  _hk_ws_prev, NULL);
    vt_wm_shortcut_register(wm, "Ctrl+Alt+Delete", _hk_logout, NULL);
    vt_wm_shortcut_register(wm, "Alt+Left",    _hk_tile_left, NULL);
    vt_wm_shortcut_register(wm, "Alt+Right",   _hk_tile_right, NULL);
    vt_wm_shortcut_register(wm, "Alt+Up",      _hk_tile_top, NULL);
    vt_wm_shortcut_register(wm, "Alt+Down",    _hk_tile_bottom, NULL);
    vt_wm_shortcut_register(wm, "Alt+M",       _hk_maximize, NULL);
    vt_wm_shortcut_register(wm, "Alt+Return",  _hk_fullscreen, NULL);
    for (int i = 1; i <= 9; i++) {
        char combo[16];
        snprintf(combo, sizeof(combo), "Alt+%d", i);
        vt_wm_shortcut_register(wm, combo, _hk_ws_n, (void *)(intptr_t)i);
    }
}

/* ---------------------------------------------------------- main */
int main(int argc, char **argv) {
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);
    signal(SIGPIPE, SIG_IGN);
    vt_log_set_level(VT_LOG_INFO);
    bool no_composite = false;
    vt_backend_kind_t cli_kind = VT_BACKEND_INVALID;

    for (int i = 1; i < argc; i++) {
        if (vt_streq(argv[i], "--no-composite")) no_composite = true;
        else if (vt_streq(argv[i], "--wayland")) cli_kind = VT_BACKEND_WAYLAND;
        else if (vt_streq(argv[i], "--x11")) cli_kind = VT_BACKEND_X11;
        else if (vt_streq(argv[i], "-v") || vt_streq(argv[i], "--verbose"))
            vt_log_set_level(VT_LOG_DEBUG);
        else if (vt_streq(argv[i], "-h") || vt_streq(argv[i], "--help")) {
            _print_usage(stdout, argv[0]);
            return 0;
        } else if (vt_streq(argv[i], "-V") || vt_streq(argv[i], "--version")) {
            printf("Vantage %s\n", vt_paths_version());
            return 0;
        } else {
            fprintf(stderr,
                    "vantage-wm: unrecognized option '%s'\n"
                    "Try 'vantage-wm --help' for usage.\n", argv[i]);
            return 2;
        }
    }

    vt_config_t *cfg = vt_config_new_defaults();
    vt_config_load(cfg, vt_config_default_path());

    /* Backend resolution matches vantage-session:
     * CLI flag > $VANTAGE_BACKEND > configuration > automatic. */
    vt_backend_kind_t be_kind = cli_kind;
    if (be_kind == VT_BACKEND_INVALID) {
        const char *env = getenv("VANTAGE_BACKEND");
        if (env && *env) be_kind = vt_backend_kind_from_str(env);
    }
    if (be_kind == VT_BACKEND_INVALID)
        be_kind = vt_backend_kind_from_str(
            vt_config_get(cfg, "desktop", "backend", "auto"));
    /* VT_BACKEND_AUTO falls through to the factory's own detection
     * (X11 when $DISPLAY is set, otherwise the Wayland compositor). */
    vt_backend_t *backend = vt_backend_new(be_kind);
    if (backend->kind == VT_BACKEND_HEADLESS) {
        vt_loge("wm: no display backend available: DISPLAY%s%s and the "
                "Wayland compositor could not start (WAYLAND_DISPLAY%s%s). "
                "See vantage-session --help.",
                getenv("DISPLAY") && *getenv("DISPLAY") ? "=" : " is unset (",
                getenv("DISPLAY") && *getenv("DISPLAY")
                    ? getenv("DISPLAY") : ")",
                getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")
                    ? "=" : " is unset (",
                getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")
                    ? getenv("WAYLAND_DISPLAY") : ")");
        vt_config_free(cfg);
        vt_backend_free(backend);
        return 1;
    }
    const char *srv = vt_backend_server_implementation(backend);
    if (backend->kind == VT_BACKEND_X11)
        vt_logi("wm: backend=x11, X server: %s", srv ? srv : "unknown");
    else
        vt_logi("wm: backend=wayland (native compositor)");

    vt_wm_t *wm = vt_wm_new(backend);
    _ctx_t ctx = { .wm = wm, .comp = NULL, .ipc = NULL };

    wm->on_window_event = _on_win_event;
    wm->on_desktop_changed = _on_desktop;
    wm->hooks_ud = &ctx;

    /* Wayland: mirror xdg windows into the WM model, route compositor
     * hotkeys through the shortcut table, and honour the [wayland]
     * scanout configuration (auto|gbm|dumb). */
    if (backend->kind == VT_BACKEND_WAYLAND) {
        const char *scanout = vt_config_get(cfg, "wayland", "scanout",
                                            "auto");
        if (scanout && *scanout && !getenv("VANTAGE_WAYLAND_SCANOUT"))
            setenv("VANTAGE_WAYLAND_SCANOUT", scanout, 1);
        vt_backend_add_event_sink(backend, _wl_event_sink, &ctx);
        if (backend->set_user_data)
            backend->set_user_data(backend, wm);
        backend->hotkey = _wl_hotkey;
    }

    _register_defaults(wm);

    /* focus policy: [wm] focus = click (default) | sloppy */
    if (backend->kind == VT_BACKEND_X11) {
        const char *focus = vt_config_get(cfg, "wm", "focus", "click");
        bool sloppy = vt_strcaseeq(focus, "sloppy") ||
                      vt_strcaseeq(focus, "follows-mouse");
        vt_wm_x11_set_focus_mode(wm->engine, sloppy);
    }

    if (vt_wm_start(wm) != VT_OK) {
        vt_loge("wm: failed to start (another WM running?)");
        vt_wm_free(wm);
        vt_config_free(cfg);
        vt_backend_free(backend);
        return 1;
    }

    /* compositor (config-gated, extension-checked, default-on but
     * everything expensive inside it is individually switchable) */
    bool want_composite = !no_composite &&
        vt_config_get_bool(cfg, "compositor", "enabled", true);
    if (want_composite) {
        vt_compositor_t *comp = vt_compositor_new(NULL, backend);
        vt_compositor_config_t cc = {0};
        vt_compositor_get_config(comp, &cc);
        cc.enable_shadows = vt_config_get_bool(cfg, "compositor", "shadows", false);
        cc.enable_transparency = vt_config_get_bool(cfg, "compositor", "transparency", true);
        cc.enable_vsync = vt_config_get_bool(cfg, "compositor", "vsync", true);
        cc.enable_blur = vt_config_get_bool(cfg, "compositor", "blur", false);
        cc.enable_animations = vt_config_get_bool(cfg, "compositor", "animations", false);
        cc.shadow_radius = (int)vt_config_get_int(cfg, "compositor", "shadow_radius", 12);
        vt_compositor_set_config(comp, &cc);
        if (vt_compositor_attach_x11(comp, wm) == VT_OK) {
            vt_compositor_start(comp);
            ctx.comp = comp;
        } else {
            vt_compositor_free(comp);
        }
    }

    /* IPC server */
    ctx.ipc = vt_ipc_new_server(NULL);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_PING,        _h_ping, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_QUERY,    _h_wm_query, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_WS_QUERY, _h_wm_ws_query, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_FOCUS,    _h_focus, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_CLOSE,    _h_close, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_MINIMIZE, _h_minimize, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_RESTORE,  _h_restore, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_MAXIMIZE, _h_maximize, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_UNMAXIMIZE, _h_unmaximize, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_FULLSCR,  _h_fullscr, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_UNFULLSCR, _h_unfullscr, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_TILE,     _h_tile, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_MOVE,     _h_move, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_RESIZE,   _h_resize, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_WS_SWITCH,_h_ws_switch, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_WS_MOVE,  _h_ws_move, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_LAUNCH,   _h_launch, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_TEST_INPUT, _h_test_input, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_LOGOUT,  _h_logout, &ctx);
    vt_logi("wm: ipc server at %s", vt_ipc_get_path(ctx.ipc));

    while (!_stop) {
        uint64_t _t0 = vt_time_now_us();
        vt_wm_step(wm, 20);
        uint64_t _t1 = vt_time_now_us();
        vt_ipc_step(ctx.ipc, 0);
        uint64_t _t2 = vt_time_now_us();
        if (ctx.comp) vt_compositor_step(ctx.comp, 0);
        uint64_t _t3 = vt_time_now_us();
        if (_t3 - _t0 > 50000) {
            vt_logd("wm-loop: slow iteration: wm=%lluus ipc=%lluus comp=%lluus",
                    (unsigned long long)(_t1 - _t0),
                    (unsigned long long)(_t2 - _t1),
                    (unsigned long long)(_t3 - _t2));
        }
    }

    vt_logi("wm: shutting down");
    vt_ipc_free(ctx.ipc);
    if (ctx.comp) { vt_compositor_stop(ctx.comp); vt_compositor_free(ctx.comp); }
    vt_wm_stop(wm);
    vt_wm_free(wm);
    vt_config_free(cfg);
    vt_backend_free(backend);
    return 0;
}
