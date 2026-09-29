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
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/prctl.h>

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

/* -------- session-owned X server (startx-equivalent) --------------
 * `vantage-session --x11` from a TTY used to fail with "requires a
 * running X server" — the user had to run startx by hand first. Now
 * the session launches its own Xorg: displayfd for the display number,
 * a generated MIT-MAGIC-COOKIE-1 authority file, -keeptty on the
 * current VT, and a supervised lifetime (SIGTERM at session end). */
static struct {
    pid_t  pid;
    char   auth_file[384];
    bool   active;
} _xsrv = { .pid = -1, .active = false };

static bool _xsrv_write_auth(const char *path, const unsigned char cookie[16]) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    /* Xauthority binary record: FamilyWild + empty address/number +
     * MIT-MAGIC-COOKIE-1 + 16 bytes — matches any display, exactly the
     * record `xauth add :0 . <cookie>` would write */
    unsigned char rec[2 + 2 + 2 + 2 + 16 + 2 + 16];
    size_t i = 0;
    unsigned char *w16 = &rec[i]; w16[0] = 0xff; w16[1] = 0xff; i += 2; /* FamilyWild */
    rec[i++] = 0; rec[i++] = 0;               /* address len 0 */
    rec[i++] = 0; rec[i++] = 0;               /* number len 0 */
    rec[i++] = 0; rec[i++] = 18;              /* name len: MIT-MAGIC-COOKIE-1 */
    memcpy(&rec[i], "MIT-MAGIC-COOKIE-1", 18); i += 18;
    rec[i++] = 0; rec[i++] = 16;              /* data len 16 */
    memcpy(&rec[i], cookie, 16); i += 16;
    bool ok = fwrite(rec, 1, i, f) == i;
    fclose(f);
    return ok;
}

