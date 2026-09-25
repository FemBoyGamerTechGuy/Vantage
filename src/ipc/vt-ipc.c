/*
 * vt-ipc.c — Vantage internal IPC over Unix domain sockets
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wire format (little-endian):
 *   [u32 magic=0x56544352][u32 msg_id][u32 type][u32 len][len bytes payload]
 *
 * Server: socket(), bind(), listen(), accept() — supports multiple
 * simultaneous clients (panel, desktop, settings, CLI tools) via poll().
 * Events can be broadcast to every connected client (optionally only to
 * those that subscribed). Payloads are opaque bytes; Vantage components
 * use human-readable one-line text payloads for easy debugging.
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
#define VT_IPC_MAX_CLIENTS  16

typedef struct {
    uint32_t id;
    vt_ipc_handler_t h;
    void *ud;
} _handler_t;

typedef struct {
    int      fd;
    bool     subscribed;
    uint8_t *rbuf;          /* partial-frame scratch */
    size_t   rbuf_cap;
    size_t   rbuf_len;
} _client_t;

struct vt_ipc {
    int      fd;                 /* listen fd (server) or connection (client) */
    bool     is_server;
    char    *path;
    _handler_t handlers[VT_IPC_MAX_HANDLERS];
    size_t   n_handlers;
    _client_t clients[VT_IPC_MAX_CLIENTS];
    size_t   n_clients;
    pthread_mutex_t lock;
};

vt_ipc_t *vt_ipc_new_server(const char *path) {
    vt_ipc_t *ipc = vt_malloc0(sizeof(*ipc));
    if (!path) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%s", vt_runtime_dir(),
                 VT_IPC_DEFAULT_SOCKET);
        ipc->path = vt_strdup(buf);
    } else {
        ipc->path = vt_strdup(path);
    }
    ipc->is_server = true;
    ipc->fd = -1;
    pthread_mutex_init(&ipc->lock, NULL);
    return ipc;
}

vt_ipc_t *vt_ipc_new_client(const char *path) {
    vt_ipc_t *ipc = vt_malloc0(sizeof(*ipc));
    ipc->path = vt_strdup(path ? path :
        vt_strprintf("%s/%s", vt_runtime_dir(), VT_IPC_DEFAULT_SOCKET));
    ipc->is_server = false;
    ipc->fd = -1;
    pthread_mutex_init(&ipc->lock, NULL);
    return ipc;
}

void vt_ipc_free(vt_ipc_t *ipc) {
    if (!ipc) return;
    if (ipc->fd >= 0) close(ipc->fd);
    for (size_t i = 0; i < ipc->n_clients; i++) {
        if (ipc->clients[i].fd >= 0) close(ipc->clients[i].fd);
        vt_free(ipc->clients[i].rbuf);
    }
    if (ipc->is_server && ipc->path) unlink(ipc->path);
    vt_free(ipc->path);
    pthread_mutex_destroy(&ipc->lock);
    vt_free(ipc);
}

int vt_ipc_get_fd(const vt_ipc_t *ipc) {
    if (!ipc) return -1;
    if (ipc->is_server) return ipc->fd;
    return ipc->fd;
}

const char *vt_ipc_get_path(const vt_ipc_t *ipc) {
    return ipc ? ipc->path : NULL;
}

/* Encode/decode ---------------------------------------------------------- */
int vt_ipc_encode(const vt_ipc_msg_t *m, uint8_t **out, size_t *out_len) {
    if (!m || !out || !out_len) return VT_IPC_E_BADMAGIC;
    size_t total = 16 + m->len;
    uint8_t *buf = vt_malloc(total);
    uint32_t magic = VT_IPC_MAGIC;
    memcpy(buf + 0, &magic, 4);
    memcpy(buf + 4, &m->id, 4);
    memcpy(buf + 8, &m->type, 4);
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
    memcpy(&out->id, buf + 4, 4);
    memcpy(&out->type, buf + 8, 4);
    memcpy(&out->len, buf + 12, 4);
    if (out->len > 1024 * 1024) return VT_IPC_E_TRUNC;
    if (out->len > n - 16) return VT_IPC_E_TRUNC;
    if (out->len) {
        /* NUL-sentinel (not counted in len): handlers may treat the
         * payload as a C string — without this, parsers run off the
         * end of the heap chunk (undefined behaviour). */
        out->payload = vt_malloc(out->len + 1);
        memcpy(out->payload, buf + 16, out->len);
        out->payload[out->len] = 0;
    } else {
        out->payload = NULL;
    }
    return VT_IPC_OK;
}

