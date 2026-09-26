/*
 * vantage-remote.c — CLI control client for the running Vantage session
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Talks to the vantage-wm IPC server (window commands) and the
 * vantage-session IPC server (status). Also usable from scripts and as the
 * reference implementation of the IPC client API.
 *
 *   vantage-remote list                 — list managed windows
 *   vantage-remote focus <id>           — focus a window
 *   vantage-remote close <id>           — close a window
 *   vantage-remote minimize <id>        — minimize a window
 *   vantage-remote restore <id>         — un-minimize a window
 *   vantage-remote maximize <id>        — toggle maximize
 *   vantage-remote unmaximize <id>      — clear maximize
 *   vantage-remote fullscreen <id>      — toggle fullscreen
 *   vantage-remote unfullscreen <id>    — clear fullscreen
 *   vantage-remote tile <id> <mode>     — left|right|top|bottom|max|full
 *   vantage-remote move <id> <x> <y>    — move window
 *   vantage-remote resize <id> <w> <h>  — resize window
 *   vantage-remote ws                   — show workspace info
 *   vantage-remote ws <n>               — switch workspace (1-based)
 *   vantage-remote ws next|prev         — cycle workspaces
 *   vantage-remote ws-move <id> <n>     — move window to workspace
 *   vantage-remote launch <cmd...>      — spawn a command
 *   vantage-remote status               — session version/stage/children
 *   vantage-remote watch                — subscribe to events
 */

#define VT_LOG_DOMAIN "remote"
#include <vantage/vt-core.h>
#include <vantage/vt-ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static void _print_usage(FILE *f) {
    fprintf(f,
        "usage: vantage-remote <command> [args]\n"
        "  list                    show managed windows\n"
        "  focus <id>              focus window\n"
        "  close <id>              close window\n"
        "  minimize <id>           minimize window\n"
        "  restore <id>            un-minimize window\n"
        "  maximize <id>           toggle maximize\n"
        "  unmaximize <id>         clear maximize\n"
        "  fullscreen <id>         toggle fullscreen\n"
        "  unfullscreen <id>       clear fullscreen\n"
        "  tile <id> <mode>        left|right|top|bottom|max|full\n"
        "  move <id> <x> <y>       move window\n"
        "  resize <id> <w> <h>     resize window\n"
        "  ws [n|next|prev]        workspace info / switch\n"
        "  ws-move <id> <n>        move window to workspace (1-based)\n"
        "  launch <cmd...>         spawn command\n"
        "  logout                  end the session cleanly\n"
        "  test-input <spec>       headless test hook (motion x=.. y=.. |\n"
        "                          press b=.. | release b=.. | axis d=..)\n"
        "  reboot|shutdown|        power actions (session manager,\n"
        "  suspend|hibernate         logind-powered when available)\n"
        "  status                  session version/stage/children\n"
        "  watch                   stream WM events\n");
}

static int _do_call(vt_ipc_t *ipc, uint32_t id, const char *payload,
                    vt_ipc_msg_t *resp) {
    int rc = vt_ipc_call(ipc, id, payload, (uint32_t)strlen(payload), resp, 2000);
    if (rc == VT_IPC_E_HANDLER) {
        fprintf(stderr, "vantage-remote: request rejected (unknown window "
                        "id or invalid arguments)\n");
        return 1;
    }
    if (rc != VT_IPC_OK) {
        fprintf(stderr, "vantage-remote: no response from vantage-wm "
                        "(is it running?)\n");
        return 1;
    }
    return 0;
}

/* Returns the n-th tab-separated field of a window-list line, or NULL. */
static const char *_wm_field(const char *line, int idx) {
    const char *p = line;
    for (int i = 0; i < idx; i++) {
        p = strchr(p, '\t');
        if (!p) return NULL;
        p++;
    }
    return p;
}

/* Queries the window list and reports whether the window with this id
 * currently carries `flag` ('M' minimized, 'X' maximized, 'S' fullscreen). */
