/*
 * vt-wl-dmabuf.c — zwp_linux_dmabuf_v1 (v1..v4): GPU client buffers
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * THE GPU-acceleration path for Wayland clients:
 *
 *   client (browser/game/toolkit) → its own EGL/Vulkan context on the
 *     GPU driver → swapchain buffers as dma-bufs → zwp_linux_dmabuf_v1
 *     → wl_surface.attach(wl_buffer) → WE import (EGLImage on the same
 *     driver) → readback into the compositor's CPU framebuffer →
 *     wl_buffer.release immediately (the client never stalls).
 *
 * The application's rendering is REAL GPU rendering — its GL/Vulkan
 * calls execute on the actual hardware driver; only the final pixels
 * cross into the compositor's scanout path. This also unblocks
 * Xwayland glamor (hardware GLX for X11 apps in the Wayland session):
 * with this global present, Xwayland's glamor backend initializes
 * instead of logging "GBM Wayland interfaces not available" and
 * falling back to software.
 *
 * Import ladder per buffer:
 *   1. EGL import (vt-dmabuf-egl): real driver GEM import, handles
 *      tiled/compressed modifiers, readback via FBO + glReadPixels.
 *   2. mmap fallback: LINEAR buffers are CPU-mappable — read rows
 *      directly (also the path exercised by headless tests with
 *      memfd-backed buffers).
 *   3. Neither → params.failed (v3) / zombie buffer (v1/v2): the
 *      client falls back to wl_shm.
 *
 * v4 feedback: get_default_feedback / get_surface_feedback answer
 * with the render node as main_device (dev_t 0 on the software/headless
 * display) and one tranche advertising ARGB/XRGB/ABGR/XBGR8888 with
 * LINEAR + INVALID modifiers — INVALID lets the driver pick its
 * optimal layout, which the EGL import then handles.
 */
#define VT_LOG_DOMAIN "wl-dmabuf"
#include <vantage/vt-core.h>
#include "vt-wl-internal.h"
#include "linux-dmabuf-unstable-v1-protocol.h"
#include <vantage/vt-dmabuf-egl.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <drm_fourcc.h>

/* ------------------------------------------------------------------ */
#define _DMABUF_VERSION 4

#define VT_FOURCC_TAG(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))

/* the formats our import + CPU blit path handle. Toolkits' SHM path is
 * unchanged (wl_shm); these are what GPU swapchains use with us. */
static const uint32_t _formats[] = {
    VT_FOURCC_TAG('A', 'R', '2', '4'),      /* ARGB8888 */
    VT_FOURCC_TAG('X', 'R', '2', '4'),      /* XRGB8888 */
    VT_FOURCC_TAG('A', 'B', '2', '4'),      /* ABGR8888 */
    VT_FOURCC_TAG('X', 'B', '2', '4'),      /* XBGR8888 */
};
#define _N_FORMATS (int)(sizeof(_formats) / sizeof(_formats[0]))

typedef struct _dmabuf_plane {
    int      fd;
    uint32_t offset;
    uint32_t stride;
} _dmabuf_plane_t;

typedef struct _dmabuf_params {
    struct wl_resource *res;            /* zwp_linux_dmabuf_params_v1 */
    _dmabuf_plane_t     planes[4];
    int                 n_planes;
    uint64_t            modifier;
    bool                used;           /* create/create_immed ran */
} _dmabuf_params_t;

#define _DMABUF_BUF_MAGIC 0x5a6dbafb
typedef struct _dmabuf_buf {
    uint32_t            magic;
    uint32_t            w, h, fourcc;
    bool                y_invert;       /* ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT */
    /* import handle: exactly one of these is active */
    vt_dmabuf_image_t  *img;            /* EGL import */
    struct {
        void           *map;
        size_t          len;
        uint32_t        stride;
        int             fd;
    } mm;
    struct wl_resource *res;            /* wl_buffer */
    struct wl_list      link;           /* st->dmabuf_bufs */
} _dmabuf_buf_t;

/* extra state parked on _wl_state_t via a side list owned here */
static struct {
    struct wl_global *global;
    struct wl_list    bufs;             /* _dmabuf_buf_t */
    long              n_imported;
    long              n_mmapped;
    long              n_failed;
    /* advertisement capability (self-tested by the import engine):
     * only layouts this machine can actually import are promised to
     * clients — an unimportable advertisement turns every GPU client
     * into a black window (the NVIDIA regression) */
    bool              adv_linear;
    bool              adv_invalid;
} _D = {0};

