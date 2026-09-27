/*
 * ipc.h — Vantage WM IPC client (wire-compatible reimplementation)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * The panel talks to the window manager (vantage-wm, X11 or Wayland —
 * both serve the same protocol on $XDG_RUNTIME_DIR/vantage.sock) using
 * the Vantage IPC wire format:
 *
 *   [u32 LE magic=0x56544352 'VTCR'][u32 LE msg_id][u32 LE type]
 *   [u32 LE len][payload bytes]
 *
 * This file deliberately shares NO code with the compositor: the panel
 * is an independent subproject with its own build and license, like a
 * panel of any other desktop. Only the (documented, stable) wire
 * protocol is shared.
 */
#ifndef VPANEL_IPC_H
#define VPANEL_IPC_H

#include <stdint.h>
#include <stddef.h>

#define VP_IPC_MAGIC       0x56544352u

/* message types */
#define VP_IPC_REQUEST     1
#define VP_IPC_RESPONSE    2
#define VP_IPC_EVENT       3

/* ids used by the panel (subset of the Vantage protocol) */
enum {
    VP_IPC_SUBSCRIBE   = 0x0005,
    VP_IPC_WM_QUERY    = 0x0020,
    VP_IPC_WM_FOCUS    = 0x0021,
    VP_IPC_WM_CLOSE    = 0x0022,
    VP_IPC_WM_WS_SWITCH= 0x0024,
    VP_IPC_WM_WS_QUERY = 0x0025,
    VP_IPC_WM_MINIMIZE = 0x0026,
    VP_IPC_WM_LAUNCH   = 0x002b,
    VP_IPC_WM_LOGOUT   = 0x002c,
    VP_IPC_WM_RESTORE  = 0x002d,
    VP_IPC_WM_WS_MOVE  = 0x0030,
    VP_IPC_WM_EVENT    = 0x0040,
    VP_IPC_WM_WS_EVENT = 0x0041,
};

typedef struct vp_ipc vp_ipc_t;

struct vp_ipc {
    int    fd;
    char   path[256];
    int    subscribed;      /* server accepted our SUBSCRIBE */
};

/* connect to the WM socket (NULL → $XDG_RUNTIME_DIR/vantage.sock) */
vp_ipc_t *vp_ipc_connect(const char *path);
void      vp_ipc_free(vp_ipc_t *ipc);

/* blocking request/response with timeout (ms). Returns a malloc'd
 * NUL-terminated payload (caller frees) or NULL. */
char     *vp_ipc_call(vp_ipc_t *ipc, uint32_t msg_id,
                      const char *payload, int timeout_ms);

/* subscribe to WM events; after this, vp_ipc_poll_event() returns
 * event payloads (malloc'd "msg_id|payload" or NULL) */
int       vp_ipc_subscribe(vp_ipc_t *ipc);

/* drain the socket; returns the next event payload (malloc'd, caller
 * frees; *msg_id set) or NULL when nothing is pending */
char     *vp_ipc_poll_event(vp_ipc_t *ipc, uint32_t *msg_id);

int       vp_ipc_fd(const vp_ipc_t *ipc);

#endif
