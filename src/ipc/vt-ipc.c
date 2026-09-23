/*
 * vt-ipc.c — Vantage internal IPC over Unix domain sockets
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wire format (little-endian):
 *   [u32 magic=0x56544352][u32 msg_id][u32 type][u32 len][len bytes payload]
 *
 * Server: socket(), bind(), listen(), accept() — single client supported
 *   but multi-client queued. Use select/poll for multiplexing.
 * Client: socket(), connect(), send/recv framed messages.
 *
 * D-Bus is NOT required; this is the canonical Vantage IPC mechanism.
 * D-Bus integration (if enabled at runtime) is layered on top in
 * src/integrations/vt-integ-dbus.c.
 */

#define VT_LOG_DOMAIN "ipc"
#include <vantage/vt-ipc.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <pthread.h>

#define VT_IPC_MAX_HANDLERS 256

typedef struct {
    uint32_t id;
    vt_ipc_handler_t h;
    void *ud;
} _handler_t;

struct vt_ipc {
    int      fd;
    bool     is_server;
    char    *path;
    _handler_t handlers[VT_IPC_MAX_HANDLERS];
    size_t   n_handlers;
    /* For server: connected client fd */
    int      client_fd;
    /* Scratch receive buffer for partial frames */
    uint8_t *rbuf;
    size_t   rbuf_cap;
    size_t   rbuf_len;
    pthread_mutex_t lock;
};

vt_ipc_t *vt_ipc_new_server(const char *path) {
    vt_ipc_t *ipc = vt_malloc0(sizeof(*ipc));
    if (!path) {
        /* default path under XDG_RUNTIME_DIR */
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%s", vt_runtime_dir(), VT_IPC_DEFAULT_SOCKET);
        ipc->path = vt_strdup(buf);
    } else {
        ipc->path = vt_strdup(path);
    }
    ipc->is_server = true;
    ipc->fd = -1;
    ipc->client_fd = -1;
    pthread_mutex_init(&ipc->lock, NULL);
    return ipc;
}

vt_ipc_t *vt_ipc_new_client(const char *path) {
    vt_ipc_t *ipc = vt_malloc0(sizeof(*ipc));
    ipc->path = vt_strdup(path ? path :
        vt_strprintf("%s/%s", vt_runtime_dir(), VT_IPC_DEFAULT_SOCKET));
    ipc->is_server = false;
    ipc->fd = -1;
    ipc->client_fd = -1;
    pthread_mutex_init(&ipc->lock, NULL);
    return ipc;
}

void vt_ipc_free(vt_ipc_t *ipc) {
    if (!ipc) return;
    if (ipc->fd >= 0) close(ipc->fd);
    if (ipc->client_fd >= 0) close(ipc->client_fd);
    if (ipc->is_server && ipc->path) {
        /* best-effort cleanup */
        unlink(ipc->path);
    }
    vt_free(ipc->path);
    vt_free(ipc->rbuf);
    pthread_mutex_destroy(&ipc->lock);
    vt_free(ipc);
}

int vt_ipc_get_fd(const vt_ipc_t *ipc) {
    if (!ipc) return -1;
    return ipc->client_fd >= 0 ? ipc->client_fd : ipc->fd;
}
const char *vt_ipc_get_path(const vt_ipc_t *ipc) { return ipc ? ipc->path : NULL; }

/* Encode/decode ---------------------------------------------------------- */
int vt_ipc_encode(const vt_ipc_msg_t *m, uint8_t **out, size_t *out_len) {
    if (!m || !out || !out_len) return VT_IPC_E_BADMAGIC;
    size_t total = 16 + m->len;
    uint8_t *buf = vt_malloc(total);
    uint32_t magic = VT_IPC_MAGIC;
    memcpy(buf +  0, &magic, 4);
    memcpy(buf +  4, &m->id, 4);
    memcpy(buf +  8, &m->type, 4);
    memcpy(buf + 12, &m->len, 4);
    if (m->len && m->payload) memcpy(buf + 16, m->payload, m->len);
    *out = buf;
    *out_len = total;
    return VT_IPC_OK;
}