int vt_ipc_register(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_handler_t h,
                    void *ud) {
    if (!ipc || !h) return VT_IPC_E_BADTYPE;
    pthread_mutex_lock(&ipc->lock);
    if (ipc->n_handlers >= VT_IPC_MAX_HANDLERS) {
        pthread_mutex_unlock(&ipc->lock);
        return VT_IPC_E_NOMEM;
    }
    ipc->handlers[ipc->n_handlers].id = msg_id;
    ipc->handlers[ipc->n_handlers].h = h;
    ipc->handlers[ipc->n_handlers].ud = ud;
    ipc->n_handlers++;
    pthread_mutex_unlock(&ipc->lock);
    return VT_IPC_OK;
}


/* Parse just the 16-byte header (payload read separately). */
static int _parse_hdr(const uint8_t *hdr, vt_ipc_msg_t *m) {
    uint32_t magic;
    memcpy(&magic, hdr + 0, 4);
    if (magic != VT_IPC_MAGIC) return VT_IPC_E_BADMAGIC;
    memcpy(&m->id, hdr + 4, 4);
    memcpy(&m->type, hdr + 8, 4);
    memcpy(&m->len, hdr + 12, 4);
    if (m->len > 1024 * 1024) return VT_IPC_E_TRUNC;
    m->payload = NULL;
    return VT_IPC_OK;
}

/* connection setup -------------------------------------------------------- */
static int _bind_listen(vt_ipc_t *ipc) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return VT_IPC_E_CONN;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->path);
    char *parent = vt_file_dirname(ipc->path);
    vt_file_mkdir_p(parent, 0755);
    vt_free(parent);
    unlink(ipc->path);
    mode_t old = umask(0);
    int rv = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old);
    if (rv < 0) { close(fd); return VT_IPC_E_CONN; }
    if (listen(fd, 8) < 0) { close(fd); return VT_IPC_E_CONN; }
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
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static ssize_t _send_all(int fd, const void *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t s = send(fd, (const char *)buf + total, n - total, 0);
        if (s < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)s;
    }
    return (ssize_t)total;
}

/* sending ----------------------------------------------------------------- */
int vt_ipc_send(vt_ipc_t *ipc, uint32_t msg_id, vt_ipc_msg_type_t type,
                const void *data, uint32_t len) {
    if (!ipc) return VT_IPC_E_CONN;
    if (ipc->fd < 0) {
        if (ipc->is_server) return VT_IPC_E_CONN;
        int rc = _connect(ipc);
        if (rc != VT_IPC_OK) return rc;
    }
    vt_ipc_msg_t m = { .id = msg_id, .type = type, .len = len,
                       .payload = (void *)data };
    uint8_t *buf; size_t n;
    if (vt_ipc_encode(&m, &buf, &n) != VT_IPC_OK) return VT_IPC_E_BADMAGIC;
    ssize_t s = _send_all(ipc->fd, buf, n);
    vt_free(buf);
    return s < 0 ? VT_IPC_E_CONN : VT_IPC_OK;
}

int vt_ipc_broadcast(vt_ipc_t *ipc, uint32_t msg_id, const void *data,
                     uint32_t len) {
    if (!ipc || !ipc->is_server) return VT_IPC_E_CONN;
    vt_ipc_msg_t m = { .id = msg_id, .type = VT_IPC_MSG_EVENT, .len = len,
                       .payload = (void *)data };
    uint8_t *buf; size_t n;
    if (vt_ipc_encode(&m, &buf, &n) != VT_IPC_OK) return VT_IPC_E_BADMAGIC;
    int sent = 0;
    for (size_t i = 0; i < ipc->n_clients; i++) {
        _client_t *c = &ipc->clients[i];
        if (c->fd < 0 || !c->subscribed) continue;
        if (_send_all(c->fd, buf, n) >= 0) sent++;
        else {
            close(c->fd);
            c->fd = -1; /* drop dead client */
        }
    }
    vt_free(buf);
    (void)sent;
    return VT_IPC_OK;
}

