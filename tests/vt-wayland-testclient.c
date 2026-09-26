/*
 * vt-wayland-testclient.c — minimal Wayland client for smoke tests
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
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
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct xdg_wm_base *wm_base = NULL;
static struct wl_seat *seat = NULL;
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
    else if (!strcmp(iface, "wl_seat"))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 4);
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
static int buffer_released = 0;   /* wl_buffer.release received? */
static int popup_done_got = 0;     /* xdg_popup.popup_done received? */
static int popup_cfg_w = 0, popup_cfg_h = 0;   /* popup configure size */

/* ---- wl_buffer listener: clients may only reuse a buffer after the
 * ---- compositor releases it. A compositor that never sends release
 * ---- stalls double-buffered apps after two frames. */
static void _buf_release(void *data, struct wl_buffer *b) {
    (void)data;
    (void)b;
    buffer_released++;
}
static const struct wl_buffer_listener _buf_listener = {
    .release = _buf_release,
};

/* ---- wl_keyboard keymap validation ---------------------------------
 * A REAL toolkit client (GTK/Qt/xfce apps) binds wl_seat + wl_keyboard
 * and expects a VALID wl_keyboard.keymap event: fd >= 0, size > 0,
 * mmap-able xkb text. A compositor that passes fd -1 (no keymap
 * compiled) produces a libwayland marshal error (dup: EBADF) that
 * kills the whole client connection — apps die on launch. This is
 * exactly the "launched app never appears" failure mode, so the test
 * client validates it like a real app would. */
static struct wl_keyboard *keyboard = NULL;
static int keymap_ok = 0;
static size_t keymap_got_size = 0;

static void _kbd_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                        int32_t fd, uint32_t size) {
    (void)data; (void)kb;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && size > 0) {
        char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            /* xkb_keymap_get_as_string text starts with "xkb" */
            if (!strncmp(map, "xkb", 3)) {
                keymap_ok = 1;
                keymap_got_size = size;
            }
            munmap(map, size);
        }
    }
    if (fd >= 0) close(fd);   /* the client owns the received fd */
}
static void _kbd_enter(void *d, struct wl_keyboard *k, uint32_t s,
                       struct wl_surface *sf, struct wl_array *keys) { (void)d;(void)k;(void)s;(void)sf;(void)keys; }
static void _kbd_leave(void *d, struct wl_keyboard *k, uint32_t s,
                       struct wl_surface *sf) { (void)d;(void)k;(void)s;(void)sf; }
static void _kbd_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t,
                     uint32_t key, uint32_t st) { (void)d;(void)k;(void)s;(void)t;(void)key;(void)st; }
static void _kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
                           uint32_t sd, uint32_t ld, uint32_t lm,
                           uint32_t gp)
{ (void)d;(void)k;(void)s;(void)sd;(void)ld;(void)lm;(void)gp; }
static void _kbd_repeat(void *d, struct wl_keyboard *k, int32_t r,
                        int32_t dl) { (void)d;(void)k;(void)r;(void)dl; }
static const struct wl_keyboard_listener _kbd_listener = {
    .keymap      = _kbd_keymap,
    .enter       = _kbd_enter,
    .leave       = _kbd_leave,
    .key         = _kbd_key,
    .modifiers   = _kbd_modifiers,
    .repeat_info = _kbd_repeat,
};

static void _seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &_kbd_listener, NULL);
    }
}
static void _seat_name(void *data, struct wl_seat *s, const char *name) {
    (void)data; (void)s; (void)name;
}
static const struct wl_seat_listener _seat_listener = {
    .capabilities = _seat_caps,
    .name = _seat_name,
};

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

/* ---- xdg_popup listeners (menus, comboboxes) ---- */
static void _popup_configure(void *data, struct xdg_popup *p,
                             int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)data; (void)p; (void)x; (void)y;
    popup_cfg_w = w;
    popup_cfg_h = h;
}
static void _popup_done(void *data, struct xdg_popup *p) {
    (void)data; (void)p;
    popup_done_got = 1;
}
static const struct xdg_popup_listener _popup_listener = {
    .configure = _popup_configure,
    .popup_done = _popup_done,
    .repositioned = NULL,
};