int vt_ipc_decode(const uint8_t *buf, size_t n, vt_ipc_msg_t *out) {
    if (!buf || n < 16 || !out) return VT_IPC_E_TRUNC;
    uint32_t magic;
    memcpy(&magic, buf + 0, 4);
    if (magic != VT_IPC_MAGIC) return VT_IPC_E_BADMAGIC;
    memcpy(&out->id,   buf + 4, 4);
    memcpy(&out->type, buf + 8, 4);
    memcpy(&out->len,  buf + 12, 4);
    if (out->len > n - 16) return VT_IPC_E_TRUNC;
    if (out->len) {
        out->payload = vt_malloc(out->len);
        memcpy(out->payload, buf + 16, out->len);
    } else {
        out->payload = NULL;
    }
    return VT_IPC_OK;
}

int vt_ipc_register(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_handler_t h, void *ud) {
    if (!ipc || !h) return VT_IPC_E_BADTYPE;
    pthread_mutex_lock(&ipc->lock);
    if (ipc->n_handlers >= VT_IPC_MAX_HANDLERS) {
        pthread_mutex_unlock(&ipc->lock);
        return VT_IPC_E_NOMEM;
    }
    ipc->handlers[ipc->n_handlers].id = msg_id;
    ipc->handlers[ipc->n_handlers].h  = h;
    ipc->handlers[ipc->n_handlers].ud = ud;
    ipc->n_handlers++;
    pthread_mutex_unlock(&ipc->lock);
    return VT_IPC_OK;
}

static int _bind_listen(vt_ipc_t *ipc) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return VT_IPC_E_CONN;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->path);
    /* ensure parent dir exists */
    char *parent = vt_file_dirname(ipc->path);
    vt_file_mkdir_p(parent, 0755);
    vt_free(parent);
    unlink(ipc->path);
    mode_t old = umask(0);
    int rv = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (rv < 0) { close(fd); return VT_IPC_E_CONN; }
    if (listen(fd, 4) < 0) { close(fd); return VT_IPC_E_CONN; }
    ipc->fd = fd;
    return VT_IPC_OK;
}

static int _connect(vt_ipc_t *ipc) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return VT_IPC_E_CONN;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return VT_IPC_E_CONN;
    }
    ipc->fd = fd;
    return VT_IPC_OK;
}

static ssize_t _recv_all(int fd, void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char *)buf + total, n - total, 0);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return 0;
        total += r;
    }
    return (ssize_t)total;
}

static ssize_t _send_all(int fd, const void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t s = send(fd, (const char *)buf + total, n - total, 0);
        if (s < 0) { if (errno == EINTR) continue; return -1; }
        total += s;
    }
    return (ssize_t)total;
}

int vt_ipc_send(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_msg_type_t type,
                 const void *data, uint32_t len) {
    if (!ipc) return VT_IPC_E_CONN;
    int fd = ipc->client_fd >= 0 ? ipc->client_fd : ipc->fd;
    if (fd < 0) {
        if (ipc->is_server) return VT_IPC_E_CONN;
        int rc = _connect(ipc);
        if (rc != VT_IPC_OK) return rc;
        fd = ipc->fd;
    }
    vt_ipc_msg_t m = { .id = msg_id, .type = type, .len = len, .payload = (void *)data };
    uint8_t *buf; size_t n;
    if (vt_ipc_encode(&m, &buf, &n) != VT_IPC_OK) return VT_IPC_E_BADMAGIC;
    ssize_t s = _send_all(fd, buf, n);
    vt_free(buf);
    return s < 0 ? VT_IPC_E_CONN : VT_IPC_OK;
}

int vt_ipc_call(vt_ipc_t *ipc, uint32_t msg_id,
                 const void *req, uint32_t req_len,
                 vt_ipc_msg_t *resp, int timeout_ms) {
    if (!ipc) return VT_IPC_E_CONN;
    int fd = ipc->client_fd >= 0 ? ipc->client_fd : ipc->fd;
    if (fd < 0) {
        if (ipc->is_server) return VT_IPC_E_CONN;
        int rc = _connect(ipc);
        if (rc != VT_IPC_OK) return rc;
        fd = ipc->fd;
    }
    if (vt_ipc_send(ipc, msg_id, VT_IPC_MSG_REQUEST, req, req_len) != VT_IPC_OK)
        return VT_IPC_E_CONN;
    /* wait for response with timeout */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return VT_IPC_E_POLL;
    uint8_t hdr[16];
    if (_recv_all(fd, hdr, 16) != 16) return VT_IPC_E_TRUNC;
    vt_ipc_msg_t m;
    int rc = vt_ipc_decode(hdr, 16, &m);
    if (rc != VT_IPC_OK) return rc;
    if (m.len) {
        m.payload = vt_malloc(m.len);
        if (_recv_all(fd, m.payload, m.len) != (ssize_t)m.len) {
            vt_free(m.payload);
            return VT_IPC_E_TRUNC;
        }
    }
    if (resp) *resp = m;
    return VT_IPC_OK;
}

