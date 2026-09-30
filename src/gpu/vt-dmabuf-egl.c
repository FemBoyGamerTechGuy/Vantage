/*
 * vt-dmabuf-egl.c — dma-buf import engine (EGL) implementation
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * See vt-dmabuf-egl.h for the architecture. Everything EGL is resolved
 * through eglGetProcAddress (glvnd-safe, and works when only client
 * headers are present). The context is created ONCE and shared by all
 * imports; readback binds each image's texture to a scratch FBO and
 * does one glReadPixels per commit.
 *
 * IMPORT LADDER (the NVIDIA lesson): a dma-buf import can fail on one
 * driver while succeeding on another with the SAME buffer, so every
 * buffer gets three attempts before it is declared dead:
 *   1. eglCreateImage with EXPLICIT modifier attributes (spec-correct;
 *      DRM_FORMAT_MOD_INVALID passed explicitly means "driver-implicit
 *      layout"). NVIDIA's driver requires the modifier attributes for
 *      its own tiled buffers — importing them with the attributes
 *      omitted (the old code) makes the driver assume a different
 *      layout and the import fails.
 *   2. eglCreateImage with NO modifier attributes (implicit import —
 *      Mesa's default and the classic path).
 *   3. GBM route: gbm_bo_import() of the fd, then an EGLImage from the
 *      bo via EGL_NATIVE_PIXMAP_KHR — the pre-dmabuf-attributes
 *      compositor path some drivers still accept when they reject raw
 *      dma-buf imports.
 *
 * SELF-TEST (real hardware only): at init we allocate REAL GBM buffers
 * (LINEAR and driver-layout), run them through the full ladder and
 * render+read them back. The linux-dmabuf module then advertises only
 * the (format, modifier) combinations this machine can actually
 * import — clients never allocate a layout we would have to drop.
 */
#define VT_LOG_DOMAIN "dmabuf-egl"
#include <vantage/vt-core.h>
#include <vantage/vt-dmabuf-egl.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#if defined(VT_HAVE_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif
#if defined(VT_HAVE_GLES)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif
#if defined(VT_HAVE_GBM)
#include <gbm.h>
#include <drm_fourcc.h>
#else
#include <drm_fourcc.h>
#endif

/* ------------------------------------------------------------------ */
/* fourcc helpers (compile-time constants, no libdrm needed) */
#define VT_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))
#define VT_FOURCC_ARGB8888 VT_FOURCC('A', 'R', '2', '4')
#define VT_FOURCC_XRGB8888 VT_FOURCC('X', 'R', '2', '4')
#define VT_FOURCC_ABGR8888 VT_FOURCC('A', 'B', '2', '4')
#define VT_FOURCC_XBGR8888 VT_FOURCC('X', 'B', '2', '4')
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

#if defined(VT_HAVE_EGL) && defined(VT_HAVE_GLES)

/* ------------------------------------------------------------ state */
typedef struct vt_dmabuf_image {
    uint32_t w, h;
    uint32_t fourcc;
    int      n_planes;
    /* the fds stay open for the image's lifetime (see header) */
    int      fds[4];
    EGLImageKHR egl_image;
    GLuint   tex;
    GLuint   fbo;
    void    *bo;                /* GBM bo when imported via the GBM route */
} vt_dmabuf_image_t;