static const char *_xsrv_find_binary(void) {
    static const char *const cands[] = {
        "/usr/lib/xorg/Xorg",      /* Debian: the real binary */
        "/usr/libexec/Xorg",       /* Fedora/Arch (logind layout) */
        "/usr/bin/Xorg",           /* wrapper script — still works */
        "/usr/bin/X",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (access(cands[i], X_OK) == 0) return cands[i];
    /* PATH search last (custom installs, dev prefixes) */
    const char *path = getenv("PATH");
    if (path && *path) {
        static char buf[512];
        const char *p = path;
        while (*p) {
            const char *e = strchr(p, ':');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len > 0 && len + 6 < sizeof(buf)) {
                memcpy(buf, p, len);
                buf[len] = '/';
                memcpy(buf + len + 1, "Xorg", 5);
                if (access(buf, X_OK) == 0) return buf;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    return NULL;
}

/* Launch the session X server. Returns 0 with the DISPLAY string set
 * in the environment (and XAUTHORITY), -1 on failure (caller falls
 * back to the manual-start instructions). */

/* print the last few lines of Xorg's log — the (EE) lines say WHY a
 * launch failed; swallowing them turned every real-hardware failure
 * into an opaque "just errors" */
static void _xsrv_print_log_tail(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *lines[12] = {0};
    int n = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        if (n == 12) {
            memmove(lines, lines + 1, sizeof(lines) - sizeof(lines[0]));
            n = 11;
        }
        lines[n++] = vt_strdup(buf);
    }
    fclose(f);
    for (int i = 0; i < n; i++) {
        vt_logw("  Xorg: %s", lines[i]);
        vt_free(lines[i]);
    }
}

static int _xsrv_launch(void) {
    const char *bin = _xsrv_find_binary();
    if (!bin) {
        vt_logw("session: no Xorg binary found — cannot auto-start the "
                "X server");
        return -1;
    }
    unsigned char cookie[16];
    bool have_random = false;
    int rf = open("/dev/urandom", O_RDONLY);
    if (rf >= 0) {
        have_random = read(rf, cookie, sizeof(cookie)) == (ssize_t)sizeof(cookie);
        close(rf);
    }
    if (!have_random) {
        /* fallback entropy: pid + time hashed into the cookie */
        uint64_t seed = ((uint64_t)getpid() << 32) ^
                        (uint64_t)time(NULL) ^ 0x9e3779b97f4a7c15ull;
        for (size_t i = 0; i < sizeof(cookie); i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            cookie[i] = (unsigned char)(seed >> 33);
        }
    }
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) rd = "/tmp";
    snprintf(_xsrv.auth_file, sizeof(_xsrv.auth_file),
             "%s/vantage-serverauth.%d", rd, (int)getpid());
    if (!_xsrv_write_auth(_xsrv.auth_file, cookie)) {
        vt_logw("session: cannot write %s", _xsrv.auth_file);
        return -1;
    }
    /* Xorg's own diagnostics go HERE now, not /dev/null: when the
     * launch fails on real hardware the old code swallowed every
     * (EE) line and the user saw only "just errors". The tail of this
     * log is printed with the failure. */
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/vantage-xorg.log", rd);
    int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        /* child: Xorg with the displayfd at fd 7, stdio to the log,
         * kept on our tty (it needs a VT to run on). PDEATHSIG: if the
         * session itself dies, the server must not linger on the VT.
         *
         * NO setsid() here: -keeptty exists precisely so Xorg KEEPS
         * the controlling terminal for VT switching — detaching the
         * session first removed the controlling tty and broke VT
         * acquisition on real hardware. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (fds[1] != 7) { dup2(fds[1], 7); close(fds[1]); }
        close(fds[0]);
        if (logfd >= 0) {
            dup2(logfd, 1);
            dup2(logfd, 2);
        } else {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        }
        char vtarg[16] = "";
        const char *vtnr = getenv("XDG_VTNR");
        if (vtnr && *vtnr)
            snprintf(vtarg, sizeof(vtarg), "vt%s", vtnr);
        else {
            /* fd 0 may be redirected; the REAL controlling terminal
             * knows the VT we are on */
            char ttybuf[64] = "";
            int tfd = open("/dev/tty", O_RDONLY);
            if (tfd >= 0) {
                if (ttyname_r(tfd, ttybuf, sizeof(ttybuf) - 1) == 0) { }
                close(tfd);
            }
            if (!ttybuf[0] && ttyname_r(0, ttybuf, sizeof(ttybuf) - 1) != 0)
                ttybuf[0] = 0;
            if (ttybuf[0]) {
                const char *v = strstr(ttybuf, "tty");
                if (v && v[1] && v[3])
                    snprintf(vtarg, sizeof(vtarg), "vt%s", v + 3);
            }
        }
        if (vtarg[0])
            execl(bin, "Xorg", "-displayfd", "7", "-auth", _xsrv.auth_file,
                  "-nolisten", "tcp", "-noreset", "-keeptty", vtarg,
                  (char *)NULL);
        else
            execl(bin, "Xorg", "-displayfd", "7", "-auth", _xsrv.auth_file,
                  "-nolisten", "tcp", "-noreset", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (logfd >= 0) close(logfd);
    _xsrv.pid = pid;
    /* read the display number (Xorg writes it once the socket listens);
     * 15 s covers slow driver probes */
    struct pollfd pf = { .fd = fds[0], .events = POLLIN };
    char disp[8] = "";
    int n = -1;
    for (int t = 0; t < 150; t++) {
        if (poll(&pf, 1, 100) == 1) { n = (int)read(fds[0], disp, sizeof(disp) - 1); break; }
        if (waitpid(pid, NULL, WNOHANG) == pid) break;   /* Xorg died */
    }
    close(fds[0]);
    if (n <= 0) {
        vt_logw("session: Xorg did not report a display number — its "
                "last words (from %s):", logpath);
        _xsrv_print_log_tail(logpath);
        vt_logw("session: common causes: not launched from a real VT "
                "(run vantage-session --x11 from a TTY login), missing "
                "logind/seat permissions, or a driver that cannot "
                "initialize KMS. Otherwise try startx.");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        unlink(_xsrv.auth_file);
        _xsrv.pid = -1;
        return -1;
    }
    disp[n] = 0;
    int dnum = atoi(disp);
    if (dnum < 0 || dnum > 99) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); unlink(_xsrv.auth_file); _xsrv.pid = -1; return -1; }
    char dpy[16];
    snprintf(dpy, sizeof(dpy), ":%d", dnum);
    setenv("DISPLAY", dpy, 1);
    setenv("XAUTHORITY", _xsrv.auth_file, 1);
    _xsrv.active = true;
    vt_logi("session: started Xorg %s (pid %d, auth %s, log %s)", dpy, pid,
            _xsrv.auth_file, logpath);
    return 0;
}

