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
} vt_dmabuf_image_t;

static struct {
    bool     inited;
    bool     ok;
    EGLDisplay dpy;
    EGLContext  ctx;
    dev_t    dev;
    char     renderer[160];
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
    /* keep fd + gbm alive for the EGL display's lifetime */
    _E.dpy = d;
    (void)fd; (void)gbm;
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
    vt_logi("dmabuf-egl: EGL on %s device (EGL %d.%d)%s", "device",
            maj, min, soft ? " [software]" : "");
    return true;
}

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
    bool have_modifiers = exts && strstr(exts,
                                "EGL_EXT_image_dma_buf_import_modifiers");
    vt_logi("dmabuf-egl: dma-buf import ready%s",
            have_modifiers ? " (with modifiers)" : " (no modifiers)");

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
    _E.ok = true;
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
    memset(&_E, 0, sizeof(_E));
    _E.dpy = EGL_NO_DISPLAY;
}

bool vt_dmabuf_egl_available(void) { return _E.ok; }
dev_t vt_dmabuf_egl_device(void) { return _E.dev; }
const char *vt_dmabuf_egl_renderer(void) { return _E.renderer; }

vt_dmabuf_image_t *vt_dmabuf_import(const int *fds, const uint32_t *offsets,
                                    const uint32_t *strides,
                                    uint64_t modifier, uint32_t w, uint32_t h,
                                    uint32_t fourcc, int n_planes) {
    if (!_E.ok || !fds || n_planes < 1 || n_planes > 4) return NULL;
    if (w == 0 || h == 0) return NULL;

    vt_dmabuf_image_t *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->w = w;
    img->h = h;
    img->fourcc = fourcc;
    img->n_planes = n_planes;

    /* single-plane RGB(A) formats only — that covers every toolkit/
     * browser/game swapchain format we advertise */
    if (n_planes != 1) {
        vt_logd("dmabuf-egl: %u planes unsupported", n_planes);
        free(img);
        return NULL;
    }

    EGLint attr[6 /*fixed*/ + 5 /*plane0*/ + 6 /*modifier*/ + 1];
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
    if (modifier != DRM_FORMAT_MOD_LINEAR &&
        modifier != DRM_FORMAT_MOD_INVALID) {
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attr[a++] = (EGLint)(modifier & 0xFFFFFFFFULL);
        attr[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attr[a++] = (EGLint)(modifier >> 32);
    }
    attr[a] = EGL_NONE;

    img->egl_image = _E.p_CreateImageKHR(_E.dpy, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT,
                                         (EGLClientBuffer)0, attr);
    if (img->egl_image == EGL_NO_IMAGE_KHR) {
        vt_logd("dmabuf-egl: eglCreateImageKHR failed (%04x) fourcc %.4s "
                "mod 0x%llx", eglGetError(), (const char *)&fourcc,
                (unsigned long long)modifier);
        free(img);
        return NULL;
    }
    glGenTextures(1, &img->tex);
    glBindTexture(GL_TEXTURE_2D, img->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    _E.p_ImageTargetTex2D(GL_TEXTURE_2D, img->egl_image);
    GLenum glerr = glGetError();
    if (glerr != GL_NO_ERROR) {
        vt_logd("dmabuf-egl: glEGLImageTargetTexture2DOES failed (%04x)",
                glerr);
        glDeleteTextures(1, &img->tex);
        _E.p_DestroyImageKHR(_E.dpy, img->egl_image);
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
        free(img);
        return NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < n_planes; i++)
        img->fds[i] = -1;               /* caller keeps fds open */
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
    free(img);
}

#else /* !VT_HAVE_EGL || !VT_HAVE_GLES */

int vt_dmabuf_egl_init(void) {
    vt_logi("dmabuf-egl: EGL/GLES not compiled — GPU buffer import off");
    return -1;
}
void vt_dmabuf_egl_fini(void) {}
bool vt_dmabuf_egl_available(void) { return false; }
dev_t vt_dmabuf_egl_device(void) { return 0; }
const char *vt_dmabuf_egl_renderer(void) { return "(no EGL)"; }
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