static struct {
    bool     inited;
    bool     ok;
    EGLDisplay dpy;
    EGLContext  ctx;
    dev_t    dev;
    char     renderer[160];
    bool     have_modifiers;    /* EGL_EXT_image_dma_buf_import_modifiers */
    /* GBM render-node device (NULL on the software/headless path) */
    void    *gbm;               /* struct gbm_device * */
    /* self-test results (real GPU): which layouts import+read back */
    bool     st_linear;         /* LINEAR GEM buffers usable (EGL or mmap) */
    bool     st_implicit;       /* driver-layout buffers import via EGL */
    bool     st_mmap;           /* GEM dma-bufs are CPU-mappable at all */
    bool     software;          /* display is the software EGL device */
    long     n_fail_warns;      /* rate limit for import-failure logs */
    /* proc addresses */
    EGLImageKHR (*p_CreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                    EGLClientBuffer, const EGLint *);
    EGLBoolean  (*p_DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    void        (*p_ImageTargetTex2D)(GLenum, EGLImageKHR);
} _E = {0};

/* ------------------------------------------------------ proc tables */
typedef void *EGLDeviceEXT_;
typedef EGLBoolean (*qdevs_t)(EGLint, EGLDeviceEXT_ *, EGLint *);
typedef const char *(*qdevstr_t)(EGLDeviceEXT_, EGLint);
typedef EGLDisplay (*getplat_t)(EGLenum, void *, const EGLint *);

#define _ERR_GOTO(lbl, ...) { vt_logw(__VA_ARGS__); goto lbl; }

/* Try EGL on the DRM render node via GBM (real hardware path). */
static bool _init_render_node(void) {
#if defined(VT_HAVE_GBM)
    const char *node = getenv("VANTAGE_DMABUF_NODE");
    if (!node || !*node) node = "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        vt_logi("dmabuf-egl: no render node (%s) — %s", node,
                strerror(errno));
        return false;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) {
        vt_logw("dmabuf-egl: gbm_create_device(%s) failed", node);
        close(fd);
        return false;
    }
    getplat_t gp = (getplat_t)eglGetProcAddress("eglGetPlatformDisplay");
    if (!gp) { gbm_device_destroy(gbm); close(fd); return false; }
    EGLDisplay d = gp(EGL_PLATFORM_GBM_MESA, (void *)gbm, NULL);
    if (d == EGL_NO_DISPLAY) {
        vt_logw("dmabuf-egl: EGL platform display on GBM failed");
        gbm_device_destroy(gbm); close(fd);
        return false;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(d, &maj, &min)) {
        vt_logw("dmabuf-egl: eglInitialize(gbm) failed");
        gbm_device_destroy(gbm); close(fd);
        return false;
    }
    struct stat st;
    _E.dev = (stat(node, &st) == 0) ? st.st_rdev : 0;
    /* keep fd + gbm alive for the EGL display's lifetime (the GBM
     * import fallback also needs the gbm_device later) */
    _E.dpy = d;
    _E.gbm = gbm;
    _E.software = false;
    vt_logi("dmabuf-egl: EGL on GBM render node %s (EGL %d.%d)",
            node, maj, min);
    return true;
#else
    (void)0;
    return false;
#endif
}

/* Headless fallback: Mesa's software EGL device (EGL_MESA_device_software).
 * llvmpipe implements EGL_EXT_image_dma_buf_import — the whole
 * import→texture→readback path runs, so headless tests exercise the
 * same code a real GPU executes. */
static bool _init_software_device(void) {
    qdevs_t q = (qdevs_t)eglGetProcAddress("eglQueryDevicesEXT");
    if (!q) {
        vt_logi("dmabuf-egl: eglQueryDevicesEXT unavailable");
        return false;
    }
    EGLDeviceEXT_ devs[8];
    EGLint n = 0;
    if (!q(8, devs, &n) || n < 1) {
        vt_logi("dmabuf-egl: no EGL devices");
        return false;
    }
    getplat_t gp = (getplat_t)eglGetProcAddress("eglGetPlatformDisplay");
    if (!gp) return false;
    qdevstr_t qs = (qdevstr_t)eglGetProcAddress("eglQueryDeviceStringEXT");
    /* prefer a device WITH a DRM render node (real GPU even when we
     * could not open renderD128 ourselves); else the software device */
    int pick = -1;
    for (int i = 0; i < n; i++) {
        const char *ext = qs ? qs(devs[i], EGL_EXTENSIONS) : "";
        if (ext && strstr(ext, "EGL_EXT_device_drm_render_node"))
            { pick = i; break; }
    }
    if (pick < 0)
        for (int i = 0; i < n; i++) {
            const char *ext = qs ? qs(devs[i], EGL_EXTENSIONS) : "";
            if (ext && strstr(ext, "EGL_MESA_device_software"))
                { pick = i; break; }
        }
    if (pick < 0) pick = 0;
    const char *ext = qs ? qs(devs[pick], EGL_EXTENSIONS) : "";
    bool soft = ext && strstr(ext, "EGL_MESA_device_software");
    EGLDisplay d = gp(EGL_PLATFORM_DEVICE_EXT, (void *)devs[pick], NULL);
    if (d == EGL_NO_DISPLAY) {
        vt_logw("dmabuf-egl: EGL device display failed");
        return false;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(d, &maj, &min)) {
        vt_logw("dmabuf-egl: eglInitialize(device) failed");
        return false;
    }
    _E.dev = 0;
    _E.dpy = d;
    _E.gbm = NULL;
    _E.software = true;
    vt_logi("dmabuf-egl: EGL on %s device (EGL %d.%d)%s", "device",
            maj, min, soft ? " [software]" : "");
    return true;
}

