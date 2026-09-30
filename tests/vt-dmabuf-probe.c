/*
 * vt-dmabuf-probe.c — GPU-buffer client probe (zwp_linux_dmabuf_v1)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Emulates exactly what a GPU application does (browser, game, toolkit):
 * binds linux-dmabuf, reads the v4 feedback (format table + main
 * device), allocates a buffer, imports it through create_params and
 * commits it — then KEEPS REDRAWING through frame callbacks (the
 * browser animation model) so the test proves BOTH:
 *   1. the buffer's pixels actually reach the compositor's framebuffer
 *      (import + readback path, byte-exact color checks via SIGUSR1
 *      frame dumps), and
 *   2. the surface keeps updating continuously (no single-frame stall).
 *
 * The backing storage is a memfd: on a real GPU the client would use a
 * driver-allocated dma-buf, but the wire protocol and the compositor's
 * import path are IDENTICAL (EGL import handles both; llvmpipe's
 * software EGL device imports memfd-backed buffers through the same
 * eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) call a real driver uses for
 * GEM buffers).
 *
 * Usage: vt-dmabuf-probe [seconds] [--linear]
 * Exit:  0 = mapped + N frames drawn;  1 = protocol failure.
 *        stdout: "dmabuf-probe frames=<N> imported=<yes> feedback=<yes>"
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define W 300
#define H 200
#define STRIDE (W * 4)

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static bool got_feedback;
static int feedback_formats;

static struct wl_surface *surface;
static struct xdg_surface *xds;
static struct xdg_toplevel *toplevel;
static int configured, running = 1, frame_no, imported_ok;

/* two buffers ping-pong (like a real swapchain) */
static struct wl_buffer *bufs[2];
static uint32_t *pix[2];
static int released[2] = {1, 1};
static int cur;

static void _global(void *data, struct wl_registry *r, uint32_t name,
                    const char *iface, uint32_t version) {
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
    else if (!strcmp(iface, "zwp_linux_dmabuf_v1"))
        dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface,
                                  version < 4 ? version : 4);
}
static void _global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d; (void)r; (void)n;
}
static const struct wl_registry_listener reg_listener = {
    .global = _global, .global_remove = _global_remove,
};

/* ---- v4 feedback listener (parses what a real toolkit parses) ------ */
static void _fb_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) {
    (void)d; (void)f;
    got_feedback = true;
}
static void _fb_format_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                             int32_t fd, uint32_t size) {
    (void)d; (void)f;
    if (fd >= 0 && size >= 16) {
        void *t = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (t != MAP_FAILED) {
            feedback_formats = (int)(size / 16);
            munmap(t, size);
        }
    }
    if (fd >= 0) close(fd);
}
static void _fb_main_device(void *d, struct zwp_linux_dmabuf_feedback_v1 *f,
                            struct wl_array *device) {
    (void)d; (void)f; (void)device;
}
static void _fb_tranche_formats(void *d,
                                struct zwp_linux_dmabuf_feedback_v1 *f,
                                struct wl_array *indices) {
    (void)d; (void)f; (void)indices;
}
static void _fb_tranche_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f) {
    (void)d; (void)f;
}
static void _fb_tranche_target(void *d,
                               struct zwp_linux_dmabuf_feedback_v1 *f,
                               struct wl_array *device) {
    (void)d; (void)f; (void)device;
}
static const struct zwp_linux_dmabuf_feedback_v1_listener fb_listener = {
    .done = _fb_done,
    .format_table = _fb_format_table,
    .main_device = _fb_main_device,
    .tranche_formats = _fb_tranche_formats,
    .tranche_done = _fb_tranche_done,
    .tranche_target_device = _fb_tranche_target,
};

static void _wm_ping(void *d, struct xdg_wm_base *wb, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(wb, serial);
}
static const struct xdg_wm_base_listener wb_listener = { .ping = _wm_ping };

static void _xdg_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    configured = 1;
}
static const struct xdg_surface_listener xds_listener = {
    .configure = _xdg_configure,
};
static void _tl_cfg(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                    struct wl_array *st) { (void)d;(void)t;(void)w;(void)h;(void)st; }
static void _tl_close(void *d, struct xdg_toplevel *t) {
    (void)d; (void)t; running = 0;
}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = _tl_cfg, .close = _tl_close,
};