/* --------------------------------------------------------- feedback */
static void _feedback_send_formats(struct wl_resource *fb_res) {
    /* v4 feedback contract: a format TABLE (fd, 16-byte entries {u32
     * fourcc, u32 pad, u64 modifier}), then main_device + one tranche
     * whose tranche_formats references the table by 16-bit indices.
     * ONLY self-tested-capable (format, modifier) combos are listed. */
    int n_entries = 0;
    for (int i = 0; i < _N_FORMATS; i++) {
        if (_D.adv_invalid) n_entries++;
        if (_D.adv_linear) n_entries++;
    }
    size_t table_size = (size_t)n_entries * 16;
    int table_fd = -1;
    uint8_t *table = NULL;
#ifdef SYS_memfd_create
    table_fd = (int)syscall(SYS_memfd_create, "vt-dmabuf-fmt", 0);
#endif
    if (table_fd < 0) {
        char path[] = "/tmp/vt-dmabuf-fmt-XXXXXX";
        table_fd = mkstemp(path);
        if (table_fd >= 0) unlink(path);
    }
    if (table_fd < 0) return;
    if (ftruncate(table_fd, (off_t)table_size) < 0) {
        close(table_fd);
        return;
    }
    table = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                 table_fd, 0);
    if (table == MAP_FAILED) { close(table_fd); return; }
    {
        int e = 0;
        for (int i = 0; i < _N_FORMATS; i++) {
            struct { uint32_t fmt, pad; uint64_t mod; } v;
            v.fmt = _formats[i];
            v.pad = 0;
            if (_D.adv_invalid) {
                v.mod = DRM_FORMAT_MOD_INVALID;
                memcpy(table + (size_t)e * 16, &v, 16);
                e++;
            }
            if (_D.adv_linear) {
                v.mod = DRM_FORMAT_MOD_LINEAR;
                memcpy(table + (size_t)e * 16, &v, 16);
                e++;
            }
        }
    }
    munmap(table, table_size);
    lseek(table_fd, 0, SEEK_SET);
    zwp_linux_dmabuf_feedback_v1_send_format_table(fb_res, table_fd,
                                                    (uint32_t)table_size);
    close(table_fd);

    dev_t dev = vt_dmabuf_egl_device();
    uint8_t devbuf[8];
    memset(devbuf, 0, sizeof(devbuf));
    memcpy(devbuf, &dev, sizeof(dev) < 8 ? sizeof(dev) : 8);
    struct wl_array dev_arr = { .size = 8, .data = devbuf };
    zwp_linux_dmabuf_feedback_v1_send_main_device(fb_res, &dev_arr);

    /* one tranche on the same device */
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb_res,
                                                            &dev_arr);
    uint16_t idx[_N_FORMATS * 2];
    for (int i = 0; i < n_entries; i++) idx[i] = (uint16_t)i;
    struct wl_array idx_arr = { .size = (size_t)n_entries * sizeof(uint16_t),
                                .data = idx };
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb_res, &idx_arr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb_res);
    zwp_linux_dmabuf_feedback_v1_send_done(fb_res);
}

static void _fb_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct zwp_linux_dmabuf_feedback_v1_interface _fb_impl = {
    .destroy = _fb_destroy,
};

static void _get_default_feedback(struct wl_client *cli,
                                  struct wl_resource *res, uint32_t id);
static void _get_surface_feedback(struct wl_client *cli,
                                  struct wl_resource *res, uint32_t id,
                                  struct wl_resource *surface);
static void _mgr_destroy(struct wl_client *cli, struct wl_resource *res);
static void _mgr_create_params(struct wl_client *cli,
                               struct wl_resource *res, uint32_t id);
static const struct zwp_linux_dmabuf_v1_interface _mgr_impl = {
    .destroy = _mgr_destroy,
    .create_params = _mgr_create_params,
    .get_default_feedback = _get_default_feedback,
    .get_surface_feedback = _get_surface_feedback,
};