/* --------------------------------------------------------- importing */
/* one eglCreateImage attempt: with_mod=true passes the modifier
 * attributes explicitly (spec path, NVIDIA requirement), false omits
 * them (implicit import, the Mesa default). */
static EGLImageKHR _import_attempt(const int *fds, const uint32_t *offsets,
                                   const uint32_t *strides, uint64_t modifier,
                                   uint32_t w, uint32_t h, uint32_t fourcc,
                                   bool with_mod) {
    EGLint attr[6 + 6 + 6 + 1];
    int a = 0;
    attr[a++] = EGL_WIDTH;
    attr[a++] = (EGLint)w;
    attr[a++] = EGL_HEIGHT;
    attr[a++] = (EGLint)h;
    attr[a++] = EGL_LINUX_DRM_FOURCC_EXT;
    attr[a++] = (EGLint)fourcc;
    attr[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attr[a++] = fds[0];
    attr[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attr[a++] = (EGLint)offsets[0];
    attr[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attr[a++] = (EGLint)strides[0];
    if (with_mod && _E.have_modifiers) {
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attr[a++] = (EGLint)(modifier & 0xFFFFFFFFULL);
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attr[a++] = (EGLint)(modifier >> 32);
    }
    attr[a] = EGL_NONE;
    return _E.p_CreateImageKHR(_E.dpy, EGL_NO_CONTEXT,
                               EGL_LINUX_DMA_BUF_EXT,
                               (EGLClientBuffer)0, attr);
}

#if defined(VT_HAVE_GBM)
/* gbm_import_fd_modifier_data's plane-count member was RENAMED across
 * gbm releases (num_planes → num_fds) while the layout stayed identical
 * (uint32 ×4, int[4] ×3, uint64). Build the payload with our own
 * ABI-identical struct so the code compiles against either header. */
#if defined(GBM_MAX_PLANES) && GBM_MAX_PLANES != 4
#error "GBM_MAX_PLANES is no longer 4 — revisit the modifier-import shim"
#endif
struct vt_gbm_fd_modifier {
    uint32_t width, height, format, nplanes;
    int      fds[4], strides[4], offsets[4];
    uint64_t modifier;
};
/* GBM route: hand the fd to our gbm_device and make the
 * EGLImage from the resulting bo (EGL_NATIVE_PIXNUM_KHR classic path). */
static struct gbm_bo *_gbm_import_bo(const int *fds, const uint32_t *offsets,
                                     const uint32_t *strides, uint64_t modifier,
                                     uint32_t w, uint32_t h,
                                     uint32_t fourcc) {
    if (!_E.gbm) return NULL;
    struct gbm_device *gbm = (struct gbm_device *)_E.gbm;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        struct vt_gbm_fd_modifier md;
        memset(&md, 0, sizeof(md));
        md.width = w;
        md.height = h;
        md.format = fourcc;
        md.nplanes = 1;
        md.fds[0] = fds[0];
        md.strides[0] = strides[0];
        md.offsets[0] = offsets[0];
        md.modifier = modifier;
        struct gbm_bo *bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER,
                                          &md, GBM_BO_USE_RENDERING);
        if (bo) return bo;
    }
    if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR) {
        struct gbm_import_fd_data d;
        memset(&d, 0, sizeof(d));
        d.fd = fds[0];
        d.width = w;
        d.height = h;
        d.stride = strides[0];
        d.format = fourcc;
        struct gbm_bo *bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &d,
                                          GBM_BO_USE_RENDERING);
        if (bo) return bo;
    }
    return NULL;
}
static void _gbm_bo_destroy(void *bo) {
    if (bo) gbm_bo_destroy((struct gbm_bo *)bo);
}
#else
static void *_gbm_import_bo(const int *fds, const uint32_t *offsets,
                            const uint32_t *strides, uint64_t modifier,
                            uint32_t w, uint32_t h, uint32_t fourcc) {
    (void)fds; (void)offsets; (void)strides; (void)modifier;
    (void)w; (void)h; (void)fourcc;
    return NULL;
}
static void _gbm_bo_destroy(void *bo) { (void)bo; }
#endif