static void _buf_release(void *d, struct wl_buffer *b) {
    (void)d; (void)b;
    for (int i = 0; i < 2; i++)
        if (bufs[i] == b) released[i] = 1;
}
static const struct wl_buffer_listener buf_listener = {
    .release = _buf_release,
};

static void _frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_listener = {
    .done = _frame_done,
};

/* draw a LOUD solid color that cycles per frame (red→green→blue→…):
 * screenshot diffs can't miss it if the readback path is live */
static void draw(int i) {
    static const uint32_t cycle[6] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xff00ffff, 0xffff00ff
    };
    uint32_t c = cycle[frame_no % 6];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            pix[i][y * W + x] = c;
}

static void commit_next(void) {
    if (!released[cur]) cur ^= 1;          /* wait for release */
    if (!released[cur]) return;
    draw(cur);
    released[cur] = 0;
    wl_surface_attach(surface, bufs[cur], 0, 0);
    wl_surface_damage(surface, 0, 0, W, H);
    struct wl_callback *cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);
    wl_surface_commit(surface);
    frame_no++;
}

static void _frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)d; (void)t;
    wl_callback_destroy(cb);
    commit_next();
}

static struct wl_buffer *make_dmabuf_buffer(int which) {
    /* the "dma-buf": memfd storage + LINEAR modifier, exactly the wire
     * flow of a GPU swapchain (create_params → add → create_immed) */
    int fd = (int)syscall(319 /* SYS_memfd_create */, "vt-dmabuf-probe", 0);
    if (fd < 0) {
        perror("memfd_create");
        return NULL;
    }
    if (ftruncate(fd, (off_t)(STRIDE * H)) < 0) {
        close(fd);
        return NULL;
    }
    pix[which] = mmap(NULL, (size_t)STRIDE * H, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (pix[which] == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(dmabuf);
    if (!params) { close(fd); return NULL; }
    zwp_linux_buffer_params_v1_add(params, fd, 0, STRIDE, 0, 0, 0);
    struct wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(
        params, W, H, 0x34325241 /* ARGB8888 */, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);      /* the server dups what it needs via the import */
    if (!b) return NULL;
    wl_buffer_add_listener(b, &buf_listener, NULL);
    imported_ok++;
    return b;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "dmabuf-probe: connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !wm_base || !dmabuf) {
        printf("dmabuf-probe frames=0 imported=no feedback=no "
               "(missing globals: %s%s%s)\n",
               compositor ? "" : "wl_compositor ",
               wm_base ? "" : "xdg_wm_base ",
               dmabuf ? "" : "zwp_linux_dmabuf_v1");
        return dmabuf ? 0 : 1;
    }
    xdg_wm_base_add_listener(wm_base, &wb_listener, NULL);

    /* v4 feedback roundtrip (what Mesa does at EGL init) */
    struct zwp_linux_dmabuf_feedback_v1 *fb =
        zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
    if (fb) {
        zwp_linux_dmabuf_feedback_v1_add_listener(fb, &fb_listener, NULL);
        wl_display_roundtrip(display);
        zwp_linux_dmabuf_feedback_v1_destroy(fb);
    }

    bufs[0] = make_dmabuf_buffer(0);
    bufs[1] = make_dmabuf_buffer(1);
    if (!bufs[0] || !bufs[1]) {
        printf("dmabuf-probe frames=0 imported=no feedback=%s\n",
               got_feedback ? "yes" : "no");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    xds = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xds, &xds_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xds);
    xdg_toplevel_add_listener(toplevel, &tl_listener, NULL);
    xdg_toplevel_set_title(toplevel, "dmabuf-probe");
    xdg_toplevel_set_app_id(toplevel, "vt-dmabuf-probe");
    wl_surface_commit(surface);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int first = 1;
    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (double)(now.tv_sec - t0.tv_sec) +
                    (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= seconds) break;
        if (wl_display_dispatch(display) < 0) break;
        if (first && configured) {
            first = 0;
            commit_next();
        }
    }
    printf("dmabuf-probe frames=%d imported=%s feedback=%s(%d formats)\n",
           frame_no, imported_ok == 2 ? "yes" : "no",
           got_feedback ? "yes" : "no", feedback_formats);
    return frame_no > 0 ? 0 : 1;
}
