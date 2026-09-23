/*
 * vantage-remote.c — CLI control client for the running Vantage WM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Talks to the vantage-wm IPC server. Also usable from scripts and as the
 * reference implementation of the IPC client API.
 *
 *   vantage-remote list                 — list managed windows
 *   vantage-remote focus <id>           — focus a window
 *   vantage-remote close <id>           — close a window
 *   vantage-remote minimize <id>        — minimize a window
 *   vantage-remote maximize <id>        — toggle maximize
 *   vantage-remote fullscreen <id>      — toggle fullscreen
 *   vantage-remote tile <id> <mode>     — left|right|top|bottom|max|full
 *   vantage-remote ws                   — show workspace info
 *   vantage-remote ws <n>               — switch workspace (1-based)
 *   vantage-remote ws next|prev         — cycle workspaces
 *   vantage-remote launch <cmd...>      — spawn a command
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
        "  maximize <id>           toggle maximize\n"
        "  fullscreen <id>         toggle fullscreen\n"
        "  tile <id> <mode>        left|right|top|bottom|max|full\n"
        "  move <id> <x> <y>       move window\n"
        "  ws [n|next|prev]        workspace info / switch\n"
        "  launch <cmd...>         spawn command\n"
        "  watch                   stream WM events\n");
}

static int _do_call(vt_ipc_t *ipc, uint32_t id, const char *payload,
                    vt_ipc_msg_t *resp) {
    int rc = vt_ipc_call(ipc, id, payload, (uint32_t)strlen(payload), resp, 2000);
    if (rc != VT_IPC_OK) {
        fprintf(stderr, "vantage-remote: no response from vantage-wm "
                        "(is it running?)\n");
        return 1;
    }
    return 0;
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
    if (vt_streq(cmd, "launch") && argc >= 3) {
        /* join remaining args into one command line */
        vt_strbuilder_t sb;
        vt_strbuilder_init(&sb, 256);
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
    else if (vt_streq(cmd, "maximize")) msg = VT_IPC_MSG_WM_MAXIMIZE;
    else if (vt_streq(cmd, "fullscreen")) msg = VT_IPC_MSG_WM_FULLSCR;
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
    else { _print_usage(stderr); vt_ipc_free(ipc); return 1; }

    vt_ipc_msg_t resp = {0};
    int rc = _do_call(ipc, msg, payload, &resp);
    vt_ipc_msg_free(&resp);
    vt_ipc_free(ipc);
    return rc;
}