vt_dmabuf_image_t *vt_dmabuf_import(const int *fds, const uint32_t *offsets,
                                    const uint32_t *strides,
                                    uint64_t modifier, uint32_t w, uint32_t h,
                                    uint32_t fourcc, int n_planes) {
    if (!_E.ok || !fds || n_planes < 1 || n_planes > 4) return NULL;
    if (w == 0 || h == 0) return NULL;
    /* test hook: force every import to fail — the zombie/empty-map
     * degradation path becomes reproducible on ANY machine */
    if (getenv("VANTAGE_DMABUF_ZOMBIE")) return NULL;

    /* single-plane RGB(A) formats only — that covers every toolkit/
     * browser/game swapchain format we advertise */
    if (n_planes != 1) {
        vt_logd("dmabuf-egl: %u planes unsupported", n_planes);
        return NULL;
    }

    EGLImageKHR eimg = EGL_NO_IMAGE_KHR;
    EGLint e_explicit = 0, e_implicit = 0;
    void *bo = NULL;

    /* 1. explicit modifier */
    if (_E.have_modifiers) {
        eimg = _import_attempt(fds, offsets, strides, modifier, w, h,
                               fourcc, true);
        if (eimg == EGL_NO_IMAGE_KHR) e_explicit = eglGetError();
    }
    /* 2. implicit (no modifier attributes) */
    if (eimg == EGL_NO_IMAGE_KHR) {
        eimg = _import_attempt(fds, offsets, strides, modifier, w, h,
                               fourcc, false);
        if (eimg == EGL_NO_IMAGE_KHR) e_implicit = eglGetError();
    }
    /* 3. GBM bo route */
    if (eimg == EGL_NO_IMAGE_KHR) {
        bo = _gbm_import_bo(fds, offsets, strides, modifier, w, h, fourcc);
        if (bo) {
            EGLint pattr[] = { EGL_NONE };
            eimg = _E.p_CreateImageKHR(_E.dpy, EGL_NO_CONTEXT,
                                       EGL_NATIVE_PIXMAP_KHR,
                                       (EGLClientBuffer)bo, pattr);
        }
    }
    if (eimg == EGL_NO_IMAGE_KHR) {
        /* rate-limited WARN: this is the one line that tells us, on a
         * user machine, exactly why their GPU buffers do not import */
        if (_E.n_fail_warns < 6) {
            _E.n_fail_warns++;
            vt_logw("dmabuf-egl: import FAILED %.4s %ux%u mod 0x%llx "
                    "stride %u — egl(explicit)=%04x egl(implicit)=%04x "
                    "gbm=%s",
                    (const char *)&fourcc, w, h,
                    (unsigned long long)modifier, strides[0],
                    (unsigned)e_explicit, (unsigned)e_implicit,
                    bo ? "img-failed" : "rejected");
        } else {
            vt_logd("dmabuf-egl: import failed %.4s mod 0x%llx",
                    (const char *)&fourcc,
                    (unsigned long long)modifier);
        }
        if (bo) _gbm_bo_destroy(bo);
        return NULL;
    }

    vt_dmabuf_image_t *img = calloc(1, sizeof(*img));
    if (!img) {
        _E.p_DestroyImageKHR(_E.dpy, eimg);
        if (bo) _gbm_bo_destroy(bo);
        return NULL;
    }
    img->w = w;
    img->h = h;
    img->fourcc = fourcc;
    img->n_planes = n_planes;
    img->egl_image = eimg;
    img->bo = bo;
    for (int i = 0; i < n_planes; i++)
        img->fds[i] = -1;               /* caller keeps fds open */

    glGenTextures(1, &img->tex);
    glBindTexture(GL_TEXTURE_2D, img->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR) { /* clear stale errors */ }
    _E.p_ImageTargetTex2D(GL_TEXTURE_2D, img->egl_image);
    GLenum glerr = glGetError();
    if (glerr != GL_NO_ERROR) {
        if (_E.n_fail_warns < 6) {
            _E.n_fail_warns++;
            vt_logw("dmabuf-egl: glEGLImageTargetTexture2DOES failed "
                    "(%04x) %.4s %ux%u mod 0x%llx", glerr,
                    (const char *)&fourcc, w, h,
                    (unsigned long long)modifier);
        }
        glDeleteTextures(1, &img->tex);
        _E.p_DestroyImageKHR(_E.dpy, img->egl_image);
        if (img->bo) _gbm_bo_destroy(img->bo);
        free(img);
        return NULL;
    }
    glGenFramebuffers(1, &img->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, img->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        vt_logd("dmabuf-egl: FBO incomplete for imported image");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &img->fbo);
        glDeleteTextures(1, &img->tex);
        _E.p_DestroyImageKHR(_E.dpy, img->egl_image);
        if (img->bo) _gbm_bo_destroy(img->bo);
        free(img);
        return NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return img;
}