/* ------------------------------------------------------------ params */
static void _params_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _buf_destroy_req(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static const struct wl_buffer_interface _buf_impl = {
    .destroy = _buf_destroy_req,
};

static void _params_add(struct wl_client *cli, struct wl_resource *res,
                        int32_t fd, uint32_t offset, uint32_t stride,
                        uint32_t modifier_hi, uint32_t modifier_lo,
                        uint32_t plane_idx) {
    _dmabuf_params_t *p = wl_resource_get_user_data(res);
    if (!p) { close(fd); return; }
    if (plane_idx >= 4 || p->n_planes >= 4 || fd < 0) {
        close(fd);
        wl_resource_post_error(res,
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
            "invalid plane index or fd");
        return;
    }
    p->planes[plane_idx].fd = fd;      /* we own it from here */
    p->planes[plane_idx].offset = offset;
    p->planes[plane_idx].stride = stride;
    if ((int)plane_idx >= p->n_planes) p->n_planes = (int)plane_idx + 1;
    p->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
}

/* import ladder: EGL first, mmap (linear only) second */
static _dmabuf_buf_t *_buffer_import(uint32_t w, uint32_t h, uint32_t fourcc,
                                     const _dmabuf_plane_t *planes,
                                     int n_planes, uint64_t modifier) {
    _dmabuf_buf_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->magic = _DMABUF_BUF_MAGIC;
    b->w = w;
    b->h = h;
    b->fourcc = fourcc;

    if (vt_dmabuf_egl_available()) {
        int fds[4] = {0};
        uint32_t offs[4] = {0}, strides[4] = {0};
        for (int i = 0; i < n_planes && i < 4; i++) {
            fds[i] = planes[i].fd;
            offs[i] = planes[i].offset;
            strides[i] = planes[i].stride;
        }
        b->img = vt_dmabuf_import(fds, offs, strides, modifier, w, h,
                                  fourcc, n_planes);
        if (b->img) {
            _D.n_imported++;
            return b;
        }
    }

    /* mmap fallback: single LINEAR plane only. An INVALID modifier
     * means "driver layout" — that is linear-in-memory ONLY on the
     * software device (memfd storage); mmapping a tiled real-GPU
     * buffer would decode garbage rows. (VANTAGE_DMABUF_ZOMBIE skips
     * this too — the hook reproduces a machine where NOTHING imports,
     * e.g. NVIDIA GEM without mmap support.) */
    if (!getenv("VANTAGE_DMABUF_ZOMBIE") &&
        n_planes == 1 &&
        (modifier == DRM_FORMAT_MOD_LINEAR ||
         (modifier == DRM_FORMAT_MOD_INVALID &&
          vt_dmabuf_egl_is_software())) &&
        planes[0].stride >= w * 4 && planes[0].fd >= 0) {
        size_t len = (size_t)planes[0].offset +
                     (size_t)planes[0].stride * (size_t)h;
        void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, planes[0].fd, 0);
        if (map != MAP_FAILED) {
            b->mm.map = map;
            b->mm.len = len;
            b->mm.stride = planes[0].stride;
            b->mm.fd = -1;             /* caller owns fd lifecycle */
            _D.n_mmapped++;
            return b;
        }
        vt_logd("dmabuf: mmap failed (%s)", strerror(errno));
    }
    _D.n_failed++;
    if (_D.n_failed <= 6)
        vt_logw("dmabuf: buffer import FAILED (%.4s %ux%u mod 0x%llx) — "
                " EGL=%s mmap=%s", (const char *)&fourcc, w, h,
                (unsigned long long)modifier,
                vt_dmabuf_egl_available() ? "tried" : "unavailable",
                strerror(errno));
    free(b);
    return NULL;
}

static void _buf_res_destroy(struct wl_resource *res);

/* readback into an ARGB8888 destination (stride in uint32 units) */
static bool _buffer_readback(_dmabuf_buf_t *b, uint32_t *dest,
                             int dest_stride_u32) {
    if (!b || b->magic != _DMABUF_BUF_MAGIC || !dest) return false;
    if (b->img)
        return vt_dmabuf_readback_argb(b->img, dest, dest_stride_u32,
                                       b->y_invert);
    if (b->mm.map) {
        const uint8_t *src = (const uint8_t *)b->mm.map;
        bool swap_rb = (b->fourcc == DRM_FORMAT_ABGR8888 ||
                        b->fourcc == DRM_FORMAT_XBGR8888);
        bool force_a = (b->fourcc == DRM_FORMAT_XRGB8888 ||
                        b->fourcc == DRM_FORMAT_XBGR8888);
        for (uint32_t y = 0; y < b->h; y++) {
            uint32_t sy = b->y_invert ? (b->h - 1 - y) : y;
            const uint32_t *srow = (const uint32_t *)
                (src + (size_t)sy * b->mm.stride);
            uint32_t *drow = dest + (size_t)y * (size_t)dest_stride_u32;
            for (uint32_t x = 0; x < b->w; x++) {
                uint32_t px = srow[x];
                if (swap_rb)
                    px = (px & 0xff00ff00) |
                         ((px & 0x000000ff) << 16) |
                         ((px & 0x00ff0000) >> 16);
                if (force_a) px |= 0xff000000;
                drow[x] = px;
            }
        }
        return true;
    }
    return false;
}

