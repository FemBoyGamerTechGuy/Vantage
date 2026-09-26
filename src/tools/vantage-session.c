/*
 * vantage-session.c — Vantage session manager binary
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Session entry point. Backend selection:
 *
 *   vantage-session --wayland    native Vantage Wayland compositor session
 *   vantage-session --x11        X11 backend; client of the running X
 *                                server ($DISPLAY — Xorg, XLibre, or any
 *                                conforming X server; never launched by
 *                                Vantage, no privileges needed)
 *   vantage-session              automatic: $DISPLAY set -> X11,
 *                                otherwise Wayland
 *
 * Session lifecycle:
 *   1. resolves the display backend (CLI > $VANTAGE_BACKEND > config)
 *   2. pre-flights it (fail fast with a clear error, exit != 0)
 *   3. starts vantage-wm (critical, supervised with restart backoff)
 *   4. starts vantage-panel and vantage-desktop (X11 session; supervised)
 *   5. runs XDG autostart entries (system + user, spec-compliant)
 *   6. runs the session IPC server (logout / reboot / shutdown / status)
 *   7. on SIGTERM or IPC logout: stops children gracefully and exits
 *
 * Works with or without systemd, D-Bus, and any Red Hat infrastructure.
 */

#define VT_LOG_DOMAIN "session"
#include <vantage/vt-core.h>
#include <vantage/vt-paths.h>
#include <vantage/vt-session.h>
#include <vantage/vt-config.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-ipc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

typedef enum {
    SESS_BACKEND_CLI = 0,   /* explicit --wayland / --x11 */
    SESS_BACKEND_ENV,       /* $VANTAGE_BACKEND */
    SESS_BACKEND_CONFIG,    /* desktop.backend in vantage.conf */
    SESS_BACKEND_AUTO,      /* detected from the environment */
} _backend_src_t;

typedef struct {
    vt_session_t *session;
    vt_ipc_t     *ipc;
} _sctx_t;

/* ------------------------------------------------------------- usage */
static void _print_version(FILE *fp) {
    fprintf(fp, "Vantage %s\n", vt_paths_version());
}

static void _print_usage(FILE *fp, const char *argv0) {
    fprintf(fp,
"Vantage %s — session manager\n"
"\n"
"Usage:\n"
"  %s [OPTION]...\n"
"\n"
"Backend selection:\n"
"  --wayland        run the native Vantage Wayland compositor session\n"
"  --x11            run the X11 backend: connect to the X server named\n"
"                   by $DISPLAY (Xorg, XLibre, or any X server — the\n"
"                   server is never started by Vantage)\n"
"  (none)           automatic: X11 when $DISPLAY is set, else Wayland\n"
"\n"
"Options:\n"
"  -v, --verbose    verbose logging\n"
"  -h, --help       show this help and exit\n"
"  -V, --version    show version and exit\n"
"\n"
"The backend may also be set with $VANTAGE_BACKEND=wayland|x11 or the\n"
"desktop.backend key in the configuration file (CLI flags win).\n",
            vt_paths_version(), argv0);
}

/* ---------------------------------------------------------- backend */
static const char *_backend_describe(vt_backend_kind_t k, const char *srv) {
    static char buf[128];
    if (k == VT_BACKEND_X11 && srv && *srv)
        snprintf(buf, sizeof(buf), "X11 (%s server)", srv);
    else if (k == VT_BACKEND_X11)
        snprintf(buf, sizeof(buf), "X11");
    else
        snprintf(buf, sizeof(buf), "Wayland (native compositor)");
    return buf;
}

/* Pre-flight the chosen backend without keeping a connection open.
 * Returns 0 and fills `server_out` on success; -1 on failure (message
 * already logged). Only display-transport variables are inspected —
 * never desktop-environment variables. */