bool vt_dmabuf_readback_argb(vt_dmabuf_image_t *img, uint32_t *dest,
                             int dest_stride_u32, bool y_invert) {
    if (!img || !dest || !_E.ok) return false;
    uint32_t w = img->w, h = img->h;

    /* scratch RGBA byte buffer (glReadPixels origin = bottom-left) */
    size_t row_bytes = (size_t)w * 4;
    uint8_t *scratch = malloc(row_bytes * h);
    if (!scratch) return false;

    if (!eglMakeCurrent(_E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, _E.ctx)) {
        free(scratch);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
    while (glGetError() != GL_NO_ERROR) { /* clear stale errors */ }
    glReadPixels(0, 0, (GLint)w, (GLint)h, GL_RGBA, GL_UNSIGNED_BYTE,
                 scratch);
    GLenum glerr = glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glerr != GL_NO_ERROR) {
        vt_logw("dmabuf-egl: glReadPixels failed (%04x)", glerr);
        free(scratch);
        return false;
    }
    if (getenv("VANTAGE_DBG_FRAMES"))
        vt_logd("dmabuf-egl: readback %ux%u first=%02x%02x%02x%02x "
                "mid=%02x%02x%02x%02x",
                w, h, scratch[0], scratch[1], scratch[2], scratch[3],
                scratch[(size_t)(w / 2 + (h / 2) * w) * 4],
                scratch[(size_t)(w / 2 + (h / 2) * w) * 4 + 1],
                scratch[(size_t)(w / 2 + (h / 2) * w) * 4 + 2],
                scratch[(size_t)(w / 2 + (h / 2) * w) * 4 + 3]);

    /* convert RGBA bytes → ARGB8888 words, Y-flipped (GL readback is
     * bottom-up; a Y_INVERT buffer already arrived bottom-up). The
     * EGLImage already decodes the fourcc — GL_RGBA readback bytes are
     * correct for ARGB and ABGR sources alike; X-variants get a forced
     * opaque alpha (their alpha bits are undefined). */
    bool force_a = (img->fourcc == VT_FOURCC_XRGB8888 ||
                    img->fourcc == VT_FOURCC_XBGR8888);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = y_invert ? y : (h - 1 - y);
        const uint8_t *src = scratch + (size_t)sy * row_bytes;
        uint32_t *dst = dest + (size_t)y * (size_t)dest_stride_u32;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1],
                    b = src[x * 4 + 2], al = src[x * 4 + 3];
            if (force_a) al = 0xff;
            dst[x] = ((uint32_t)al << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    free(scratch);
    return true;
}

void vt_dmabuf_image_free(vt_dmabuf_image_t *img) {
    if (!img) return;
    if (_E.ok) {
        if (img->fbo) glDeleteFramebuffers(1, &img->fbo);
        if (img->tex) glDeleteTextures(1, &img->tex);
        if (img->egl_image)
            _E.p_DestroyImageKHR(_E.dpy, img->egl_image);
    }
    if (img->bo) _gbm_bo_destroy(img->bo);
    free(img);
}