int vt_ipc_call(vt_ipc_t *ipc, uint32_t msg_id,
                const void *req, uint32_t req_len,
                vt_ipc_msg_t *resp, int timeout_ms) {
    if (!ipc) return VT_IPC_E_CONN;
    if (ipc->fd < 0) {
        if (ipc->is_server) return VT_IPC_E_CONN;
        int rc = _connect(ipc);
        if (rc != VT_IPC_OK) return rc;
    }
    if (vt_ipc_send(ipc, msg_id, VT_IPC_MSG_REQUEST, req, req_len) != VT_IPC_OK)
        return VT_IPC_E_CONN;
    struct pollfd pfd = { .fd = ipc->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return VT_IPC_E_POLL;
    uint8_t hdr[16];
    if (_recv_all(ipc->fd, hdr, 16) != 16) return VT_IPC_E_TRUNC;
    vt_ipc_msg_t m;
    int rc = _parse_hdr(hdr, &m);
    if (rc != VT_IPC_OK) return rc;
    if (m.len) {
        m.payload = vt_malloc(m.len + 1);
        if (_recv_all(ipc->fd, m.payload, m.len) != (ssize_t)m.len) {
            vt_free(m.payload);
            return VT_IPC_E_TRUNC;
        }
        m.payload[m.len] = 0;   /* NUL sentinel for string parsers */
    }
    if (m.type == VT_IPC_MSG_ERROR) {
        /* server rejected the request (no handler, or handler rc != 0) */
        vt_free(m.payload);
        return VT_IPC_E_HANDLER;
    }
    if (resp) *resp = m;
    else vt_free(m.payload);
    return VT_IPC_OK;
}

void vt_ipc_msg_free(vt_ipc_msg_t *m) {
    if (m) { vt_free(m->payload); m->payload = NULL; }
}

/* server dispatch --------------------------------------------------------- */
static vt_ipc_handler_t _find_handler(vt_ipc_t *ipc, uint32_t id, void **ud) {
    for (size_t i = 0; i < ipc->n_handlers; i++) {
        if (ipc->handlers[i].id == id) {
            if (ud) *ud = ipc->handlers[i].ud;
            return ipc->handlers[i].h;
        }
    }
    return NULL;
}

static void _drop_client(vt_ipc_t *ipc, size_t idx) {
    _client_t *c = &ipc->clients[idx];
    if (c->fd >= 0) close(c->fd);
    vt_free(c->rbuf);
    c->rbuf = NULL;
    c->rbuf_len = 0;
    /* compact */
    for (size_t j = idx; j + 1 < ipc->n_clients; j++)
        ipc->clients[j] = ipc->clients[j + 1];
    ipc->n_clients--;
}

/* read one full frame from a client, dispatch to handler, send response */
static int _serve_client(vt_ipc_t *ipc, _client_t *c) {
    uint8_t hdr[16];
    ssize_t r = recv(c->fd, hdr, 16, MSG_DONTWAIT);
    if (r == 0) return VT_IPC_E_CONN;             /* peer closed */
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return VT_IPC_E_CONN;
    }
    if (r < 16) {
        if (_recv_all(c->fd, hdr + r, (size_t)(16 - r)) != 16 - r)
            return VT_IPC_E_TRUNC;
    }
    vt_ipc_msg_t m;
    /* header-only parse: the payload is received separately below —
     * vt_ipc_decode() would reject any len > 0 when given only the
     * 16-byte header (out->len > n - 16). */
    int rc = _parse_hdr(hdr, &m);
    if (rc != VT_IPC_OK) { vt_loge("ipc: decode failed rc=%d", rc); return rc; }
    if (m.len) {
        m.payload = vt_malloc(m.len + 1);
        if (_recv_all(c->fd, m.payload, m.len) != (ssize_t)m.len) {
            vt_free(m.payload);
            vt_loge("ipc: payload trunc");
            return VT_IPC_E_TRUNC;
        }
        m.payload[m.len] = 0;   /* NUL sentinel for string parsers */
    }
    vt_logd("ipc: msg id=0x%x len=%u", m.id, m.len);
    if (m.id == VT_IPC_MSG_SUBSCRIBE) {
        c->subscribed = true;
        vt_free(m.payload);
        return VT_IPC_OK;
    }
    vt_ipc_msg_t resp = {0};
    void *ud = NULL;
    vt_ipc_handler_t h = _find_handler(ipc, m.id, &ud);
    vt_logd("ipc: handler=%s id=0x%x", h ? "found" : "none", m.id);
    if (h) {
        int hr = h(ipc, &m, &resp, ud);
        vt_logd("ipc: handler rc=%d resp_len=%u", hr, resp.len);
        if (hr == 0) {
            resp.id = m.id;
            resp.type = VT_IPC_MSG_RESPONSE;
            uint8_t *wbuf; size_t wlen;
            vt_ipc_encode(&resp, &wbuf, &wlen);
            _send_all(c->fd, wbuf, wlen);
            vt_free(wbuf);
            vt_free(resp.payload);
        } else {
            /* handler rejected the request (e.g. unknown window id):
             * answer with an ERROR frame so the client fails fast
             * instead of waiting for a timeout */
            vt_free(resp.payload);
            vt_ipc_msg_t err = { .id = m.id, .type = VT_IPC_MSG_ERROR, .len = 0 };
            uint8_t *wbuf; size_t wlen;
            vt_ipc_encode(&err, &wbuf, &wlen);
            _send_all(c->fd, wbuf, wlen);
            vt_free(wbuf);
        }
    } else {
        vt_ipc_msg_t err = { .id = m.id, .type = VT_IPC_MSG_ERROR, .len = 0 };
        uint8_t *wbuf; size_t wlen;
        vt_ipc_encode(&err, &wbuf, &wlen);
        _send_all(c->fd, wbuf, wlen);
        vt_free(wbuf);
    }
    vt_free(m.payload);
    return VT_IPC_OK;
}

