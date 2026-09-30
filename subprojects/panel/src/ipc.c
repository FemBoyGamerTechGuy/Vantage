/*
 * ipc.c — Vantage WM IPC client implementation
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

static void _le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static uint32_t _rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int _send_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* read exactly n bytes (poll-aware, respects the timeout) */
static int _recv_exact(int fd, void *buf, size_t n, int timeout_ms) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return -1;
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static char *_recv_msg(vp_ipc_t *ipc, uint32_t *id_out, uint32_t *type_out,
                       int timeout_ms) {
    unsigned char hdr[16];
    if (_recv_exact(ipc->fd, hdr, sizeof(hdr), timeout_ms) != 0)
        return NULL;
    if (_rd32(hdr) != VP_IPC_MAGIC) return NULL;
    uint32_t id = _rd32(hdr + 4), type = _rd32(hdr + 8), len = _rd32(hdr + 12);
    if (len > 4u * 1024u * 1024u) return NULL;
    char *payload = malloc(len + 1);
    if (!payload) return NULL;
    if (len > 0 && _recv_exact(ipc->fd, payload, len, timeout_ms) != 0) {
        free(payload);
        return NULL;
    }
    payload[len] = 0;
    if (id_out) *id_out = id;
    if (type_out) *type_out = type;
    return payload;
}

vp_ipc_t *vp_ipc_connect(const char *path) {
    char pbuf[256];
    if (!path || !*path) {
        const char *rd = getenv("XDG_RUNTIME_DIR");
        if (!rd || !*rd) return NULL;
        snprintf(pbuf, sizeof(pbuf), "%s/vantage.sock", rd);
        path = pbuf;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path is 108 bytes; a longer runtime path cannot be
     * represented — fail loudly instead of silently truncating. */
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return NULL; }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    /* don't let a slow WM hang the panel UI */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    /* back to blocking for the request/response helpers */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    vp_ipc_t *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) { close(fd); return NULL; }
    ipc->fd = fd;
    snprintf(ipc->path, sizeof(ipc->path), "%s", path);
    return ipc;
}

void vp_ipc_free(vp_ipc_t *ipc) {
    if (!ipc) return;
    close(ipc->fd);
    free(ipc);
}

char *vp_ipc_call(vp_ipc_t *ipc, uint32_t msg_id, const char *payload,
                  int timeout_ms) {
    if (!ipc || ipc->fd < 0) return NULL;
    size_t plen = payload ? strlen(payload) : 0;
    unsigned char hdr[16];
    _le32(hdr, VP_IPC_MAGIC);
    _le32(hdr + 4, msg_id);
    _le32(hdr + 8, VP_IPC_REQUEST);
    _le32(hdr + 12, (uint32_t)plen);
    if (_send_all(ipc->fd, hdr, sizeof(hdr)) != 0) return NULL;
    if (plen > 0 && _send_all(ipc->fd, payload, plen) != 0) return NULL;
    /* responses may be interleaved with events: skip those */
    for (int tries = 0; tries < 64; tries++) {
        uint32_t id = 0, type = 0;
        char *p = _recv_msg(ipc, &id, &type, timeout_ms);
        if (!p) return NULL;
        if (type == VP_IPC_RESPONSE) return p;
        free(p);   /* event before the response — drop it (the poller
                      will re-derive state from a refresh) */
    }
    return NULL;
}

int vp_ipc_subscribe(vp_ipc_t *ipc) {
    if (!ipc || ipc->fd < 0) return -1;
    unsigned char hdr[16];
    _le32(hdr, VP_IPC_MAGIC);
    _le32(hdr + 4, VP_IPC_SUBSCRIBE);
    _le32(hdr + 8, VP_IPC_REQUEST);
    _le32(hdr + 12, 0);
    if (_send_all(ipc->fd, hdr, sizeof(hdr)) != 0) return -1;
    uint32_t id = 0, type = 0;
    char *p = _recv_msg(ipc, &id, &type, 2000);
    if (!p) return -1;
    free(p);
    ipc->subscribed = 1;
    return 0;
}

char *vp_ipc_poll_event(vp_ipc_t *ipc, uint32_t *msg_id) {
    if (!ipc || ipc->fd < 0) return NULL;
    struct pollfd pfd = { .fd = ipc->fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) != 1) return NULL;
    uint32_t id = 0, type = 0;
    char *p = _recv_msg(ipc, &id, &type, 0);
    if (!p) return NULL;
    if (type != VP_IPC_EVENT) {
        free(p);
        return NULL;
    }
    if (msg_id) *msg_id = id;
    return p;
}

int vp_ipc_fd(const vp_ipc_t *ipc) {
    return ipc ? ipc->fd : -1;
}