/* create: async-style — the wl_buffer id is SERVER-allocated and
 * delivered via the `created` event (v3+). v1/v2 callers get no
 * feedback (the protocol had none); modern clients all use v3+. */
static void _params_create(struct wl_client *cli, struct wl_resource *res,
                           int32_t w, int32_t h, uint32_t format,
                           uint32_t flags) {
    _dmabuf_params_t *p = wl_resource_get_user_data(res);
    uint32_t v = wl_resource_get_version(res);
    if (!p || p->used) {
        wl_resource_post_error(res,
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
            "params already used");
        return;
    }
    p->used = true;
    if (w <= 0 || h <= 0 || p->n_planes < 1) {
        wl_resource_post_error(res,
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
            "zero size or no planes");
        return;
    }
    _dmabuf_buf_t *b = _buffer_import((uint32_t)w, (uint32_t)h, format,
                                      p->planes, p->n_planes, p->modifier);
    if (!b) {
        vt_logw("dmabuf: create %.4s %dx%d import failed — client falls "
                "back", (const char *)&format, w, h);
        if (v >= ZWP_LINUX_BUFFER_PARAMS_V1_FAILED_SINCE_VERSION)
            zwp_linux_buffer_params_v1_send_failed(res);
        return;
    }
    b->y_invert = !!(flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);
    struct wl_resource *br = wl_resource_create(cli, &wl_buffer_interface,
                                                1, 0);   /* server id */
    if (!br) {
        wl_client_post_no_memory(cli);
        if (b->img) vt_dmabuf_image_free(b->img);
        if (b->mm.map) munmap(b->mm.map, b->mm.len);
        free(b);
        return;
    }
    wl_resource_set_implementation(br, &_buf_impl, b, _buf_res_destroy);
    b->res = br;
    wl_list_insert(_D.bufs.prev, &b->link);
    if (v >= ZWP_LINUX_BUFFER_PARAMS_V1_CREATED_SINCE_VERSION)
        zwp_linux_buffer_params_v1_send_created(res, br);
    vt_logd("dmabuf: created buffer %p %.4s %dx%d mod 0x%llx (%s)",
            (void *)br, (const char *)&format, w, h,
            (unsigned long long)p->modifier,
            b->img ? "EGL" : "mmap");
}

/* create_immed: the client gave the buffer id — ALWAYS create the
 * resource. A failed import becomes a ZOMBIE that still carries the
 * buffer's layout (w/h/fourcc): the surface can map (empty content)
 * instead of vanishing, attach is a no-op + instant release, and the
 * client's proxy resolves so it can keep cycling buffers. */
static void _params_create_immed(struct wl_client *cli,
                                 struct wl_resource *res, uint32_t id,
                                 int32_t w, int32_t h, uint32_t format,
                                 uint32_t flags) {
    _dmabuf_params_t *p = wl_resource_get_user_data(res);
    if (!p || p->used) {
        wl_resource_post_error(res,
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
            "params already used");
        return;
    }
    p->used = true;
    if (w <= 0 || h <= 0 || p->n_planes < 1 || id == 0) {
        wl_resource_post_error(res,
            ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
            "zero size, no planes or bad id");
        return;
    }
    _dmabuf_buf_t *b = _buffer_import((uint32_t)w, (uint32_t)h, format,
                                      p->planes, p->n_planes, p->modifier);
    if (!b) {
        /* zombie WITH layout: the commit path reports the size and the
         * surface maps with empty content (see _dmabuf_commit_pixels) */
        b = calloc(1, sizeof(*b));
        if (b) {
            b->magic = _DMABUF_BUF_MAGIC;
            b->w = (uint32_t)w;
            b->h = (uint32_t)h;
            b->fourcc = format;
        }
    }
    struct wl_resource *br = wl_resource_create(cli, &wl_buffer_interface,
                                                1, id);
    if (!br) {
        wl_client_post_no_memory(cli);
        if (b) {
            if (b->img) vt_dmabuf_image_free(b->img);
            if (b->mm.map) munmap(b->mm.map, b->mm.len);
            free(b);
        }
        return;
    }
    if (b) {
        b->y_invert = !!(flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);
        b->res = br;
        wl_list_insert(_D.bufs.prev, &b->link);
    }
    wl_resource_set_implementation(br, &_buf_impl, b, _buf_res_destroy);
    vt_logd("dmabuf: immed buffer %p %.4s %dx%d mod 0x%llx (%s)",
            (void *)br, (const char *)&format, w, h,
            (unsigned long long)p->modifier,
            b ? (b->img ? "EGL" : (b->mm.map ? "mmap" : "zombie"))
              : "zombie");
}

