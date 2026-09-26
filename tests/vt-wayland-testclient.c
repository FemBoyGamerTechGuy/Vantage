/*
 * vt-wayland-testclient.c — minimal Wayland client for smoke tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Connects to the Vantage Wayland compositor, creates an xdg_toplevel,
 * fills a shm buffer with a known color pattern and commits. Used to
 * validate the compositor's surface/shm/xdg-shell paths; the compositor
 * framebuffer is dumped via SIGUSR1 and pixel-checked by the harness.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base *wm_base = NULL;
static int have_globals = 0;

static void _registry_global(void *data, struct wl_registry *r, uint32_t name,
                             const char *iface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 3);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
}

static void _registry_global_remove(void *data, struct wl_registry *r,
                                    uint32_t name) {
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener _registry_listener = {
    .global = _registry_global,
    .global_remove = _registry_global_remove,
};

static void _wm_base_ping(void *data, struct xdg_wm_base *wb, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wb, serial);
}
static const struct xdg_wm_base_listener _wm_base_listener = {
    .ping = _wm_base_ping,
};

static int configured = 0;
static void _xdg_surface_configure(void *data, struct xdg_surface *xs,
                                   uint32_t serial) {
    (void)data; (void)serial;
    configured++;
    xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener _xdg_surface_listener = {
    .configure = _xdg_surface_configure,
};

static void _toplevel_configure(void *data, struct xdg_toplevel *t,
                                int32_t w, int32_t h, struct wl_array *st) {
    (void)data; (void)t; (void)w; (void)h; (void)st;
}
static void _toplevel_close(void *data, struct xdg_toplevel *t) {
    (void)data; (void)t;
}
static const struct xdg_toplevel_listener _toplevel_listener = {
    .configure = _toplevel_configure,
    .close = _toplevel_close,
};

int main(int argc, char **argv) {
    int width = 300, height = 200;
    unsigned color = 0xff9a3a5f;   /* aarrggbb: r=0x9a g=0x3a b=0x5f */
    if (argc > 1) color = (unsigned)strtoul(argv[1], NULL, 0) | 0xff000000;
    if (argc > 2) width = atoi(argv[2]);
    if (argc > 3) height = atoi(argv[3]);

    struct wl_display *d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "cannot connect to wayland display\n"); return 1; }
    printf("connected\n");
    fflush(stdout);

    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &_registry_listener, NULL);
    wl_display_roundtrip(d);
    if (!compositor || !shm || !wm_base) {
        fprintf(stderr, "missing globals (compositor=%p shm=%p xdg=%p)\n",
                (void *)compositor, (void *)shm, (void *)wm_base);
        return 1;
    }
    xdg_wm_base_add_listener(wm_base, &_wm_base_listener, NULL);
    have_globals = 1;

    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xs, &_xdg_surface_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &_toplevel_listener, NULL);
    xdg_toplevel_set_title(tl, "Vantage Wayland Test");
    xdg_toplevel_set_app_id(tl, "vantage.wltest");
    wl_surface_commit(surf);
    /* wait for the initial configure */
    while (!configured) wl_display_roundtrip(d);
    printf("configured\n");
    fflush(stdout);

    /* shm buffer with a solid color */
    size_t stride = (size_t)width * 4;
    size_t sz = stride * (size_t)height;
    int fd = memfd_create("vt-test", 0);
    if (fd < 0) return 1;
    if (ftruncate(fd, (off_t)sz) < 0) return 1;
    uint32_t *pix = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) return 1;
    for (size_t i = 0; i < (size_t)width * height; i++)
        pix[i] = color;   /* ARGB8888 little-endian = 0xAARRGGBB */
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0,
        width, height, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, width, height);
    wl_surface_commit(surf);
    wl_display_roundtrip(d);
    printf("committed %dx%d\n", width, height);
    fflush(stdout);   /* harness polls this line while we are alive */

    sleep(2);   /* let the compositor paint + dump (SIGUSR1 from harness) */
    (void)have_globals;
    return 0;
}