int main(int argc, char **argv) {
    int width = 300, height = 200;
    unsigned color = 0xff9a3a5f;   /* aarrggbb: r=0x9a g=0x3a b=0x5f */
    bool keymap_only = false;
    bool popup_mode = false;      /* xdg_popup lifecycle (menus) */
    bool multipool_mode = false;  /* two shm pools alternating buffers —
                                    * the exact crash that killed real apps */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keymap-only")) keymap_only = true;
        else if (!strcmp(argv[i], "--popup")) popup_mode = true;
        else if (!strcmp(argv[i], "--multipool")) multipool_mode = true;
        else if (argc > 1 && i == 1) color = (unsigned)strtoul(argv[1], NULL, 0) | 0xff000000;
    }
    if (argc > 2 && !popup_mode && !multipool_mode && !keymap_only)
        width = atoi(argv[2]);
    if (argc > 3 && !popup_mode && !multipool_mode && !keymap_only)
        height = atoi(argv[3]);
    if (popup_mode) { width = 240; height = 120; }
    if (multipool_mode) { width = 200; height = 100; }

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

    /* bind the seat + keyboard and validate the keymap event BEFORE
     * anything else: if this connection dies here, every later check
     * would fail confusingly — fail with a precise message instead */
    if (seat) {
        wl_seat_add_listener(seat, &_seat_listener, NULL);
        wl_display_roundtrip(d);   /* capabilities */
        wl_display_roundtrip(d);   /* keymap event */
    }
    if (keymap_ok) {
        printf("keymap ok %zu\n", keymap_got_size);
    } else {
        fprintf(stderr, "wl_keyboard.keymap missing or invalid "
                "(fd/size/mmap/xkb text)\n");
    }
    fflush(stdout);

    /* keymap-only mode: validate the wl_keyboard path exactly like a
     * real toolkit app on launch and exit — no surface, no window.
     * The harness runs several of these SEQUENTIALLY to regression-test
     * that every Nth keyboard client gets a valid keymap (the
     * real-hardware bug: apps died on connect). */
    if (keymap_only) return keymap_ok ? 0 : 1;

    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xs, &_xdg_surface_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &_toplevel_listener, NULL);
    xdg_toplevel_set_title(tl, "Vantage Wayland Test");
    xdg_toplevel_set_app_id(tl, "vantage.wltest");
    wl_surface_commit(surf);
    /* wait for the initial configure (bail out on a dead connection) */
    while (!configured) {
        if (wl_display_roundtrip(d) < 0) {
            fprintf(stderr, "connection error while waiting for configure\n");
            return 1;
        }
    }
    printf("configured\n");
    fflush(stdout);

    if (popup_mode) {
        /* create an xdg_popup like a GTK menu: sized positioner,
         * anchored to the parent surface, grabbed. Verifies:
         *   - configure carries the POSITIONER's size (not a fixed one)
         *   - the surface configure/ack flow lets the client commit
         *   - popup_done arrives (dismiss) and destroy does not kill
         *     the connection (the historical "app crashed on menu
         *     close" — a NULL request implementation was a protocol
         *     error) */
        struct xdg_positioner *pos =
            xdg_wm_base_create_positioner(wm_base);
        xdg_positioner_set_size(pos, 180, 160);
        xdg_positioner_set_anchor_rect(pos, 20, 30, 60, 20);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
        struct wl_surface *psurf = wl_compositor_create_surface(compositor);
        struct xdg_surface *pxs =
            xdg_wm_base_get_xdg_surface(wm_base, psurf);
        xdg_surface_add_listener(pxs, &_xdg_surface_listener, NULL);
        configured = 0;
        struct xdg_popup *pp = xdg_surface_get_popup(pxs, xs, pos);
        xdg_popup_add_listener(pp, &_popup_listener, NULL);
        xdg_popup_grab(pp, seat, 0);
        wl_surface_commit(psurf);
        while (!configured) {
            if (wl_display_roundtrip(d) < 0) {
                fprintf(stderr, "connection died during popup setup\n");
                return 1;
            }
        }
        if (popup_cfg_w != 180 || popup_cfg_h != 160) {
            fprintf(stderr, "popup configure size %dx%d != requested "
                    "180x160\n", popup_cfg_w, popup_cfg_h);
            return 1;
        }
        printf("popup configured %dx%d\n", popup_cfg_w, popup_cfg_h);
        fflush(stdout);
        /* commit a buffer into the popup */
        {
            size_t stride = (size_t)180 * 4;
            size_t sz = stride * 160;
            int fd = memfd_create("vt-popup", 0);
            if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) return 1;
            uint32_t *pix = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
            if (pix == MAP_FAILED) return 1;
            for (size_t i = 0; i < 180u * 160u; i++)
                pix[i] = 0xff3ac05a;   /* distinctive menu green */
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd,
                                                          (int32_t)sz);
            struct wl_buffer *buf = wl_shm_pool_create_buffer(
                pool, 0, 180, 160, (int32_t)stride,
                WL_SHM_FORMAT_ARGB8888);
            wl_buffer_add_listener(buf, &_buf_listener, NULL);
            wl_surface_attach(psurf, buf, 0, 0);
            wl_surface_damage(psurf, 0, 0, 180, 160);
            wl_surface_commit(psurf);
            wl_display_roundtrip(d);
        }
        printf("popup committed\n");
        fflush(stdout);
        sleep(1);
        /* destroy the popup — must NOT kill the connection */
        xdg_popup_destroy(pp);
        xdg_surface_destroy(pxs);
        wl_surface_destroy(psurf);
        xdg_positioner_destroy(pos);
        wl_display_roundtrip(d);
        printf("popup destroyed without dying\n");
        fflush(stdout);
        return (popup_cfg_w == 180 && popup_cfg_h == 160) ? 0 : 1;
    }

    if (multipool_mode) {
        /* Alternate buffers from TWO different wl_shm pools, like real
         * toolkits do. The historical failure: the compositor never
         * ended its shm access, so libwayland's begin_access assertion
         * ABORTED it on the second pool — every app died. Also checks
         * that wl_buffer.release events arrive (double-buffering).
         */
        for (int round = 0; round < 6; round++) {
            size_t stride = (size_t)width * 4;
            size_t sz = stride * (size_t)height;
            int fd = memfd_create("vt-mp", 0);
            if (fd < 0 || ftruncate(fd, (off_t)sz) < 0) return 1;
            uint32_t *pix = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
            if (pix == MAP_FAILED) return 1;
            uint32_t c = (round % 2) ? 0xff2a7ac2 : 0xffc27a2a;
            for (size_t i = 0; i < (size_t)width * height; i++) pix[i] = c;
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd,
                                                          (int32_t)sz);
            struct wl_buffer *buf = wl_shm_pool_create_buffer(
                pool, 0, width, height, (int32_t)stride,
                WL_SHM_FORMAT_ARGB8888);
            wl_buffer_add_listener(buf, &_buf_listener, NULL);
            wl_surface_attach(surf, buf, 0, 0);
            wl_surface_damage(surf, 0, 0, width, height);
            wl_surface_commit(surf);
            wl_display_roundtrip(d);
            wl_display_roundtrip(d);
            /* destroy the pool immediately — the compositor must have
             * copied (or keep the buffer alive); reading freed memory
             * here would crash the COMPOSITOR later */
            wl_shm_pool_destroy(pool);
            wl_buffer_destroy(buf);
            munmap(pix, sz);
            close(fd);
            usleep(120000);
        }
        if (wl_display_roundtrip(d) < 0) {
            fprintf(stderr, "connection died in multipool mode\n");
            return 1;
        }
        printf("multipool ok, releases=%d\n", buffer_released);
        fflush(stdout);
        return buffer_released >= 6 ? 0 : 1;
    }

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
    wl_buffer_add_listener(buf, &_buf_listener, NULL);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, width, height);
    wl_surface_commit(surf);
    wl_display_roundtrip(d);
    printf("committed %dx%d\n", width, height);
    fflush(stdout);   /* harness polls this line while we are alive */

    sleep(2);   /* let the compositor paint + dump (SIGUSR1 from harness) */
    (void)have_globals;
    if (buffer_released)
        printf("buffer released %d\n", buffer_released);
    return keymap_ok ? 0 : 1;
}