static const struct zwp_linux_buffer_params_v1_interface _params_impl = {
    .destroy = _params_destroy,
    .add = _params_add,
    .create = _params_create,
    .create_immed = _params_create_immed,
};

static void _params_res_destroy(struct wl_resource *res) {
    _dmabuf_params_t *p = wl_resource_get_user_data(res);
    if (!p) return;
    /* close any fds never consumed by create */
    for (int i = 0; i < p->n_planes; i++)
        if (p->planes[i].fd >= 0) close(p->planes[i].fd);
    free(p);
}

/* ------------------------------------------------------------ bind */
static void _bind_dmabuf(struct wl_client *cli, void *data,
                         uint32_t version, uint32_t id) {
    (void)data;
    uint32_t v = version < _DMABUF_VERSION ? version : _DMABUF_VERSION;
    struct wl_resource *res = wl_resource_create(
        cli, &zwp_linux_dmabuf_v1_interface, v, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_mgr_impl, NULL, NULL);
    /* legacy format/modifier advertisement (v1–v3 binds); v4 clients
     * use the feedback objects instead. Only self-tested-capable
     * modifiers are promised (same policy as the v4 feedback). */
    if (v < 4) {
        for (int i = 0; i < _N_FORMATS; i++) {
            uint32_t f = _formats[i];
            zwp_linux_dmabuf_v1_send_format(res, f);
            if (v >= 3) {
                if (_D.adv_invalid)
                    zwp_linux_dmabuf_v1_send_modifier(
                        res, f, DRM_FORMAT_MOD_INVALID >> 32,
                        (uint32_t)DRM_FORMAT_MOD_INVALID);
                if (_D.adv_linear)
                    zwp_linux_dmabuf_v1_send_modifier(
                        res, f, DRM_FORMAT_MOD_LINEAR >> 32,
                        (uint32_t)DRM_FORMAT_MOD_LINEAR);
            }
        }
    }
}

/* implement the forwarded manager requests (bodies after the
 * implementation structs above) */