int vt_ipc_step(vt_ipc_t *ipc, int timeout_ms) {
    if (!ipc) return VT_IPC_E_CONN;
    if (ipc->fd < 0 && ipc->is_server) {
        int rc = _bind_listen(ipc);
        if (rc != VT_IPC_OK) return rc;
    }
    if (!ipc->is_server) {
        /* client step: poll + dispatch handler for responses/events */
        if (ipc->fd < 0) {
            int rc = _connect(ipc);
            if (rc != VT_IPC_OK) return rc;
        }
        struct pollfd pfd = { .fd = ipc->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) return errno == EINTR ? 1 : VT_IPC_E_POLL;
        if (pr == 0) return 1;
        uint8_t hdr[16];
        ssize_t r = recv(ipc->fd, hdr, 16, MSG_DONTWAIT);
        if (r == 0) { close(ipc->fd); ipc->fd = -1; return VT_IPC_E_CONN; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return VT_IPC_E_CONN;
        }
        if (r < 16 &&
            _recv_all(ipc->fd, hdr + r, (size_t)(16 - r)) != 16 - r)
            return VT_IPC_E_TRUNC;
        vt_ipc_msg_t m;
        int rc = _parse_hdr(hdr, &m);
        if (rc != VT_IPC_OK) return rc;
        if (m.len) {
            m.payload = vt_malloc(m.len + 1);
            if (_recv_all(ipc->fd, m.payload, m.len) != (ssize_t)m.len) {
                vt_free(m.payload);
                return VT_IPC_E_TRUNC;
            }
            m.payload[m.len] = 0;   /* NUL sentinel for string parsers */
        }
        vt_ipc_msg_t resp = {0};
        void *ud = NULL;
        vt_ipc_handler_t h = _find_handler(ipc, m.id, &ud);
        if (h) {
            h(ipc, &m, &resp, ud);
            vt_free(resp.payload);
        }
        vt_free(m.payload);
        return VT_IPC_OK;
    }

    /* server: poll listen fd + all clients */
    struct pollfd pfds[VT_IPC_MAX_CLIENTS + 1];
    size_t idxmap[VT_IPC_MAX_CLIENTS + 1];
    size_t n = 0;
    pfds[n].fd = ipc->fd;
    pfds[n].events = POLLIN;
    n++;
    for (size_t i = 0; i < ipc->n_clients && n < VT_ARRAY_SIZE(pfds); i++) {
        if (ipc->clients[i].fd < 0) continue;
        pfds[n].fd = ipc->clients[i].fd;
        pfds[n].events = POLLIN | POLLHUP | POLLERR;
        idxmap[n] = i;
        n++;
    }
    int pr = poll(pfds, (nfds_t)n, timeout_ms);
    if (pr < 0) return errno == EINTR ? 1 : VT_IPC_E_POLL;
    if (pr == 0) return 1;

    if (pfds[0].revents & POLLIN) {
        int cfd = accept4(ipc->fd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd >= 0) {
            if (ipc->n_clients >= VT_IPC_MAX_CLIENTS) {
                close(cfd); /* too many clients */
            } else {
                _client_t *c = &ipc->clients[ipc->n_clients];
                memset(c, 0, sizeof(*c));
                c->fd = cfd;
                ipc->n_clients++;
                vt_logd("ipc: client connected (fd %d, %zu clients)",
                        cfd, ipc->n_clients);
            }
        }
    }
    /* iterate backwards: _serve_client may drop clients */
    for (size_t pi = n - 1; pi >= 1; pi--) {
        if (!(pfds[pi].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        size_t ci = idxmap[pi];
        if (ci >= ipc->n_clients) continue;
        _client_t *c = &ipc->clients[ci];
        int rc2 = _serve_client(ipc, c);
        if (rc2 == VT_IPC_E_CONN || rc2 == VT_IPC_E_TRUNC) {
            vt_logd("ipc: client dropped (fd %d)", c->fd);
            _drop_client(ipc, ci);
        }
    }
    return VT_IPC_OK;
}