/* ---------------------------------------------------------- selftest */
#if defined(VT_HAVE_GBM)
/* Allocate a REAL GBM buffer, import it through the full ladder, render
 * into it and read the result back — the exact per-commit path a real
 * client buffer takes. `mmap_seen` records whether GEM dma-bufs on this
 * machine are CPU-mappable (the LINEAR readback fallback). */
static bool _selftest_run(uint32_t fourcc, uint32_t flags, const char *tag,
                          bool *mmap_seen) {
    /* test hook: pretend the self-test passed so the advertisement
     * stays up while VANTAGE_DMABUF_ZOMBIE fails real imports */
    if (getenv("VANTAGE_DMABUF_ZOMBIE")) {
        vt_logi("dmabuf-egl: selftest[%s]: SIMULATED OK (zombie test)", tag);
        return true;
    }
    struct gbm_device *gbm = (struct gbm_device *)_E.gbm;
    struct gbm_bo *bo = gbm_bo_create(gbm, 64, 64, fourcc, flags);
    if (!bo && (flags & GBM_BO_USE_RENDERING)) {
        /* some drivers refuse linear+rendering combos — plain linear */
        bo = gbm_bo_create(gbm, 64, 64, fourcc, GBM_BO_USE_LINEAR);
    }
    if (!bo) {
        vt_logi("dmabuf-egl: selftest[%s]: allocation FAILED", tag);
        return false;
    }
    int fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        vt_logi("dmabuf-egl: selftest[%s]: export FAILED", tag);
        gbm_bo_destroy(bo);
        return false;
    }
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    if (mmap_seen && !*mmap_seen) {
        size_t len = (size_t)stride * 64;
        void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) { munmap(m, len); *mmap_seen = true; }
    }
    int fds[1] = { fd };
    uint32_t offs[1] = { 0 }, strides[1] = { stride };
    vt_dmabuf_image_t *img = vt_dmabuf_import(fds, offs, strides, mod,
                                               64, 64, fourcc, 1);
    close(fd);
    bool ok = false;
    if (img) {
        /* GPU-render INTO the imported image, then read it back and
         * verify the pixels survived the round trip */
        if (eglMakeCurrent(_E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           _E.ctx)) {
            glBindFramebuffer(GL_FRAMEBUFFER, img->fbo);
            glClearColor(0.25f, 0.50f, 0.75f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            uint32_t out[64 * 64];
            if (vt_dmabuf_readback_argb(img, out, 64, false)) {
                uint32_t p = out[32 * 64 + 32];
                int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff,
                    b = p & 0xff;
                ok = (r >= 0x30 && r <= 0x50) &&
                     (g >= 0x70 && g <= 0x90) &&
                     (b >= 0xb0 && b <= 0xd0);
            }
        }
        vt_dmabuf_image_free(img);
    }
    vt_logi("dmabuf-egl: selftest[%s]: %s (mod 0x%llx stride %u%s)",
            tag, ok ? "OK — import+render+readback verified"
                    : "FAILED (import unusable on this driver)",
            (unsigned long long)mod, stride,
            *mmap_seen ? ", GEM mmap works" : "");
    gbm_bo_destroy(bo);
    return ok;
}
#endif

