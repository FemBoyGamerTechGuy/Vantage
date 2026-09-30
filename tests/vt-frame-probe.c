/*
 * vt-frame-probe.c — frame-callback-driven animating client (browser model)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Draws a new color every frame in a loop driven by wl_surface.frame
 * callbacks — exactly how browsers/toy-toolkits pace redraws. Events are
 * traced into a shared-mmap file with ZERO syscalls so the timing is not
 * perturbed (printf-based tracing HID the frame-callback freeze bug):
 *   C = commit sent        R = frame callback done received
 *   r = buffer release     P = poll woke readable
 *   c = configure acked
 * The color cycles per frame so screenshots differ while animating.
 *
 * Usage: vt-frame-probe [seconds]
 * Env:   VANTAGE_FRAME_TRACE=/path  (mmap trace, same protocol as the
 *       compositor-side tracer; use a DIFFERENT file for the client)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

/* ---- zero-syscall mmap tracer (mirrors the compositor's) ----------- */
static char *ft_map = NULL;
static size_t ft_pos = 0, ft_cap = 0;
static void ft_init(void) {
    const char *p = getenv("VANTAGE_FRAME_TRACE");
    if (!p || !*p) return;
    int fd = open(p, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    if (ftruncate(fd, 1 << 18) < 0) { close(fd); return; }
    ft_map = mmap(NULL, 1 << 18, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ft_map == MAP_FAILED) { ft_map = NULL; close(fd); return; }
    ft_cap = 1 << 18;
    close(fd);
}
static inline void ft(char c) {
    static int inited = 0;
    if (!inited) { inited = 1; ft_init(); }
    if (ft_map && ft_pos + 1 < ft_cap) {
        ft_map[ft_pos++] = c;
        ft_map[ft_pos] = '\n';
    }
}

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_surface *xds;
static struct xdg_toplevel *toplevel;
static int configured;
static int running = 1;
static int frame_no;
static uint32_t color_phase;

#define W 300
#define H 200

static struct wl_buffer *bufA, *bufB;
static uint32_t *pixA, *pixB;
static struct wl_buffer *cur;
static int a_released = 1, b_released = 1;

static void draw(uint32_t *pix) {
    uint32_t base = 0xff000000;
    uint8_t r = (uint8_t)(color_phase), g = (uint8_t)(color_phase >> 8),
            b = (uint8_t)(color_phase >> 16);
    for (int i = 0; i < W * H; i++) pix[i] = base | (r << 16) | (g << 8) | b;
    color_phase += 0x010101;
}

static void _frame_done(void *data, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = {
    .done = _frame_done,
};

static void _buf_release(void *data, struct wl_buffer *b) {
    (void)b;
    if (data == (void *)0) a_released = 1;
    else b_released = 1;
    ft('r');
}
static const struct wl_buffer_listener buf_listener = {
    .release = _buf_release,
};

static void commit_next(void) {
    /* pick a free buffer */
    if (a_released) { draw(pixA); cur = bufA; a_released = 0; }
    else if (b_released) { draw(pixB); cur = bufB; b_released = 0; }
    else return;                    /* both in flight: wait for release */
    wl_surface_attach(surface, cur, 0, 0);
    wl_surface_damage(surface, 0, 0, W, H);
    struct wl_callback *cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_commit(surface);
    frame_no++;
    ft('C');
}

static void _frame_done(void *data, struct wl_callback *cb, uint32_t t) {
    (void)data; (void)t;
    wl_callback_destroy(cb);
    ft('R');
    commit_next();
}

static void _xdg_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    configured = 1;
    ft('c');
}
static const struct xdg_surface_listener xds_listener = {
    .configure = _xdg_configure,
};
static void _toplevel_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *st) { (void)d;(void)t;(void)w;(void)h;(void)st; }
static void _toplevel_close(void *d, struct xdg_toplevel *t) {
    (void)d; (void)t; running = 0;
}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = _toplevel_cfg,
    .close = _toplevel_close,
};
static void _wm_ping(void *d, struct xdg_wm_base *wb, uint32_t serial) {
    (void)d; xdg_wm_base_pong(wb, serial);
}
static const struct xdg_wm_base_listener wb_listener = { .ping = _wm_ping };

static void _global(void *data, struct wl_registry *r, uint32_t name,
                    const char *iface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
}
static void _global_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)d; (void)r; (void)name;
}
static const struct wl_registry_listener reg_listener = {
    .global = _global, .global_remove = _global_remove,
};

static struct wl_buffer *make_shm(uint32_t **pix_out, int which) {
    int stride = W * 4;
    size_t size = (size_t)stride * H;
    char path[] = "/tmp/vt-frame-probe-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return NULL; }
    uint32_t *pix = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) { close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(
        pool, 0, W, H, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &buf_listener, (void *)(long)which);
    *pix_out = pix;
    return b;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "missing globals\n"); return 1;
    }
    xdg_wm_base_add_listener(wm_base, &wb_listener, NULL);

    bufA = make_shm(&pixA, 0);
    bufB = make_shm(&pixB, 1);
    surface = wl_compositor_create_surface(compositor);
    xds = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xds, &xds_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xds);
    xdg_toplevel_add_listener(toplevel, &tl_listener, NULL);
    xdg_toplevel_set_title(toplevel, "frame-probe");
    xdg_toplevel_set_app_id(toplevel, "vt-frame-probe");
    wl_surface_commit(surface);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int first = 1;
    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (double)(now.tv_sec - t0.tv_sec) +
                    (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= seconds) break;
        (void)el;
        /* blocking dispatch like the toy clients */
        if (wl_display_dispatch(display) < 0) break;
        if (first && configured) {
            first = 0;
            commit_next();
        }
    }
    printf("frames=%d\n", frame_no);
    return 0;
}
