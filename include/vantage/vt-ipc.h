/*
 * vt-ipc.h — Vantage internal IPC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides a lightweight Unix-domain-socket RPC protocol so that
 * Vantage components can talk to each other without depending on D-Bus.
 *
 * Wire format (length-prefixed JSON-ish messages, but binary-safe):
 *
 *     [u32 LE magic=0x56544352][u32 LE msg_id][u32 LE type][u32 LE len][bytes]
 *
 * Magic = 'V' 'T' 'C' 'R' = 0x56544352
 *
 * D-Bus integration is provided as an OPTIONAL separate module (see
 * vt-integ-dbus.h) and never required at runtime.
 */
#ifndef VANTAGE_IPC_H
#define VANTAGE_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT_IPC_MAGIC   0x56544352u  /* 'VTCR' */
#define VT_IPC_DEFAULT_SOCKET "vantage.sock"
#define VT_IPC_DEFAULT_DIR    "/run/vantage"

typedef enum {
    VT_IPC_MSG_REQUEST   = 1,
    VT_IPC_MSG_RESPONSE  = 2,
    VT_IPC_MSG_EVENT     = 3,
    VT_IPC_MSG_ERROR     = 4,
} vt_ipc_msg_type_t;

typedef enum {
    VT_IPC_OK         = 0,
    VT_IPC_E_BADMAGIC = -1,
    VT_IPC_E_BADTYPE  = -2,
    VT_IPC_E_TRUNC    = -3,
    VT_IPC_E_POLL     = -4,
    VT_IPC_E_CONN     = -5,
    VT_IPC_E_NOMEM    = -6,
} vt_ipc_status_t;

typedef struct vt_ipc_msg {
    uint32_t  id;
    uint32_t  type;
    uint32_t  len;
    uint8_t  *payload;
} vt_ipc_msg_t;

typedef struct vt_ipc vt_ipc_t;
typedef int (*vt_ipc_handler_t)(vt_ipc_t *ipc,
                                 const vt_ipc_msg_t *req,
                                 vt_ipc_msg_t *resp,
                                 void *ud);

vt_ipc_t *vt_ipc_new_server(const char *path);
vt_ipc_t *vt_ipc_new_client(const char *path);
void      vt_ipc_free(vt_ipc_t *ipc);
int       vt_ipc_get_fd(const vt_ipc_t *ipc);
const char *vt_ipc_get_path(const vt_ipc_t *ipc);

/* Register a handler for a message id range. */
int  vt_ipc_register(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_handler_t h, void *ud);

/* Single message dispatch loop step (returns 0 on success, <0 on error,
 * 1 if would-block). */
int  vt_ipc_step(vt_ipc_t *ipc, int timeout_ms);

/* Send a message (request/response/event). */
int  vt_ipc_send(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_msg_type_t type,
                  const void *data, uint32_t len);

/* Broadcast an event to every subscribed client (server only). */
int  vt_ipc_broadcast(vt_ipc_t *ipc, uint32_t msg_id, const void *data,
                      uint32_t len);

/* Free a message payload received via vt_ipc_call / client dispatch. */
void vt_ipc_msg_free(vt_ipc_msg_t *m);

/* Convenience: send a request, wait for response with timeout. */
int  vt_ipc_call(vt_ipc_t *ipc, uint32_t msg_id,
                  const void *req, uint32_t req_len,
                  vt_ipc_msg_t *resp, int timeout_ms);

/* Wire framing — exposed for tests */
int  vt_ipc_encode(const vt_ipc_msg_t *m, uint8_t **out, size_t *out_len);
int  vt_ipc_decode(const uint8_t *buf, size_t n, vt_ipc_msg_t *out);

/* Common message ids used by Vantage components.
 * Payloads are single-line UTF-8 text (key=value) for debuggability. */
enum {
    VT_IPC_MSG_HELLO       = 0x0001,
    VT_IPC_MSG_PING        = 0x0002,
    VT_IPC_MSG_QUIT        = 0x0003,
    VT_IPC_MSG_RELOAD      = 0x0004,
    VT_IPC_MSG_SUBSCRIBE   = 0x0005,   /* client → server: send me events */
    VT_IPC_MSG_EVENT_OUT   = 0x0006,   /* server → clients: event line */
    VT_IPC_MSG_LOG_LINE    = 0x0007,
    VT_IPC_MSG_PANEL_QUERY = 0x0010,
    /* WM commands (payload: "id=<window-id>" or "ws=<n>" etc.) */
    VT_IPC_MSG_WM_QUERY    = 0x0020,   /* → list of windows, one per line */
    VT_IPC_MSG_WM_FOCUS    = 0x0021,
    VT_IPC_MSG_WM_CLOSE    = 0x0022,
    VT_IPC_MSG_WM_TILE     = 0x0023,
    VT_IPC_MSG_WM_WS_SWITCH= 0x0024,
    VT_IPC_MSG_WM_WS_QUERY = 0x0025,   /* → workspace count + current */
    VT_IPC_MSG_WM_MINIMIZE = 0x0026,
    VT_IPC_MSG_WM_MAXIMIZE = 0x0027,
    VT_IPC_MSG_WM_FULLSCR  = 0x0028,
    VT_IPC_MSG_WM_MOVE     = 0x0029,
    VT_IPC_MSG_WM_RESIZE   = 0x002a,
    VT_IPC_MSG_WM_LAUNCH   = 0x002b,   /* payload: command line to spawn */
    VT_IPC_MSG_WM_LOGOUT   = 0x002c,
    /* WM events (broadcast, payload: one text line) */
    VT_IPC_MSG_WM_EVENT    = 0x0040,   /* window-opened|closed|focused|... */
    VT_IPC_MSG_WM_WS_EVENT = 0x0041,   /* workspace-changed <n> */
};

#ifdef __cplusplus
}
#endif
#endif