int vt_dmabuf_egl_init(void) {
    if (_E.inited) return _E.ok ? 0 : -1;
    _E.inited = true;
    if (getenv("VANTAGE_DMABUF") && !strcmp(getenv("VANTAGE_DMABUF"), "0")) {
        vt_logi("dmabuf-egl: disabled (VANTAGE_DMABUF=0)");
        return -1;
    }

    if (!_init_render_node())
        if (!_init_software_device())
            return -1;

    const char *exts = eglQueryString(_E.dpy, EGL_EXTENSIONS);
    if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
        vt_logw("dmabuf-egl: EGL_EXT_image_dma_buf_import missing on this "
                "display — client GPU buffers cannot be imported");
        eglTerminate(_E.dpy);
        _E.dpy = EGL_NO_DISPLAY;
        return -1;
    }
    _E.have_modifiers = exts && strstr(exts,
                                "EGL_EXT_image_dma_buf_import_modifiers");
    vt_logi("dmabuf-egl: dma-buf import ready%s",
            _E.have_modifiers ? " (with modifiers)" : " (no modifiers)");

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        vt_logw("dmabuf-egl: eglBindAPI(ES) failed");
        eglTerminate(_E.dpy);
        _E.dpy = EGL_NO_DISPLAY;
        return -1;
    }
    EGLint const ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    _E.ctx = eglCreateContext(_E.dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (_E.ctx == EGL_NO_CONTEXT) {
        /* configless context unsupported — pick an RGBA8 ES2 config */
        EGLint const cfg_attr[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE,
        };
        EGLConfig cfg = NULL;
        EGLint nc = 0;
        if (!eglChooseConfig(_E.dpy, cfg_attr, &cfg, 1, &nc) || nc < 1) {
            vt_logw("dmabuf-egl: no ES2 config — import engine off");
            eglTerminate(_E.dpy);
            _E.dpy = EGL_NO_DISPLAY;
            return -1;
        }
        _E.ctx = eglCreateContext(_E.dpy, cfg, EGL_NO_CONTEXT, ca);
        if (_E.ctx == EGL_NO_CONTEXT) {
            vt_logw("dmabuf-egl: eglCreateContext failed (%04x)",
                    eglGetError());
            eglTerminate(_E.dpy);
            _E.dpy = EGL_NO_DISPLAY;
            return -1;
        }
    }
    if (!eglMakeCurrent(_E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, _E.ctx)) {
        vt_logw("dmabuf-egl: eglMakeCurrent(surfaceless) failed (%04x)",
                eglGetError());
        eglDestroyContext(_E.dpy, _E.ctx);
        _E.ctx = EGL_NO_CONTEXT;
        eglTerminate(_E.dpy);
        _E.dpy = EGL_NO_DISPLAY;
        return -1;
    }
    /* POSIX dlsym-style lookup: eglGetProcAddress returns a FUNCTION
     * POINTER and the struct fields are typed function pointers of a
     * different signature — ISO C forbids both fp<->void* and
     * fp->fp conversions, so bridge each through a union (identical
     * bits on every supported ABI, and -Wpedantic stays quiet). */
    union { void (*raw)(void); __typeof__(_E.p_CreateImageKHR) f; } ucreate;
    union { void (*raw)(void); __typeof__(_E.p_DestroyImageKHR) f; } udestroy;
    union { void (*raw)(void); __typeof__(_E.p_ImageTargetTex2D) f; } utarget;
    ucreate.raw = eglGetProcAddress("eglCreateImageKHR");
    udestroy.raw = eglGetProcAddress("eglDestroyImageKHR");
    utarget.raw = eglGetProcAddress("glEGLImageTargetTexture2DOES");
    _E.p_CreateImageKHR = ucreate.f;
    _E.p_DestroyImageKHR = udestroy.f;
    _E.p_ImageTargetTex2D = utarget.f;
    if (!_E.p_CreateImageKHR || !_E.p_DestroyImageKHR ||
        !_E.p_ImageTargetTex2D) {
        vt_logw("dmabuf-egl: EGLImage entry points missing");
        vt_dmabuf_egl_fini();
        return -1;
    }
    const char *r = (const char *)glGetString(GL_RENDERER);
    snprintf(_E.renderer, sizeof(_E.renderer), "%s", r ? r : "?");
    vt_logi("dmabuf-egl: import renderer: %s — client GPU buffers will be "
            "imported and read back per commit", _E.renderer);
    /* the engine is live from here on — the self-test below runs REAL
     * allocations through vt_dmabuf_import/vt_dmabuf_readback_argb,
     * both of which require _E.ok */
    _E.ok = true;

    /* ---- capability self-test ----------------------------------------
     * On the software/headless device every advertised layout works
     * (memfd-backed LINEAR buffers import through the same EGL path
     * AND are CPU-mappable). On a REAL render node we verify with real
     * GBM allocations — the advertisement then only promises what this
     * machine can actually import. */
    if (_E.gbm) {
#if defined(VT_HAVE_GBM)
        bool mmap_seen = false;
        bool lin = _selftest_run(VT_FOURCC_ARGB8888,
                                 GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING,
                                 "LINEAR", &mmap_seen);
        bool imp = _selftest_run(VT_FOURCC_ARGB8888, GBM_BO_USE_RENDERING,
                                 "driver-layout", &mmap_seen);
        _E.st_linear = lin || mmap_seen;
        _E.st_implicit = imp;
        _E.st_mmap = mmap_seen;
        if (!_E.st_linear && !_E.st_implicit)
            vt_logw("dmabuf-egl: this driver cannot import ANY client "
                    "GPU buffer (selftest) — the linux-dmabuf global "
                    "must stay unadvertised; clients will use wl_shm");
#endif
    } else {
        _E.st_linear = true;
        _E.st_implicit = true;
        _E.st_mmap = true;
    }
    vt_logi("dmabuf-egl: capability: LINEAR=%s driver-layout=%s mmap=%s "
            "(%s path)",
            _E.st_linear ? "yes" : "no", _E.st_implicit ? "yes" : "no",
            _E.st_mmap ? "yes" : "no",
            _E.gbm ? "render node" : "software device");
    return 0;
}