static void _xsrv_shutdown(void) {
    if (!_xsrv.active || _xsrv.pid <= 0) return;
    kill(_xsrv.pid, SIGTERM);
    /* bounded grace: a wedged Xorg must not hang the logout */
    for (int i = 0; i < 30; i++) {
        if (waitpid(_xsrv.pid, NULL, WNOHANG) == _xsrv.pid) break;
        vt_time_sleep_ms(100);
    }
    kill(_xsrv.pid, SIGKILL);
    waitpid(_xsrv.pid, NULL, 0);
    unlink(_xsrv.auth_file);
    _xsrv.active = false;
    _xsrv.pid = -1;
    vt_logi("session: X server stopped");
}

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
        if ((!dpy || !*dpy) && _xsrv_launch() == 0)
            dpy = getenv("DISPLAY");
        if (!dpy || !*dpy) {
            vt_loge("session: the X11 backend requires a running X server.\n"
                    "  Auto-starting one failed (no Xorg found, or it\n"
                    "  could not open the display — missing logind/seat\n"
                    "  permissions). Start one manually (startx) or run\n"
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
        if (server_out) {
            /* COPY: the probe backend (and its heap strings) is freed
             * below — the old code returned a pointer into freed
             * memory (heap-use-after-free, caught by ASan). */
            static char srv_buf[64];
            snprintf(srv_buf, sizeof(srv_buf), "%s",
                     vt_backend_server_implementation(probe));
            *server_out = srv_buf;
        }
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
    /* Deterministic Wayland socket: the compositor binds exactly this
     * name (VANTAGE_WAYLAND_SOCKET) and the session exports the same
     * value as WAYLAND_DISPLAY for every child (panel, autostart apps)
     * — no runtime-dir polling, no races. */
    if (kind == VT_BACKEND_WAYLAND && !getenv("VANTAGE_WAYLAND_SOCKET")) {
        char sock[32];
        for (int i = 0; i < 64; i++) {
            snprintf(sock, sizeof(sock), "wayland-%d", i);
            char full[512];
            const char *rd = getenv("XDG_RUNTIME_DIR");
            snprintf(full, sizeof(full), "%s/%s",
                     rd && *rd ? rd : "/tmp", sock);
            if (access(full, F_OK) != 0) {
                setenv("VANTAGE_WAYLAND_SOCKET", sock, 1);
                break;
            }
        }
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
    /* cursor theme consistency for clients that still load their own
     * cursors (cursor-shape-v1 clients ask us instead, but older
     * toolkits fall back to XCURSOR_*): without a size hint every app
     * picked whatever it found — oversized mismatched pointers */
    setenv("XCURSOR_SIZE", "24", 0);
    if (!getenv("XCURSOR_THEME")) {
        /* prefer a real installed theme, else leave unset (toolkits
         * then use their default) */
        const char *const try_themes[] = {
            "Vantage-cursors", "Adwaita", "Breeze", "default",
        };
        for (size_t i = 0;
             i < sizeof(try_themes) / sizeof(try_themes[0]); i++) {
            char *p = vt_strprintf(
                "/usr/share/icons/%s/cursors", try_themes[i]);
            bool have = access(p, F_OK) == 0;
            vt_free(p);
            if (have) {
                setenv("XCURSOR_THEME", try_themes[i], 0);
                break;
            }
        }
    }
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
        _xsrv_shutdown();
        return 1;
    }
    char *wm_cmd = vt_strprintf("\"%s\" %s", wm_bin, be_flag);
    vt_free(wm_bin);
    vt_session_spawn_managed(s, "wm", wm_cmd, true);
    vt_free(wm_cmd);

    /* ONE panel for both backends: the GTK4 panel (subprojects/panel)
     * docks through layer-shell on Wayland and as an EWMH dock on X11.
     * The desktop is X11-only (the Wayland compositor paints it). */
    if (kind == VT_BACKEND_WAYLAND) {
        /* the compositor binds the socket asynchronously after startup;
         * children need it as WAYLAND_DISPLAY, so wait for it (the
         * compositor logs its 15 stages; 15 s covers cold GPU init) */
        const char *want = getenv("VANTAGE_WAYLAND_SOCKET");
        if (want && *want) {
            char full[512];
            const char *rd = getenv("XDG_RUNTIME_DIR");
            snprintf(full, sizeof(full), "%s/%s",
                     rd && *rd ? rd : "/tmp", want);
            for (int i = 0; i < 150; i++) {
                if (access(full, F_OK) == 0) break;
                vt_session_supervise(s);
                vt_time_sleep_ms(100);
            }
            if (access(full, F_OK) == 0) {
                setenv("WAYLAND_DISPLAY", want, 1);
                vt_logi("session: compositor socket %s ready", want);
            } else {
                vt_logw("session: compositor socket %s did not appear "
                        "within 15 s; panel may fail to connect", want);
            }
        }
    }
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
    if (kind == VT_BACKEND_X11 &&
        vt_config_get_bool(cfg, "desktop", "show", true)) {
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

    /* 4. XDG autostart (system + user) */
    vt_session_autostart_load(s);
    if (kind == VT_BACKEND_WAYLAND) {
        /* X11 autostart apps need the compositor's Xwayland DISPLAY —
         * which the WM exports on its IPC socket (its own setenv
         * cannot cross process boundaries). Wait for the WM socket,
         * ask once, and set DISPLAY/XAUTHORITY for the autostart
         * children; the panel launcher does the same dance per launch. */
        for (int i = 0; i < 100; i++) {
            char sock[512];
            snprintf(sock, sizeof(sock), "%s/vantage.sock",
                     vt_runtime_dir());
            if (access(sock, F_OK) == 0) break;
            vt_session_supervise(s);
            vt_time_sleep_ms(100);
        }
        char sock[512];
        snprintf(sock, sizeof(sock), "%s/vantage.sock", vt_runtime_dir());
        vt_ipc_t *ipc = vt_ipc_new_client(sock);
        if (ipc) {
            vt_ipc_msg_t resp = {0};
            for (int i = 0; i < 80; i++) {   /* Xwayland may still start */
                if (vt_ipc_call(ipc, VT_IPC_MSG_WM_XWL_ENV, "", 0,
                                &resp, 500) == VT_IPC_OK &&
                    resp.payload && resp.len) {
                    const char *p = (const char *)resp.payload;
                    const char *dl = strstr(p, "display=");
                    const char *al = strstr(p, "xauthority=");
                    if (dl && *(dl + 8)) {
                        char disp[64] = "", auth[512] = "";
                        sscanf(dl, "display=%63s", disp);
                        if (al) sscanf(al, "xauthority=%511s", auth);
                        setenv("DISPLAY", disp, 1);
                        if (auth[0]) setenv("XAUTHORITY", auth, 1);
                        vt_logi("session: Xwayland for autostart apps: "
                                "DISPLAY=%s", disp);
                        vt_ipc_msg_free(&resp);
                        break;
                    }
                    vt_ipc_msg_free(&resp);
                }
                vt_session_supervise(s);
                vt_time_sleep_ms(100);
            }
            vt_ipc_free(ipc);
        }
    }
    vt_session_autostart_run(s);

    /* 5. session IPC server */
    {
        char *sock = vt_strprintf("%s/vantage-session.sock",
                                  vt_runtime_dir());
        ctx.ipc = vt_ipc_new_server(sock);
        vt_free(sock);   /* new_server copies the path */
    }
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
    /* our own X server (if we started one) dies with the session */
    _xsrv_shutdown();
    vt_logi("session: exited");
    return 0;
}