static void _get_default_feedback(struct wl_client *cli,
                                  struct wl_resource *res, uint32_t id) {
    (void)res;
    struct wl_resource *fb = wl_resource_create(
        cli, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    if (!fb) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(fb, &_fb_impl, NULL, NULL);
    _feedback_send_formats(fb);
}
static void _get_surface_feedback(struct wl_client *cli,
                                  struct wl_resource *res, uint32_t id,
                                  struct wl_resource *surface) {
    (void)surface;
    _get_default_feedback(cli, res, id);
}
static void _mgr_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}
static void _mgr_create_params(struct wl_client *cli,
                               struct wl_resource *res, uint32_t id) {
    _dmabuf_params_t *p = calloc(1, sizeof(*p));
    if (!p) { wl_client_post_no_memory(cli); return; }
    p->modifier = DRM_FORMAT_MOD_INVALID;
    struct wl_resource *pr = wl_resource_create(
        cli, &zwp_linux_buffer_params_v1_interface,
        wl_resource_get_version(res), id);
    if (!pr) { free(p); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(pr, &_params_impl, p,
                                   _params_res_destroy);
}

/* ------------------------------------------------------------ buffer */
static void _buf_res_destroy(struct wl_resource *res) {
    _dmabuf_buf_t *b = wl_resource_get_user_data(res);
    if (!b) return;
    if (b->magic != _DMABUF_BUF_MAGIC) return;
    if (b->img) vt_dmabuf_image_free(b->img);
    if (b->mm.map) munmap(b->mm.map, b->mm.len);
    wl_list_remove(&b->link);
    b->magic = 0;
    free(b);
}

/* ------------------------------------------------------ public hooks */
bool _dmabuf_globals_create(_wl_state_t *st) {
    if (!st) return false;
    if (vt_dmabuf_egl_init() != 0) {
        vt_logi("dmabuf: EGL import engine unavailable — the linux-dmabuf "
                "global stays unadvertised (clients use wl_shm)");
        return false;
    }
    _D.adv_linear = vt_dmabuf_egl_advertise_linear();
    _D.adv_invalid = vt_dmabuf_egl_advertise_implicit();
    if (!_D.adv_linear && !_D.adv_invalid) {
        /* the self-test could not import ANY real GPU buffer: promising
         * dmabuf support anyway would hand every EGL client zombie
         * buffers (black windows, dead Xwayland). Stay silent instead —
         * clients keep rendering through wl_shm and the desktop WORKS. */
        vt_logi("dmabuf: this machine's driver cannot import client GPU "
                "buffers (selftest failed) — the linux-dmabuf global "
                "stays unadvertised; clients render via wl_shm");
        return false;
    }
    wl_list_init(&_D.bufs);
    _D.global = wl_global_create(
        st->display, &zwp_linux_dmabuf_v1_interface, _DMABUF_VERSION, NULL,
        _bind_dmabuf);
    if (!_D.global) {
        vt_logw("dmabuf: global creation failed");
        return false;
    }
    vt_logi("dmabuf: zwp_linux_dmabuf_v1 v%d advertised — import renderer "
            "'%s', %d formats, modifiers {%s%s%s} (self-tested)",
            _DMABUF_VERSION, vt_dmabuf_egl_renderer(), _N_FORMATS,
            _D.adv_linear ? "LINEAR" : "",
            _D.adv_linear && _D.adv_invalid ? "+" : "",
            _D.adv_invalid ? "INVALID" : "");
    return true;
}

void _dmabuf_globals_destroy(_wl_state_t *st) {
    (void)st;
    if (_D.global) {
        wl_global_destroy(_D.global);
        _D.global = NULL;
    }
    /* buffers die with their client resources; just report */
    vt_logi("dmabuf: shutdown — imports=%ld mmapped=%ld failed=%ld",
            _D.n_imported, _D.n_mmapped, _D.n_failed);
    vt_dmabuf_egl_fini();
}

/* Is this wl_buffer one of OUR dma-buf buffers? */
_dmabuf_buf_t *_dmabuf_buffer_of(struct wl_resource *res) {
    if (!res) return NULL;
    /* get_class returns the interface NAME (a string) */
    const char *cls = wl_resource_get_class(res);
    if (!cls || strcmp(cls, wl_buffer_interface.name) != 0) return NULL;
    _dmabuf_buf_t *b = wl_resource_get_user_data(res);
    if (!b || b->magic != _DMABUF_BUF_MAGIC) return NULL;
    return b;
}

/* Read a dma-buf buffer back into ARGB8888 pixels. Mirrors the SHM
 * commit path: the compositor owns its copy afterwards, so the buffer
 * is released right away and the client never stalls.
 *
 * dest == NULL is a PROBE: no readback, only the size is reported
 * (the backend core sizes its pixel storage between the two calls).
 *
 * A ZOMBIE (import failed, layout recorded) still reports its size —
 * the surface then maps with empty content instead of vanishing — but
 * yields no pixels, so the readback call returns false. */
bool _dmabuf_commit_pixels(struct wl_resource *res, uint32_t *dest,
                           int dest_stride_u32, int32_t *out_w,
                           int32_t *out_h) {
    _dmabuf_buf_t *b = _dmabuf_buffer_of(res);
    if (!b) return false;
    if (out_w) *out_w = (int32_t)b->w;
    if (out_h) *out_h = (int32_t)b->h;
    if (!b->img && !b->mm.map)
        return dest == NULL;                     /* zombie: size only */
    if (!dest) return true;                      /* probe only */
    return _buffer_readback(b, dest, dest_stride_u32);
}

/* per-frame stats for the stage log */
void _dmabuf_log_stats(void) {
    if (_D.n_imported || _D.n_mmapped || _D.n_failed)
        vt_logi("dmabuf: buffers so far: EGL-imported=%ld mmap=%ld "
                "failed=%ld", _D.n_imported, _D.n_mmapped, _D.n_failed);
}