static int _preflight_backend(vt_backend_kind_t kind, const char **server_out) {
    if (kind == VT_BACKEND_X11) {
        const char *dpy = getenv("DISPLAY");
        if (!dpy || !*dpy) {
            vt_loge("session: the X11 backend requires a running X server,\n"
                    "  but DISPLAY is not set. Start one (e.g. from a\n"
                    "  display manager or `startx`), or run\n"
                    "  `vantage-session --wayland` for the native\n"
                    "  compositor.");
            return -1;
        }
        /* Verify we can actually connect to that server. */
        vt_backend_t *probe = vt_backend_new(VT_BACKEND_X11);
        if (!probe || probe->kind == VT_BACKEND_HEADLESS) {
            vt_loge("session: cannot connect to the X server '%s'.\n"
                    "  Is it running and reachable?", dpy);
            if (probe) vt_backend_free(probe);
            return -1;
        }
        if (server_out) *server_out = vt_backend_server_implementation(probe);
        vt_backend_free(probe);
        return 0;
    }
    if (kind == VT_BACKEND_WAYLAND) {
        const char *wd = getenv("WAYLAND_DISPLAY");
        if (wd && *wd) {
            vt_loge("session: WAYLAND_DISPLAY is set ('%s') — Vantage's\n"
                    "  Wayland backend is a compositor and does not nest\n"
                    "  inside another Wayland session. Run it from a\n"
                    "  display manager or a TTY instead.", wd);
            return -1;
        }
        /* A live X session would lose its DRM master to us (or we to
         * it): refuse instead of silently fighting over the card. */
        const char *dpy = getenv("DISPLAY");
        if (dpy && *dpy) {
            vt_backend_t *probe = vt_backend_new(VT_BACKEND_X11);
            bool live = probe && probe->kind == VT_BACKEND_X11;
            if (probe) vt_backend_free(probe);
            if (live) {
                vt_loge("session: DISPLAY='%s' points at a running X\n"
                        "  server — starting the native Wayland compositor\n"
                        "  now would try to take over its DRM master.\n"
                        "  Log into a TTY (Ctrl+Alt+F2..F6) and run\n"
                        "    vantage-session --wayland\n"
                        "  there, or let a display manager start it.",
                        dpy);
                return -1;
            }
            vt_logw("session: DISPLAY is set ('%s') but unreachable —\n"
                    "  passing it through untouched (stale variable?)",
                    dpy);
        }
        return 0;
    }
    vt_loge("session: internal error: invalid backend kind %d", (int)kind);
    return -1;
}

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
                           vt_paths_version(),
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

    vt_backend_kind_t cli_kind = VT_BACKEND_INVALID;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (vt_streq(argv[i], "--wayland")) {
            cli_kind = VT_BACKEND_WAYLAND;
        } else if (vt_streq(argv[i], "--x11")) {
            cli_kind = VT_BACKEND_X11;
        } else if (vt_streq(argv[i], "-v") || vt_streq(argv[i], "--verbose")) {
            verbose = true;
        } else if (vt_streq(argv[i], "-h") || vt_streq(argv[i], "--help")) {
            _print_usage(stdout, argv[0]);
            return 0;
        } else if (vt_streq(argv[i], "-V") || vt_streq(argv[i], "--version")) {
            _print_version(stdout);
            return 0;
        } else {
            fprintf(stderr,
                    "vantage-session: unrecognized option '%s'\n"
                    "Try 'vantage-session --help' for usage.\n",
                    argv[i]);
            return 2;
        }
    }
    if (verbose) vt_log_set_level(VT_LOG_DEBUG);

    vt_config_t *cfg = vt_config_new_defaults();
    vt_config_load(cfg, vt_config_default_path());

    /* ---- resolve the display backend (CLI > env > config > auto) ---- */
    vt_backend_kind_t kind = cli_kind;
    _backend_src_t src = SESS_BACKEND_CLI;
    if (kind == VT_BACKEND_INVALID) {
        const char *env = getenv("VANTAGE_BACKEND");
        if (env && *env) {
            kind = vt_backend_kind_from_str(env);
            if (kind == VT_BACKEND_AUTO) {
                vt_loge("session: invalid VANTAGE_BACKEND='%s' "
                        "(expected 'wayland' or 'x11')", env);
                vt_config_free(cfg);
                return 2;
            }
            src = SESS_BACKEND_ENV;
        }
    }
    if (kind == VT_BACKEND_INVALID) {
        const char *conf = vt_config_get(cfg, "desktop", "backend", "auto");
        kind = vt_backend_kind_from_str(conf);
        src = SESS_BACKEND_CONFIG;
    }
    if (kind == VT_BACKEND_AUTO) {
        /* Sensible automatic detection, display transports only:
         * an X server is already running -> use it; otherwise start
         * the native Wayland compositor. */
        const char *dpy = getenv("DISPLAY");
        kind = (dpy && *dpy) ? VT_BACKEND_X11 : VT_BACKEND_WAYLAND;
        src = SESS_BACKEND_AUTO;
    }

    const char *server = NULL;   /* X server implementation (informational) */
    if (_preflight_backend(kind, &server) != 0) {
        vt_config_free(cfg);
        return 1;
    }

    /* Children (wm/panel/desktop) inherit the environment; give the
     * compositor a hint about the VT this session was started on so the
     * direct (no-session-manager) seat path can find it. */
    if (!getenv("VANTAGE_VT")) {
        const char *vtnr = getenv("XDG_VTNR");
        if (vtnr && *vtnr)
            setenv("VANTAGE_VT", vtnr, 1);
    }

    static const char *src_name[] = { "command line", "environment",
                                      "configuration", "auto-detection" };
    vt_logi("session: display backend: %s [source: %s]",
            _backend_describe(kind, server), src_name[src]);

    /* Make the choice stick for everything this session launches. */
    setenv("VANTAGE_BACKEND", vt_backend_kind_str(kind), 1);

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

    /* 2-3. supervised components. The backend is passed explicitly so
     * the WM's choice always matches the session's. */
    const char *be_flag = (kind == VT_BACKEND_WAYLAND) ? "--wayland" : "--x11";
    char *wm_bin = vt_paths_bin_find("vantage-wm");
    if (!wm_bin) {
        vt_loge("session: cannot find the vantage-wm executable "
                "(VANTAGE_BIN_DIR, PATH, or the install prefix)");
        vt_session_free(s);
        vt_config_free(cfg);
        return 1;
    }
    char *wm_cmd = vt_strprintf("\"%s\" %s", wm_bin, be_flag);
    vt_free(wm_bin);
    vt_session_spawn_managed(s, "wm", wm_cmd, true);
    vt_free(wm_cmd);

    if (kind == VT_BACKEND_X11) {
        /* The panel and desktop are X11 clients; they run under the X11
         * backend on any conforming X server. */
        if (vt_config_get_bool(cfg, "panel", "enabled", true)) {
            char *bin = vt_paths_bin_find("vantage-panel");
            if (bin) {
                char *cmd = vt_strprintf("\"%s\"", bin);
                vt_session_spawn_managed(s, "panel", cmd, false);
                vt_free(cmd);
                vt_free(bin);
            } else {
                vt_logw("session: vantage-panel not found; skipping");
            }
        }
        if (vt_config_get_bool(cfg, "desktop", "show", true)) {
            char *bin = vt_paths_bin_find("vantage-desktop");
            if (bin) {
                char *cmd = vt_strprintf("\"%s\"", bin);
                vt_session_spawn_managed(s, "desktop", cmd, false);
                vt_free(cmd);
                vt_free(bin);
            } else {
                vt_logw("session: vantage-desktop not found; skipping");
            }
        }
    } else {
        vt_logi("session: the Wayland session draws its panel inside the "
                "compositor (Vantage menu, window list, workspaces, clock, "
                "session controls); the X11 panel/desktop clients are not "
                "needed there");
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
    vt_ipc_register(ctx.ipc, VT_IPC_MSG_SESSION_STATUS, _h_status, &ctx);
    vt_logi("session: ready (ipc at %s)",
            vt_ipc_get_path(ctx.ipc));

    /* 6. main loop */
    while (!_stop && vt_session_is_running(s)) {
        vt_session_supervise(s);
        vt_ipc_step(ctx.ipc, 200);
    }

    /* shutdown: TERM children, wait briefly, KILL stragglers */
    vt_logi("session: stopping children (policy: SIGTERM + %d ms grace; "
            "SIGKILL only as the documented last resort for survivors)",
            30 * 100);
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