void vt_dmabuf_egl_fini(void) {
    if (_E.ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(_E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(_E.dpy, _E.ctx);
        _E.ctx = EGL_NO_CONTEXT;
    }
    if (_E.dpy != EGL_NO_DISPLAY) {
        eglTerminate(_E.dpy);
        _E.dpy = EGL_NO_DISPLAY;
    }
    /* the EGL display was created ON the gbm device — destroy it only
     * after eglTerminate (a live display holds references into gbm) */
    if (_E.gbm) {
#if defined(VT_HAVE_GBM)
        gbm_device_destroy((struct gbm_device *)_E.gbm);
#endif
        _E.gbm = NULL;
    }
    memset(&_E, 0, sizeof(_E));
    _E.dpy = EGL_NO_DISPLAY;
}

bool vt_dmabuf_egl_available(void) { return _E.ok; }
dev_t vt_dmabuf_egl_device(void) { return _E.dev; }
const char *vt_dmabuf_egl_renderer(void) { return _E.renderer; }
bool vt_dmabuf_egl_advertise_linear(void) { return _E.ok && _E.st_linear; }
bool vt_dmabuf_egl_advertise_implicit(void) { return _E.ok && _E.st_implicit; }
bool vt_dmabuf_egl_is_software(void) { return _E.software; }
bool vt_dmabuf_egl_gbm_mmap(void) { return _E.st_mmap; }

#else /* !VT_HAVE_EGL || !VT_HAVE_GLES */

int vt_dmabuf_egl_init(void) {
    vt_logi("dmabuf-egl: EGL/GLES not compiled — GPU buffer import off");
    return -1;
}
void vt_dmabuf_egl_fini(void) {}
bool vt_dmabuf_egl_available(void) { return false; }
dev_t vt_dmabuf_egl_device(void) { return 0; }
const char *vt_dmabuf_egl_renderer(void) { return "(no EGL)"; }
bool vt_dmabuf_egl_advertise_linear(void) { return false; }
bool vt_dmabuf_egl_advertise_implicit(void) { return false; }
bool vt_dmabuf_egl_is_software(void) { return true; }
bool vt_dmabuf_egl_gbm_mmap(void) { return false; }
vt_dmabuf_image_t *vt_dmabuf_import(const int *fds, const uint32_t *offsets,
                                    const uint32_t *strides,
                                    uint64_t modifier, uint32_t w, uint32_t h,
                                    uint32_t fourcc, int n_planes) {
    (void)fds; (void)offsets; (void)strides; (void)modifier;
    (void)w; (void)h; (void)fourcc; (void)n_planes;
    return NULL;
}
bool vt_dmabuf_readback_argb(vt_dmabuf_image_t *img, uint32_t *dest,
                             int dest_stride_u32, bool y_invert) {
    (void)img; (void)dest; (void)dest_stride_u32; (void)y_invert;
    return false;
}
void vt_dmabuf_image_free(vt_dmabuf_image_t *img) { (void)img; }

#endif
