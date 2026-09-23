/*
 * vantage-session.c — Vantage session manager binary
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Session entry point (used by vantage.desktop / xsessions):
 *   1. sets up the desktop environment variables
 *   2. starts vantage-wm (critical, supervised with restart backoff)
 *   3. starts vantage-panel and vantage-desktop (supervised)
 *   4. runs XDG autostart entries (system + user, spec-compliant)
 *   5. runs the session IPC server (logout / reboot / shutdown / status)
 *   6. on SIGTERM or IPC logout: stops children gracefully and exits
 *
 * Works with or without systemd, D-Bus, and any Red Hat infrastructure.
 */

#define VT_LOG_DOMAIN "session"
#include <vantage/vt-core.h>
#include <vantage/vt-session.h>
#include <vantage/vt-config.h>
#include <vantage/vt-ipc.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

typedef struct {
    vt_session_t *session;
    vt_ipc_t     *ipc;
} _sctx_t;

/* ------------------------------------------------------------- IPC */
static int _h_ping(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                   vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req; (void)ud;
    resp->payload = (uint8_t *)vt_strdup("pong");
    resp->len = 4;
    return 0;
}

static int _h_status(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                     vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req;
    _sctx_t *ctx = ud;
    const char *stage[] = { "init", "early", "components", "ready",
                            "shutdown" };
    int st = (int)vt_session_stage(ctx->session);
    char *s = vt_strprintf("session=vantage %s\nstage=%s\nchildren=%zu\n",
                           VT_VERSION,
                           stage[st >= 0 && st < 5 ? st : 0],
                           vt_session_children_alive(ctx->session));
    resp->payload = (uint8_t *)s;
    resp->len = (uint32_t)strlen(s);
    return 0;
}

static int _h_end(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                  vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    _sctx_t *ctx = ud;
    const char *action = "";
    if (req && req->payload && req->len)
        action = (const char *)req->payload;
    vt_session_end_t how = VT_SESSION_END_LOGOUT;
    if (vt_strstartswith(action, "reboot")) how = VT_SESSION_END_REBOOT;
    else if (vt_strstartswith(action, "shutdown")) how = VT_SESSION_END_SHUTDOWN;
    else if (vt_strstartswith(action, "suspend")) how = VT_SESSION_END_SUSPEND;
    else if (vt_strstartswith(action, "hibernate")) how = VT_SESSION_END_HIBERNATE;
    vt_logi("session: end requested (%s)", action);
    vt_session_end(ctx->session, how);
    _stop = 1;
    return 0;
}

static int _h_reload(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                     vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)req; (void)resp; (void)ud;
    /* children watch their own config files (inotify) */
    resp->payload = (uint8_t *)vt_strdup("ok");
    resp->len = 2;
    return 0;
}

/* --------------------------------------------------------------- main */
int main(int argc, char **argv) {
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    vt_log_set_level(VT_LOG_INFO);
    for (int i = 1; i < argc; i++) {
        if (vt_streq(argv[i], "-v") || vt_streq(argv[i], "--verbose"))
            vt_log_set_level(VT_LOG_DEBUG);
    }

    vt_config_t *cfg = vt_config_new_defaults();
    vt_config_load(cfg, vt_config_default_path());

    vt_session_t *s = vt_session_new();
    _sctx_t ctx = { .session = s, .ipc = NULL };

    /* 1. environment */
    vt_session_start(s);   /* sets XDG_* vars, runs hooks */
    /* toolkit hints so GTK/Qt apps follow the Vantage theme */
    setenv("XDG_CURRENT_DESKTOP", "Vantage", 0);
    setenv("DESKTOP_SESSION", "vantage", 0);
    setenv("QT_QPA_PLATFORMTHEME", "vantage", 0);
    char *qt_plugins = vt_strprintf("%s/.local/lib/vantage/qt6",
                                    vt_home_dir());
    setenv("QT_PLUGIN_PATH", qt_plugins, 0);
    vt_free(qt_plugins);

    /* 2-3. supervised components */
    const char *backend = vt_config_get(cfg, "desktop", "backend", "auto");
    char *wm_cmd;
    if (vt_proc_find_in_path("vantage-wm"))
        wm_cmd = vt_strprintf("vantage-wm");
    else
        wm_cmd = vt_strprintf("\"%s/vantage-wm\"", VT_BINDIR);
    vt_session_spawn_managed(s, "wm", wm_cmd, true);
    vt_free(wm_cmd);
    (void)backend;

    if (vt_config_get_bool(cfg, "panel", "enabled", true)) {
        vt_session_spawn_managed(s, "panel",
                                 vt_proc_find_in_path("vantage-panel")
                                 ? "vantage-panel"
                                 : VT_BINDIR "/vantage-panel", false);
    }
    if (vt_config_get_bool(cfg, "desktop", "show", true)) {
        vt_session_spawn_managed(s, "desktop",
                                 vt_proc_find_in_path("vantage-desktop")
                                 ? "vantage-desktop"
                                 : VT_BINDIR "/vantage-desktop", false);
    }

    /* 4. XDG autostart (system + user) */
    vt_session_autostart_load(s);
    vt_session_autostart_run(s);

    /* 5. session IPC server */
    ctx.ipc = vt_ipc_new_server(vt_strprintf("%s/vantage-session.sock",
                                             vt_runtime_dir()));
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_PING,   _h_ping, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_RELOAD, _h_reload, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_QUIT,   _h_end, &ctx);
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_WM_LOGOUT, _h_end, &ctx);
    vt_logi("session: ready (ipc at %s)",
            vt_ipc_get_path(ctx.ipc));

    /* 6. main loop */
    while (!_stop && vt_session_is_running(s)) {
        vt_session_supervise(s);
        vt_ipc_step(ctx.ipc, 200);
    }

    /* shutdown: TERM children, wait briefly, KILL stragglers */
    vt_logi("session: stopping children");
    vt_session_end(s, VT_SESSION_END_LOGOUT);  /* enter SHUTDOWN stage:
                                                  no more restarts */
    vt_session_term_children(s);
    for (int i = 0; i < 30; i++) {
        vt_session_supervise(s);
        if (vt_session_children_alive(s) == 0) break;
        vt_time_sleep_ms(100);
    }
    vt_session_stop_children(s, SIGKILL);
    vt_ipc_free(ctx.ipc);
    vt_session_free(s);
    vt_config_free(cfg);
    vt_logi("session: exited");
    return 0;
}