static vt_ipc_handler_t _find_handler(vt_ipc_t *ipc, uint32_t id, void **ud) {
    for (size_t i = 0; i < ipc->n_handlers; i++) {
        if (ipc->handlers[i].id == id) {
            if (ud) *ud = ipc->handlers[i].ud;
            return ipc->handlers[i].h;
        }
    }
    return NULL;
}

int vt_ipc_step(vt_ipc_t *ipc, int timeout_ms) {
    if (!ipc) return VT_IPC_E_CONN;
    /* On first call, lazy-bind/connect */
    if (ipc->fd < 0) {
        int rc = ipc->is_server ? _bind_listen(ipc) : _connect(ipc);
        if (rc != VT_IPC_OK) return rc;
    }
    int listen_fd = ipc->is_server ? ipc->fd : -1;
    int active_fd = ipc->client_fd >= 0 ? ipc->client_fd : (ipc->is_server ? -1 : ipc->fd);
    struct pollfd pfds[2];
    int n = 0;
    if (ipc->is_server) {
        pfds[n].fd = listen_fd; pfds[n].events = POLLIN; n++;
        if (active_fd >= 0) {
            pfds[n].fd = active_fd; pfds[n].events = POLLIN; n++;
        }
    } else {
        pfds[n].fd = ipc->fd; pfds[n].events = POLLIN; n++;
    }
    int pr = poll(pfds, n, timeout_ms);
    if (pr < 0) return errno == EINTR ? 1 : VT_IPC_E_POLL;
    if (pr == 0) return 1;  /* would-block / timeout */
    if (ipc->is_server && (pfds[0].revents & POLLIN)) {
        /* new client connection */
        int cfd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) return VT_IPC_E_CONN;
        if (ipc->client_fd >= 0) close(ipc->client_fd);
        ipc->client_fd = cfd;
        active_fd = cfd;
    }
    if (active_fd >= 0 && (pfds[ipc->is_server ? 1 : 0].revents & (POLLIN | POLLHUP | POLLERR))) {
        uint8_t hdr[16];
        ssize_t r = recv(active_fd, hdr, 16, MSG_DONTWAIT);
        if (r == 0) {
            close(active_fd);
            if (ipc->is_server) ipc->client_fd = -1;
            else ipc->fd = -1;
            return VT_IPC_E_CONN;
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return VT_IPC_E_CONN;
        }
        if (r < 16) {
            /* partial header — wait for the rest */
            ssize_t r2 = _recv_all(active_fd, hdr + r, 16 - r);
            if (r2 < 16 - r) return VT_IPC_E_TRUNC;
        }
        vt_ipc_msg_t m;
        int rc = vt_ipc_decode(hdr, 16, &m);
        if (rc != VT_IPC_OK) return rc;
        if (m.len) {
            m.payload = vt_malloc(m.len);
            if (_recv_all(active_fd, m.payload, m.len) != (ssize_t)m.len) {
                vt_free(m.payload);
                return VT_IPC_E_TRUNC;
            }
        }
        vt_ipc_msg_t resp = {0};
        void *ud = NULL;
        vt_ipc_handler_t h = _find_handler(ipc, m.id, &ud);
        if (h) {
            int hr = h(ipc, &m, &resp, ud);
            if (hr == 0 && resp.len) {
                uint8_t *wbuf; size_t wlen;
                vt_ipc_encode(&resp, &wbuf, &wlen);
                _send_all(active_fd, wbuf, wlen);
                vt_free(wbuf);
                vt_free(resp.payload);
            }
        } else {
            /* no handler — reply with error */
            vt_ipc_msg_t err = { .id = m.id, .type = VT_IPC_MSG_ERROR, .len = 0 };
            uint8_t *wbuf; size_t wlen;
            vt_ipc_encode(&err, &wbuf, &wlen);
            _send_all(active_fd, wbuf, wlen);
            vt_free(wbuf);
        }
        vt_free(m.payload);
    }
    return VT_IPC_OK;
}