static bool _wm_flag_of(vt_ipc_t *ipc, long id, char flag) {
    vt_ipc_msg_t resp = {0};
    if (_do_call(ipc, VT_IPC_MSG_WM_QUERY, "", &resp)) return false;
    bool found = false;
    if (resp.payload && resp.len) {
        char *buf = vt_malloc((size_t)resp.len + 1);
        if (buf) {
            memcpy(buf, resp.payload, resp.len);
            buf[resp.len] = '\0';
            for (char *ln = strtok(buf, "\n"); ln && !found;
                 ln = strtok(NULL, "\n")) {
                if (strtol(ln, NULL, 0) != id) continue;
                const char *flags = _wm_field(ln, 3);
                if (flags && strchr(flags, flag)) found = true;
            }
            vt_free(buf);
        }
    }
    vt_ipc_msg_free(&resp);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 2) { _print_usage(stderr); return 1; }
    const char *cmd = argv[1];
    signal(SIGPIPE, SIG_IGN);

    vt_ipc_t *ipc = vt_ipc_new_client(NULL);

    if (vt_streq(cmd, "list") || vt_streq(cmd, "windows")) {
        vt_ipc_msg_t resp = {0};
        if (_do_call(ipc, VT_IPC_MSG_WM_QUERY, "", &resp)) { vt_ipc_free(ipc); return 1; }
        if (resp.payload && resp.len) {
            printf("%.*s", (int)resp.len, (char *)resp.payload);
        }
        vt_ipc_msg_free(&resp);
        vt_ipc_free(ipc);
        return 0;
    }
    if (vt_streq(cmd, "ws")) {
        vt_ipc_msg_t resp = {0};
        if (_do_call(ipc, VT_IPC_MSG_WM_WS_QUERY, "", &resp)) { vt_ipc_free(ipc); return 1; }
        if (argc >= 3) {
            char payload[64];
            if (vt_streq(argv[2], "next") || vt_streq(argv[2], "prev"))
                snprintf(payload, sizeof(payload), "dir=%s", argv[2]);
            else
                snprintf(payload, sizeof(payload), "ws=%d", atoi(argv[2]) - 1);
            vt_ipc_msg_free(&resp);
            if (_do_call(ipc, VT_IPC_MSG_WM_WS_SWITCH, payload, &resp)) {
                vt_ipc_free(ipc); return 1;
            }
            vt_ipc_msg_free(&resp);
            printf("switched\n");
        } else if (resp.payload && resp.len) {
            printf("%.*s\n", (int)resp.len, (char *)resp.payload);
        }
        vt_ipc_free(ipc);
        return 0;
    }
    if (vt_streq(cmd, "test-input") && argc >= 3) {
        /* headless test hook — join remaining args as one spec */
        vt_strbuilder_t sb;
        vt_strbuilder_init(&sb, 256);
        vt_strbuilder_append(&sb, "spec=");
        for (int i = 2; i < argc; i++) {
            if (i > 2) vt_strbuilder_append(&sb, " ");
            vt_strbuilder_append(&sb, argv[i]);
        }
        char *payload = vt_strbuilder_finish(&sb, NULL);
        vt_ipc_msg_t resp = {0};
        int rc = _do_call(ipc, VT_IPC_MSG_WM_TEST_INPUT, payload, &resp);
        vt_free(payload);
        if (rc == 0 && resp.payload && resp.len)
            printf("%.*s\n", (int)resp.len, (char *)resp.payload);
        vt_ipc_msg_free(&resp);
        vt_ipc_free(ipc);
        return rc;
    }
    if (vt_streq(cmd, "launch") && argc >= 3) {
        /* join remaining args into one command line (the WM handler
         * parses key=value payloads: cmd=<line>) */
        vt_strbuilder_t sb;
        vt_strbuilder_init(&sb, 256);
        vt_strbuilder_append(&sb, "cmd=");
        for (int i = 2; i < argc; i++) {
            if (i > 2) vt_strbuilder_append(&sb, " ");
            vt_strbuilder_append(&sb, argv[i]);
        }
        char *payload = vt_strbuilder_finish(&sb, NULL);
        vt_ipc_msg_t resp = {0};
        int rc = _do_call(ipc, VT_IPC_MSG_WM_LAUNCH, payload, &resp);
        vt_free(payload);
        vt_ipc_msg_free(&resp);
        vt_ipc_free(ipc);
        return rc;
    }
    if (vt_streq(cmd, "logout") || vt_streq(cmd, "reboot") ||
        vt_streq(cmd, "shutdown") || vt_streq(cmd, "suspend") ||
        vt_streq(cmd, "hibernate")) {
        /* Session-managed run: the session manager owns the child
         * supervision and the shutdown policy (SIGTERM + grace period;
         * SIGKILL only as a documented last resort). Standalone WM run:
         * the compositor itself performs the clean unwind (CRTC
         * restore, VT back to text). */
        char *sock = vt_strprintf("%s/vantage-session.sock",
                                  vt_runtime_dir());
        vt_ipc_t *sipc = vt_ipc_new_client(sock);
        vt_free(sock);
        vt_ipc_t *target = sipc ? sipc : ipc;
        vt_ipc_msg_t resp = {0};
        int rc = _do_call(target, VT_IPC_MSG_WM_LOGOUT, cmd, &resp);
        vt_ipc_msg_free(&resp);
        if (sipc) {
            vt_ipc_free(sipc);
            if (rc == 0) printf("%s requested from the session manager\n",
                                cmd);
            else printf("%s\n",
                        "session manager unreachable — trying the WM");
            if (rc != 0) {
                rc = _do_call(ipc, VT_IPC_MSG_WM_LOGOUT, cmd, &resp);
                vt_ipc_msg_free(&resp);
            }
        } else {
            if (rc == 0) printf("%s requested from the compositor\n", cmd);
        }
        vt_ipc_free(ipc);
        return rc;
    }
    if (vt_streq(cmd, "status")) {
        /* served by vantage-session, not vantage-wm */
        char *sock = vt_strprintf("%s/vantage-session.sock",
                                  vt_runtime_dir());
        vt_ipc_t *sipc = vt_ipc_new_client(sock);
        vt_free(sock);
        if (!sipc) {
            fprintf(stderr, "vantage-remote: out of memory\n");
            vt_ipc_free(ipc);
            return 1;
        }
        vt_ipc_msg_t resp = {0};
        int rc = _do_call(sipc, VT_IPC_MSG_SESSION_STATUS, "", &resp);
        if (rc == 0 && resp.payload && resp.len)
            printf("%.*s", (int)resp.len, (char *)resp.payload);
        vt_ipc_msg_free(&resp);
        vt_ipc_free(sipc);
        vt_ipc_free(ipc);
        return rc;
    }

    if (vt_streq(cmd, "watch")) {
        /* subscribe and stream events */
        if (vt_ipc_send(ipc, VT_IPC_MSG_SUBSCRIBE, VT_IPC_MSG_REQUEST,
                        "", 0) != VT_IPC_OK) {
            fprintf(stderr, "vantage-remote: cannot connect\n");
            vt_ipc_free(ipc);
            return 1;
        }
        printf("watching events...\n");
        for (;;) {
            int rc = vt_ipc_step(ipc, 500);
            if (rc < 0 && rc != 1) break;
        }
        vt_ipc_free(ipc);
        return 0;
    }

    /* window-id based commands */
    if (argc < 3) { _print_usage(stderr); vt_ipc_free(ipc); return 1; }
    long id = strtol(argv[2], NULL, 0);
    char payload[64];
    snprintf(payload, sizeof(payload), "id=%ld", id);
    uint32_t msg = 0;
    if (vt_streq(cmd, "focus"))         msg = VT_IPC_MSG_WM_FOCUS;
    else if (vt_streq(cmd, "close"))    msg = VT_IPC_MSG_WM_CLOSE;
    else if (vt_streq(cmd, "minimize")) msg = VT_IPC_MSG_WM_MINIMIZE;
    else if (vt_streq(cmd, "restore"))  msg = VT_IPC_MSG_WM_RESTORE;
    else if (vt_streq(cmd, "maximize"))
        msg = _wm_flag_of(ipc, id, 'X') ? VT_IPC_MSG_WM_UNMAXIMIZE
                                        : VT_IPC_MSG_WM_MAXIMIZE;
    else if (vt_streq(cmd, "unmaximize"))  msg = VT_IPC_MSG_WM_UNMAXIMIZE;
    else if (vt_streq(cmd, "fullscreen"))
        msg = _wm_flag_of(ipc, id, 'S') ? VT_IPC_MSG_WM_UNFULLSCR
                                        : VT_IPC_MSG_WM_FULLSCR;
    else if (vt_streq(cmd, "unfullscreen")) msg = VT_IPC_MSG_WM_UNFULLSCR;
    else if (vt_streq(cmd, "tile")) {
        if (argc < 4) { _print_usage(stderr); vt_ipc_free(ipc); return 1; }
        snprintf(payload, sizeof(payload), "id=%ld\nmode=%s", id, argv[3]);
        msg = VT_IPC_MSG_WM_TILE;
    }
    else if (vt_streq(cmd, "move")) {
        if (argc < 5) { _print_usage(stderr); vt_ipc_free(ipc); return 1; }
        snprintf(payload, sizeof(payload), "id=%ld\nx=%d\ny=%d",
                 id, atoi(argv[3]), atoi(argv[4]));
        msg = VT_IPC_MSG_WM_MOVE;
    }
    else if (vt_streq(cmd, "resize")) {
        if (argc < 5) { _print_usage(stderr); vt_ipc_free(ipc); return 1; }
        snprintf(payload, sizeof(payload), "id=%ld\nw=%d\nh=%d",
                 id, atoi(argv[3]), atoi(argv[4]));
        msg = VT_IPC_MSG_WM_RESIZE;
    }
    else if (vt_streq(cmd, "ws-move")) {
        if (argc < 4) { _print_usage(stderr); vt_ipc_free(ipc); return 1; }
        snprintf(payload, sizeof(payload), "id=%ld\nws=%d",
                 id, atoi(argv[3]) - 1);
        msg = VT_IPC_MSG_WM_WS_MOVE;
    }
    else { _print_usage(stderr); vt_ipc_free(ipc); return 1; }

    vt_ipc_msg_t resp = {0};
    int rc = _do_call(ipc, msg, payload, &resp);
    vt_ipc_msg_free(&resp);
    vt_ipc_free(ipc);
    return rc;
}
