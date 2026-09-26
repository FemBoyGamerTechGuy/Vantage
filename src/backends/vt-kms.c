/*
 * vt-kms.c — DRM/KMS scanout: connectors, CRTCs, GBM/dumb buffers,
 *            async page flips, hardware cursor, EGL renderer probe
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Composition is CPU-side into mapped scanout buffers — deliberately:
 * it is the same path the headless tests exercise, so what the tests
 * verify is what real hardware runs. The EGL context exists to report
 * the *real* renderer (llvmpipe on software Mesa, the actual GPU name
 * on real hardware) — never a hardcoded string.
 *
 * Async flips: only one flip may be in flight per CRTC (the kernel
 * rejects queuing a second with -EBUSY). While one is pending we park
 * the frame; the completion handler re-arms it.
 */
#define VT_LOG_DOMAIN "kms"
#include <vantage/vt-kms.h>
#include <vantage/vt-core.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#if defined(VT_HAVE_LIBDRM)
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#endif

#if defined(VT_HAVE_GBM)
#include <gbm.h>
#endif

#if defined(VT_HAVE_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#if defined(VT_HAVE_GLES)
#include <GLES2/gl2.h>
#endif

/* ------------------------------------------------------------ internals */

typedef struct {
    uint32_t conn_id;
    uint32_t crtc_id;
    char     name[32];
    int      w, h;                 /* mode dims */
    unsigned refresh_khz;          /* mode refresh in kHz (vrefresh/1000) */
    unsigned refresh_frac;         /* vrefresh % 1000 / 10 */
    /* ping-pong scanout buffers */
    struct {
        uint32_t fb_id;
        uint32_t handle;
        uint32_t stride;
        uint64_t size;
        void    *map;
#if defined(VT_HAVE_GBM)
        struct gbm_bo *bo;
        void          *map_data;   /* gbm_bo_map token */
#endif
    } bo[2];
    int      front;                /* index currently scanned out */
    int      pending;              /* index in flight, -1 = none */
    bool     needs_flip;           /* frame arrived during a pending flip */
    bool     live;                 /* first mode set done */
    drmModeCrtcPtr saved;          /* pre-compositor CRTC state */
} _out_t;

struct vt_kms {
    int              fd;
    bool             fd_via_seat;
    vt_seat_t       *seat;
    bool             master;
    bool             started;
    vt_kms_scanout_t how;          /* requested */
    bool             use_gbm;      /* actual buffer path */
#if defined(VT_HAVE_GBM)
    struct gbm_device *gbm_dev;
#endif
#if defined(VT_HAVE_EGL)
    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
#endif
    char             renderer[160];
    _out_t           outs[VT_KMS_MAX_OUT];
    int              n_out;
    /* cursor plane */
    bool             hw_cursor;
    bool             cursor_on;
    int              cur_w, cur_h;
    uint32_t         cur_stride;
    uint64_t         cur_size;
    uint32_t        *cur_map;
    uint32_t         cur_handle;
#if defined(VT_HAVE_GBM)
    struct gbm_bo   *cursor_bo;
    void            *cursor_map_data;
#endif
    /* completion hook so the compositor can repaint after a flip */
    void           (*flip_done)(void *ud);
    void            *flip_ud;
    vt_kms_scanout_fn first_scanout;
    void            *scanout_ud;
};

static void _cursor_init(struct vt_kms *k);
static void _flip_rearm(struct vt_kms *k, _out_t *o);
#if defined(VT_HAVE_LIBDRM)
static drmModeModeInfoPtr _pick_mode(drmModeConnectorPtr conn);
#endif

/* ------------------------------------------------------------- discovery */

int vt_kms_discover_cards(vt_kms_card_t *cards, int max_cards) {
#if defined(VT_HAVE_LIBDRM)
    int found = 0;
    for (int i = 0; i < 16 && found < max_cards; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT)
                vt_logd("kms: %s exists but cannot be opened: %s",
                        path, strerror(errno));
            continue;
        }
        drmVersionPtr ver = drmGetVersion(fd);
        if (!ver) {
            vt_logw("kms: %s is not a DRM device (drmGetVersion failed)",
                    path);
            close(fd);
            continue;
        }
        vt_kms_card_t *c = &cards[found];
        snprintf(c->path, sizeof(c->path), "%s", path);
        size_t drv_len = ver->name_len;
        if (drv_len >= sizeof(c->driver)) drv_len = sizeof(c->driver) - 1;
        snprintf(c->driver, sizeof(c->driver), "%.*s", (int)drv_len,
                 ver->name ? ver->name : "?");
        drmFreeVersion(ver);

        /* connected-connector probe (needs no master) */
        c->has_connected = false;
        drmModeResPtr res = drmModeGetResources(fd);
        if (res) {
            for (int ci = 0; ci < res->count_connectors && !c->has_connected;
                 ci++) {
                drmModeConnectorPtr conn =
                    drmModeGetConnector(fd, res->connectors[ci]);
                if (conn && conn->connection == DRM_MODE_CONNECTED)
                    c->has_connected = true;
                if (conn) drmModeFreeConnector(conn);
            }
            drmModeFreeResources(res);
        }
        close(fd);
        vt_logi("kms: found %s (driver '%s', connected output %s)",
                c->path, c->driver, c->has_connected ? "yes" : "no");
        found++;
    }
    if (found == 0)
        vt_logi("kms: no DRM cards under /dev/dri — no kernel mode "
                "setting available (container or missing GPU drivers)");
    return found;
#else
    (void)cards; (void)max_cards;
    vt_logi("kms: built without libdrm — KMS unavailable");
    return 0;
#endif
}

/* ------------------------------------------------------------- buffers */

static void _bo_free(_out_t *o, int i, int fd) {
#if defined(VT_HAVE_GBM)
    if (o->bo[i].bo) {
        if (o->bo[i].map)
            gbm_bo_unmap(o->bo[i].bo, o->bo[i].map_data);
        gbm_bo_destroy(o->bo[i].bo);
        memset(&o->bo[i], 0, sizeof(o->bo[i]));
        return;
    }
#endif
    if (o->bo[i].fb_id && fd >= 0) drmModeRmFB(fd, o->bo[i].fb_id);
    if (o->bo[i].map && o->bo[i].map != MAP_FAILED)
        munmap(o->bo[i].map, (size_t)o->bo[i].size);
    if (o->bo[i].handle && fd >= 0) {
        struct drm_mode_destroy_dumb d;
        memset(&d, 0, sizeof(d));
        d.handle = o->bo[i].handle;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(&o->bo[i], 0, sizeof(o->bo[i]));
}

#if defined(VT_HAVE_GBM)
static bool _gbm_bo_make(struct vt_kms *k, _out_t *o, int i) {
    struct gbm_bo *bo = gbm_bo_create(k->gbm_dev, o->w, o->h,
                                      GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_WRITE);
    if (!bo)
        bo = gbm_bo_create(k->gbm_dev, o->w, o->h,
                           GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!bo)
        bo = gbm_bo_create(k->gbm_dev, o->w, o->h,
                           GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
    if (!bo) {
        vt_logw("kms: gbm_bo_create(%dx%d) failed: %s",
                o->w, o->h, strerror(errno));
        return false;
    }
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t fb = 0;
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t strides[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(k->fd, o->w, o->h, GBM_FORMAT_XRGB8888,
                      handles, strides, offsets, &fb, 0) < 0 || !fb) {
        vt_logw("kms: drmModeAddFB2 for gbm_bo failed: %s",
                strerror(errno));
        gbm_bo_destroy(bo);
        return false;
    }
    void *map_data = NULL;
    uint32_t map_stride = 0;
    void *map = gbm_bo_map(bo, 0, 0, 0, 0, GBM_BO_TRANSFER_WRITE,
                           &map_stride, &map_data);
    if (!map) {
        vt_logw("kms: gbm_bo_map failed (non-mappable scanout): %s",
                strerror(errno));
        drmModeRmFB(k->fd, fb);
        gbm_bo_destroy(bo);
        return false;
    }
    o->bo[i].bo = bo;
    o->bo[i].map_data = map_data;
    o->bo[i].map = map;
    o->bo[i].stride = map_stride ? map_stride : stride;
    o->bo[i].fb_id = fb;
    o->bo[i].handle = handle;
    return true;
}
#endif

static bool _dumb_bo_make(struct vt_kms *k, _out_t *o, int i) {
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = (uint32_t)o->w;
    creq.height = (uint32_t)o->h;
    creq.bpp = 32;
    if (ioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        vt_logw("kms: CREATE_DUMB(%dx%d) failed: %s",
                o->w, o->h, strerror(errno));
        return false;
    }
    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (ioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        vt_logw("kms: MAP_DUMB failed: %s", strerror(errno));
        struct drm_mode_destroy_dumb d;
        memset(&d, 0, sizeof(d));
        d.handle = creq.handle;
        ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return false;
    }
    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     k->fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        vt_logw("kms: mmap dumb buffer failed: %s", strerror(errno));
        struct drm_mode_destroy_dumb d;
        memset(&d, 0, sizeof(d));
        d.handle = creq.handle;
        ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return false;
    }
    uint32_t fb = 0;
    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t strides[4] = { creq.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(k->fd, o->w, o->h, DRM_FORMAT_XRGB8888,
                      handles, strides, offsets, &fb, 0) < 0 || !fb) {
        vt_logw("kms: drmModeAddFB2 for dumb bo failed: %s",
                strerror(errno));
        munmap(map, creq.size);
        struct drm_mode_destroy_dumb d;
        memset(&d, 0, sizeof(d));
        d.handle = creq.handle;
        ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return false;
    }
    o->bo[i].map = map;
    o->bo[i].stride = creq.pitch;
    o->bo[i].size = creq.size;
    o->bo[i].handle = creq.handle;
    o->bo[i].fb_id = fb;
    return true;
}

static bool _out_buffers_make(struct vt_kms *k, _out_t *o) {
    for (int i = 0; i < 2; i++) {
        bool ok = false;
#if defined(VT_HAVE_GBM)
        if (k->use_gbm && k->gbm_dev)
            ok = _gbm_bo_make(k, o, i);
#endif
        if (!ok && k->how != VT_KMS_SCANOUT_GBM) {
            if (k->use_gbm)
                vt_logw("kms: GBM buffer %d unusable — dumb-buffer "
                        "fallback for this output", i);
            ok = _dumb_bo_make(k, o, i);
        }
        if (!ok) return false;
    }
    o->front = 0;
    o->pending = -1;
    return true;
}

/* ------------------------------------------------------------ EGL probe */

#if defined(VT_HAVE_EGL)
static void _egl_probe(struct vt_kms *k) {
    EGLDisplay dpy = EGL_NO_DISPLAY;
#if defined(VT_HAVE_GBM) && defined(EGL_PLATFORM_GBM_KHR)
    if (k->gbm_dev) {
        dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,
                                    (void *)k->gbm_dev, NULL);
        if (dpy == EGL_NO_DISPLAY)
            vt_logd("kms: EGL GBM platform display unavailable");
    }
#endif
#ifdef EGL_PLATFORM_SURFACELESS_MESA
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                    (void *)EGL_DEFAULT_DISPLAY, NULL);
        if (dpy == EGL_NO_DISPLAY)
            vt_logd("kms: EGL surfaceless platform unavailable");
    }
#endif
    if (dpy == EGL_NO_DISPLAY) {
        snprintf(k->renderer, sizeof(k->renderer),
                 "software (CPU) — no EGL display");
        return;
    }
    EGLint maj = 0, min = 0;
    if (eglInitialize(dpy, &maj, &min) != EGL_TRUE) {
        vt_logd("kms: eglInitialize failed");
        snprintf(k->renderer, sizeof(k->renderer),
                 "software (CPU) — eglInitialize failed");
        return;
    }
    k->egl_dpy = dpy;
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        snprintf(k->renderer, sizeof(k->renderer),
                 "software (CPU) — GLES bind failed (EGL %d.%d)", maj, min);
        return;
    }
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR,
                                      EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        EGLConfig cfg;
        EGLint nc = 0;
        if (eglChooseConfig(dpy, NULL, &cfg, 1, &nc) == EGL_TRUE && nc > 0)
            ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    }
    if (ctx == EGL_NO_CONTEXT) {
        snprintf(k->renderer, sizeof(k->renderer),
                 "software (CPU) — no GLES context (EGL %d.%d)", maj, min);
        return;
    }
    k->egl_ctx = ctx;
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) ==
        EGL_TRUE) {
        const char *r = NULL;
#if defined(VT_HAVE_GLES)
        r = (const char *)glGetString(GL_RENDERER);
#endif
        snprintf(k->renderer, sizeof(k->renderer), "%s (EGL %d.%d, GLES2)",
                 (r && *r) ? r : "unknown", maj, min);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    } else {
        snprintf(k->renderer, sizeof(k->renderer),
                 "software (CPU) — EGL context not currentable");
    }
}
#endif

/* ------------------------------------------------------------ open/close */

static const char *_conn_name(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA:        return "VGA";
    case DRM_MODE_CONNECTOR_DVII:       return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:       return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA:       return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite:  return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO:     return "S-Video";
    case DRM_MODE_CONNECTOR_LVDS:       return "LVDS";
    case DRM_MODE_CONNECTOR_Component:  return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN:    return "9-Pin-DIN";
    case DRM_MODE_CONNECTOR_DisplayPort:return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:      return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:      return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:         return "TV";
    case DRM_MODE_CONNECTOR_eDP:        return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL:    return "Virtual";
    case DRM_MODE_CONNECTOR_DSI:        return "DSI";
    case DRM_MODE_CONNECTOR_DPI:        return "DPI";
    case DRM_MODE_CONNECTOR_WRITEBACK:  return "Writeback";
    case DRM_MODE_CONNECTOR_SPI:        return "SPI";
    case DRM_MODE_CONNECTOR_USB:        return "USB";
    default:                            return "UNK";
    }
}

vt_kms_t *vt_kms_open(const char *card_path, vt_seat_t *seat,
                      vt_kms_scanout_t how, vt_kms_status_t *status) {
    if (status) *status = VT_KMS_OK;
#if defined(VT_HAVE_LIBDRM)
    struct vt_kms *k = vt_malloc0(sizeof(*k));
    k->how = how;
    k->seat = seat;

    vt_logi("kms: opening %s (%s)", card_path,
            seat ? "through the seat" : "direct open");

    if (seat) {
        k->fd = vt_seat_open_device(seat, card_path);
        k->fd_via_seat = true;
    } else {
        k->fd = open(card_path, O_RDWR | O_CLOEXEC);
    }
    if (k->fd < 0) {
        vt_logw("kms: cannot open %s: %s", card_path, strerror(errno));
        if (status) *status = VT_KMS_NO_CARDS;
        vt_free(k);
        return NULL;
    }

    /* DRM master (required to mode-set) */
    if (vt_seat_drm_set_master(seat, k->fd) == 0) {
        k->master = true;
        vt_logi("kms: DRM master acquired");
    } else {
        /* logind grants master only once the session is ACTIVE; retry
         * briefly, then decide honestly. */
        bool ok = false;
        for (int i = 0; i < 5 && !ok; i++) {
            vt_time_sleep_ms(100);
            if (vt_seat_drm_set_master(seat, k->fd) == 0) ok = true;
        }
        if (!ok && seat && vt_seat_is_active(seat)) {
            vt_logw("kms: drmSetMaster kept failing — trying mode-set "
                    "anyway (may still work with logind access)");
            k->master = true;
        } else if (!ok) {
            vt_loge("kms: could not become DRM master on %s — the "
                    "session/VT is not active (start from a TTY or via "
                    "the session manager)", card_path);
            if (status) *status = VT_KMS_NO_MASTER;
            close(k->fd);
            vt_free(k);
            return NULL;
        }
    }

    drmModeResPtr res = drmModeGetResources(k->fd);
    if (!res) {
        vt_loge("kms: drmModeGetResources failed: %s", strerror(errno));
        if (status) *status = VT_KMS_NO_CONNECTED;
        close(k->fd);
        vt_free(k);
        return NULL;
    }

    /* GBM device (optional) */
#if defined(VT_HAVE_GBM)
    if (how != VT_KMS_SCANOUT_DUMB) {
        k->gbm_dev = gbm_create_device(k->fd);
        if (k->gbm_dev) {
            k->use_gbm = true;
            vt_logi("kms: GBM device created");
        } else {
            vt_logw("kms: gbm_create_device failed: %s — dumb buffers "
                    "instead", strerror(errno));
        }
    }
#endif

    /* outputs: one per connected connector (up to VT_KMS_MAX_OUT) */
    uint32_t used_crtcs = 0;
    for (int ci = 0; ci < res->count_connectors &&
                     k->n_out < VT_KMS_MAX_OUT; ci++) {
        drmModeConnectorPtr conn =
            drmModeGetConnector(k->fd, res->connectors[ci]);
        if (!conn) continue;
        const char *st = conn->connection == DRM_MODE_CONNECTED ?
                         "connected" :
                         conn->connection == DRM_MODE_DISCONNECTED ?
                         "disconnected" : "unknown";
        vt_logi("kms: connector %s-%u: %s (%d modes)",
                _conn_name(conn->connector_type),
                conn->connector_type_id, st, conn->count_modes);
        if (conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            continue;
        }
        drmModeModeInfoPtr mode = _pick_mode(conn);
        if (!mode) {
            vt_logw("kms: %s-%u connected but reports no modes — skipping",
                    _conn_name(conn->connector_type),
                    conn->connector_type_id);
            drmModeFreeConnector(conn);
            continue;
        }

        /* CRTC: reuse the one currently driving this connector when
         * possible, else a free one allowed by possible_crtcs. */
        uint32_t crtc_id = 0;
        if (conn->encoder_id) {
            drmModeEncoderPtr enc = drmModeGetEncoder(k->fd,
                                                      conn->encoder_id);
            if (enc) {
                if (enc->crtc_id) crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        if (crtc_id) {
            /* do not reuse a CRTC already claimed by another output
             * (some firmware mirrors two connectors onto one CRTC) */
            int idx = -1;
            for (int cj = 0; cj < res->count_crtcs; cj++)
                if (res->crtcs[cj] == crtc_id) { idx = cj; break; }
            if (idx < 0 || (used_crtcs & (1u << idx))) crtc_id = 0;
        }
        if (!crtc_id) {
            for (int ei = 0; ei < conn->count_encoders && !crtc_id; ei++) {
                drmModeEncoderPtr enc =
                    drmModeGetEncoder(k->fd, conn->encoders[ei]);
                if (!enc) continue;
                for (int cj = 0; cj < res->count_crtcs; cj++) {
                    uint32_t mask = 1u << cj;
                    if (!(enc->possible_crtcs & mask)) continue;
                    if (used_crtcs & mask) continue;
                    crtc_id = res->crtcs[cj];
                    break;
                }
                drmModeFreeEncoder(enc);
            }
        }
        if (!crtc_id) {
            vt_logw("kms: no free CRTC for %s-%u — output unusable",
                    _conn_name(conn->connector_type),
                    conn->connector_type_id);
            drmModeFreeConnector(conn);
            continue;
        }
        for (int cj = 0; cj < res->count_crtcs; cj++)
            if (res->crtcs[cj] == crtc_id) used_crtcs |= (1u << cj);

        _out_t *o = &k->outs[k->n_out];
        o->conn_id = conn->connector_id;
        o->crtc_id = crtc_id;
        snprintf(o->name, sizeof(o->name), "%s-%u",
                 _conn_name(conn->connector_type),
                 conn->connector_type_id);
        o->w = mode->hdisplay;
        o->h = mode->vdisplay;
        o->refresh_khz = mode->vrefresh / 1000;
        o->refresh_frac = (mode->vrefresh % 1000) / 10;
        o->saved = drmModeGetCrtc(k->fd, crtc_id);
        o->pending = -1;
        vt_logi("kms: output %s: mode %dx%d@%u.%02u on CRTC %u",
                o->name, o->w, o->h, o->refresh_khz, o->refresh_frac,
                crtc_id);
        k->n_out++;
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);

    if (k->n_out == 0) {
        vt_loge("kms: no usable output (no connected connector with a "
                "free CRTC and modes)");
        if (status) *status = VT_KMS_NO_CONNECTED;
        vt_kms_close(k);
        return NULL;
    }

    /* scanout buffers */
    for (int i = 0; i < k->n_out; i++) {
        if (!_out_buffers_make(k, &k->outs[i])) {
            vt_loge("kms: could not create scanout buffers for %s",
                    k->outs[i].name);
            if (status) *status = VT_KMS_NO_SCANOUT;
            vt_kms_close(k);
            return NULL;
        }
        const char *bo_kind = "dumb";
#if defined(VT_HAVE_GBM)
        if (k->outs[i].bo[0].bo) bo_kind = "gbm";
#endif
        vt_logi("kms: %s: 2x %s scanout buffer (%dx%d, stride %u, fb %u)",
                k->outs[i].name, bo_kind,
                k->outs[i].w, k->outs[i].h, k->outs[i].bo[0].stride,
                k->outs[i].bo[0].fb_id);
    }

    /* hardware cursor plane */
    _cursor_init(k);

    /* EGL renderer probe (honest reporting only) */
#if defined(VT_HAVE_EGL)
    _egl_probe(k);
#else
    snprintf(k->renderer, sizeof(k->renderer),
             "software (CPU) — built without EGL");
#endif
    vt_logi("kms: renderer: %s", k->renderer);
    return k;
#else
    (void)card_path; (void)seat; (void)how;
    if (status) *status = VT_KMS_NO_CARDS;
    return NULL;
#endif /* VT_HAVE_LIBDRM */
}

/* ------------------------------------------------------------ cursor */

static void _cursor_init(struct vt_kms *k) {
#if defined(VT_HAVE_LIBDRM)
    const int sz = 64;
    bool ok = false;
#if defined(VT_HAVE_GBM)
    if (k->use_gbm && k->gbm_dev) {
        struct gbm_bo *bo = gbm_bo_create(
            k->gbm_dev, sz, sz, GBM_FORMAT_ARGB8888,
            GBM_BO_USE_CURSOR_64X64 | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
        if (bo) {
            void *md = NULL;
            uint32_t stride = 0;
            uint32_t *map = gbm_bo_map(bo, 0, 0, 0, 0,
                                       GBM_BO_TRANSFER_WRITE,
                                       &stride, &md);
            if (map) {
                k->cursor_bo = bo;
                k->cursor_map_data = md;
                k->cur_handle = gbm_bo_get_handle(bo).u32;
                k->cur_map = map;
                k->cur_stride = stride;
                k->cur_w = k->cur_h = sz;
                k->cur_size = (uint64_t)stride * sz;
                ok = true;
            } else {
                vt_logd("kms: cursor gbm_bo_map failed");
                gbm_bo_destroy(bo);
            }
        } else {
            vt_logd("kms: gbm cursor bo unavailable: %s",
                    strerror(errno));
        }
    }
#endif
    if (!ok) {
        struct drm_mode_create_dumb creq;
        memset(&creq, 0, sizeof(creq));
        creq.width = (uint32_t)sz;
        creq.height = (uint32_t)sz;
        creq.bpp = 32;
        if (ioctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) == 0) {
            struct drm_mode_map_dumb mreq;
            memset(&mreq, 0, sizeof(mreq));
            mreq.handle = creq.handle;
            if (ioctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0) {
                uint32_t *map = mmap(NULL, creq.size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     k->fd, (off_t)mreq.offset);
                if (map != MAP_FAILED) {
                    k->cur_handle = creq.handle;
                    k->cur_map = map;
                    k->cur_stride = (uint32_t)sz * 4;
                    k->cur_w = k->cur_h = sz;
                    k->cur_size = creq.size;
                    ok = true;
                }
            }
            if (!ok) {
                struct drm_mode_destroy_dumb d;
                memset(&d, 0, sizeof(d));
                d.handle = creq.handle;
                ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            }
        }
    }
    k->hw_cursor = ok;
    k->cursor_on = false;
    vt_logi("kms: hardware cursor plane %s",
            ok ? "available (64x64 ARGB)" :
                 "unavailable — software cursor sprite instead");
#endif
}

/* ------------------------------------------------------------ mode set */

#if defined(VT_HAVE_LIBDRM)
static drmModeModeInfoPtr _pick_mode(drmModeConnectorPtr conn) {
    if (conn->count_modes <= 0) return NULL;
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            return &conn->modes[i];
    /* biggest mode below 1920x1200, else the first */
    drmModeModeInfoPtr best = &conn->modes[0];
    for (int i = 1; i < conn->count_modes; i++) {
        drmModeModeInfoPtr m = &conn->modes[i];
        if (m->hdisplay > 1920 || m->vdisplay > 1200) continue;
        if (m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay)
            best = m;
    }
    return best;
}

/* set CRTC with the connector's (re-fetched) mode; NULL mode = keep */
static int _set_crtc(struct vt_kms *k, _out_t *o, uint32_t fb,
                     drmModeModeInfoPtr mode) {
    if (mode)
        return drmModeSetCrtc(k->fd, o->crtc_id, fb, 0, 0,
                              &o->conn_id, 1, mode);
    return drmModeSetCrtc(k->fd, o->crtc_id, fb, 0, 0,
                          &o->conn_id, 1, NULL);
}

static int _set_crtc_mode(struct vt_kms *k, _out_t *o, uint32_t fb) {
    int rc = _set_crtc(k, o, fb, NULL);   /* keep current mode */
    if (rc == 0) return 0;
    drmModeConnectorPtr conn = drmModeGetConnector(k->fd, o->conn_id);
    if (conn) {
        drmModeModeInfoPtr mode = _pick_mode(conn);
        rc = _set_crtc(k, o, fb, mode);
        drmModeFreeConnector(conn);
    }
    return rc;
}
#endif

int vt_kms_start(vt_kms_t *k, vt_kms_scanout_fn first_scanout, void *ud) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || k->n_out == 0) return -1;
    k->first_scanout = first_scanout;
    k->scanout_ud = ud;
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        if (o->live) continue;
        if (_set_crtc_mode(k, o, o->bo[o->front].fb_id) < 0) {
            vt_loge("kms: drmModeSetCrtc(%s) failed: %s",
                    o->name, strerror(errno));
            return -1;
        }
        o->live = true;
        vt_logi("kms: %s: scanout live (fb %u)", o->name,
                o->bo[o->front].fb_id);
    }
    k->started = true;
    if (k->first_scanout)
        k->first_scanout(k, k->scanout_ud);
    return 0;
#else
    (void)k; (void)first_scanout; (void)ud;
    return -1;
#endif
}

/* ------------------------------------------------------------ present */

#if defined(VT_HAVE_LIBDRM)
static void _flip_done_cb(int fd, unsigned int seq, unsigned int tv_sec,
                          unsigned int tv_usec, unsigned int crtc_id,
                          void *ud) {
    (void)fd; (void)seq; (void)tv_sec; (void)tv_usec;
    struct vt_kms *k = ud;
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        if (o->crtc_id != crtc_id || o->pending < 0) continue;
        o->front = o->pending;
        o->pending = -1;
        if (o->needs_flip) {
            o->needs_flip = false;
            _flip_rearm(k, o);
        }
    }
    if (k->flip_done)
        k->flip_done(k->flip_ud);
}
#endif

static void _flip_rearm(struct vt_kms *k, _out_t *o) {
#if defined(VT_HAVE_LIBDRM)
    int back = 1 - o->front;
    if (drmModePageFlip(k->fd, o->crtc_id, o->bo[back].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, k) < 0) {
        if (errno == EACCES) {
            /* master revoked (VT switched away): the resume path
             * re-arms the CRTC with the current front buffer */
            o->pending = -1;
            o->needs_flip = false;
            return;
        }
        vt_logw("kms: page flip on %s failed: %s — retry next frame",
                o->name, strerror(errno));
        o->pending = -1;
        return;
    }
    o->pending = back;
#else
    (void)k; (void)o;
#endif
}

void vt_kms_present(vt_kms_t *k, const uint32_t *pixels, int w, int h) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || !k->started || !pixels) return;
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        void *dst = o->bo[1 - o->front].map;
        if (!dst) continue;
        if (o->w == w && o->bo[1 - o->front].stride == (uint32_t)(w * 4)) {
            memcpy(dst, pixels, (size_t)w * h * 4);
        } else {
            /* clone with differing mode/stride: row-by-row copy */
            int cw = o->w < w ? o->w : w;
            int ch = o->h < h ? o->h : h;
            for (int y = 0; y < ch; y++)
                memcpy((uint8_t *)dst +
                       (size_t)y * o->bo[1 - o->front].stride,
                       pixels + (size_t)y * w, (size_t)cw * 4);
        }
        if (o->pending >= 0)
            o->needs_flip = true;   /* re-armed at flip completion */
        else
            _flip_rearm(k, o);
    }
#else
    (void)k; (void)pixels; (void)w; (void)h;
#endif
}

void vt_kms_prime(vt_kms_t *k, const uint32_t *pixels, int w, int h) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || !pixels) return;
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        for (int b = 0; b < 2; b++) {
            void *dst = o->bo[b].map;
            if (!dst) continue;
            int cw = o->w < w ? o->w : w;
            int ch = o->h < h ? o->h : h;
            for (int y = 0; y < ch; y++)
                memcpy((uint8_t *)dst + (size_t)y * o->bo[b].stride,
                       pixels + (size_t)y * w, (size_t)cw * 4);
        }
    }
#else
    (void)k; (void)pixels; (void)w; (void)h;
#endif
}

void vt_kms_handle_events(vt_kms_t *k) {
#if defined(VT_HAVE_LIBDRM)
    if (!k) return;
    drmEventContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.version = 3;
    ctx.page_flip_handler2 = _flip_done_cb;
    drmHandleEvent(k->fd, &ctx);
#endif
}

/* ------------------------------------------------------------ cursor api */

bool vt_kms_cursor_set(vt_kms_t *k, const uint32_t *argb, int w, int h) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || !k->hw_cursor || !k->cur_map || !argb) return false;
    if (w > k->cur_w || h > k->cur_h || w <= 0 || h <= 0) return false;
    memset(k->cur_map, 0, (size_t)k->cur_size);
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)k->cur_map + (size_t)y * k->cur_stride,
               argb + (size_t)y * w, (size_t)w * 4);
    for (int i = 0; i < k->n_out; i++) {
        if (drmModeSetCursor(k->fd, k->outs[i].crtc_id, k->cur_handle,
                             (uint32_t)k->cur_w, (uint32_t)k->cur_h) < 0) {
            vt_logw("kms: drmModeSetCursor failed: %s — falling back to "
                    "software cursor", strerror(errno));
            k->hw_cursor = false;
            return false;
        }
    }
    k->cursor_on = true;
    return true;
#else
    (void)k; (void)argb; (void)w; (void)h;
    return false;
#endif
}

void vt_kms_cursor_move(vt_kms_t *k, int x, int y) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || !k->cursor_on) return;
    for (int i = 0; i < k->n_out; i++)
        drmModeMoveCursor(k->fd, k->outs[i].crtc_id, x, y);
#else
    (void)k; (void)x; (void)y;
#endif
}

void vt_kms_cursor_hide(vt_kms_t *k) {
#if defined(VT_HAVE_LIBDRM)
    if (!k || !k->cursor_on) return;
    for (int i = 0; i < k->n_out; i++)
        drmModeSetCursor(k->fd, k->outs[i].crtc_id, 0, 0, 0);
    k->cursor_on = false;
#else
    (void)k;
#endif
}

/* ------------------------------------------------------------ lifecycle */

void vt_kms_pause(vt_kms_t *k) {
    if (!k) return;
    vt_logi("kms: pausing (VT switch away): dropping DRM master");
    for (int i = 0; i < k->n_out; i++) {
        k->outs[i].pending = -1;
        k->outs[i].needs_flip = false;
    }
    vt_seat_drm_drop_master(k->seat, k->fd);
    k->master = false;
}

int vt_kms_resume(vt_kms_t *k) {
#if defined(VT_HAVE_LIBDRM)
    if (!k) return -1;
    vt_logi("kms: resuming: retaking DRM master");
    bool ok = false;
    for (int i = 0; i < 20 && !ok; i++) {
        if (vt_seat_drm_set_master(k->seat, k->fd) == 0) ok = true;
        else vt_time_sleep_ms(100);   /* EACCES until logind completes */
    }
    if (!ok) {
        vt_loge("kms: could not retake DRM master on resume");
        return -1;
    }
    k->master = true;
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        if (_set_crtc_mode(k, o, o->bo[o->front].fb_id) < 0) {
            vt_loge("kms: re-arm CRTC on %s failed: %s",
                    o->name, strerror(errno));
            return -1;
        }
        o->pending = -1;
        o->needs_flip = false;
    }
    if (k->cursor_on && k->hw_cursor) {
        for (int i = 0; i < k->n_out; i++)
            drmModeSetCursor(k->fd, k->outs[i].crtc_id, k->cur_handle,
                             (uint32_t)k->cur_w, (uint32_t)k->cur_h);
    }
    vt_logi("kms: resumed — scanout re-armed");
    return 0;
#else
    (void)k;
    return -1;
#endif
}

void vt_kms_close(vt_kms_t *k) {
    if (!k) return;
    vt_logi("kms: closing");
#if defined(VT_HAVE_EGL)
    if (k->egl_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(k->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(k->egl_dpy, k->egl_ctx);
        k->egl_ctx = EGL_NO_CONTEXT;
    }
    if (k->egl_dpy != EGL_NO_DISPLAY) {
        eglTerminate(k->egl_dpy);
        k->egl_dpy = EGL_NO_DISPLAY;
    }
#endif
#if defined(VT_HAVE_LIBDRM)
    if (k->cursor_on) {
        for (int i = 0; i < k->n_out; i++)
            drmModeSetCursor(k->fd, k->outs[i].crtc_id, 0, 0, 0);
        k->cursor_on = false;
    }
#endif
#if defined(VT_HAVE_GBM)
    if (k->cursor_bo) {
        gbm_bo_unmap(k->cursor_bo, k->cursor_map_data);
        gbm_bo_destroy(k->cursor_bo);
        k->cursor_bo = NULL;
        k->cur_map = NULL;        /* GBM-owned */
    }
#endif
    if (k->cur_map) {
        munmap(k->cur_map, (size_t)k->cur_size);
        k->cur_map = NULL;
        if (k->cur_handle) {
            struct drm_mode_destroy_dumb d;
            memset(&d, 0, sizeof(d));
            d.handle = k->cur_handle;
#if defined(VT_HAVE_LIBDRM)
            ioctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
#endif
        }
    }
#if defined(VT_HAVE_LIBDRM)
    for (int i = 0; i < k->n_out; i++) {
        _out_t *o = &k->outs[i];
        if (o->saved) {
            if (o->saved->mode_valid && o->saved->buffer_id) {
                if (drmModeSetCrtc(k->fd, o->crtc_id, o->saved->buffer_id,
                                   o->saved->x, o->saved->y,
                                   &o->conn_id, 1, &o->saved->mode) < 0)
                    vt_logw("kms: restore saved CRTC on %s failed: %s",
                            o->name, strerror(errno));
            }
            drmModeFreeCrtc(o->saved);
            o->saved = NULL;
        }
        _bo_free(o, 0, k->fd);
        _bo_free(o, 1, k->fd);
    }
#endif
#if defined(VT_HAVE_GBM)
    if (k->gbm_dev) {
        gbm_device_destroy(k->gbm_dev);
        k->gbm_dev = NULL;
    }
#endif
    if (k->fd >= 0) {
        if (k->seat && k->fd_via_seat) vt_seat_close_device(k->seat, k->fd);
        else close(k->fd);
        k->fd = -1;
    }
    vt_free(k);
}

/* ------------------------------------------------------------ info api */

int vt_kms_output_count(const vt_kms_t *k) { return k ? k->n_out : 0; }
const char *vt_kms_out_name(const vt_kms_t *k, int i) {
    return (k && i >= 0 && i < k->n_out) ? k->outs[i].name : "?";
}
int vt_kms_out_width(const vt_kms_t *k, int i) {
    return (k && i >= 0 && i < k->n_out) ? k->outs[i].w : 0;
}
int vt_kms_out_height(const vt_kms_t *k, int i) {
    return (k && i >= 0 && i < k->n_out) ? k->outs[i].h : 0;
}
int vt_kms_out_refresh(const vt_kms_t *k, int i) {
    return (k && i >= 0 && i < k->n_out)
        ? (int)k->outs[i].refresh_khz : 0;
}
const char *vt_kms_renderer(const vt_kms_t *k) {
    return k ? k->renderer : "none";
}
bool vt_kms_hw_cursor(const vt_kms_t *k) {
    return k ? k->hw_cursor : false;
}
const char *vt_kms_scanout_str(const vt_kms_t *k) {
    if (!k) return "none";
#if defined(VT_HAVE_GBM)
    if (k->use_gbm && k->outs[0].bo[0].bo) return "gbm";
#endif
    return "dumb";
}
int vt_kms_fd(const vt_kms_t *k) { return k ? k->fd : -1; }

const char *vt_kms_status_str(vt_kms_status_t st) {
    switch (st) {
    case VT_KMS_OK:           return "ok";
    case VT_KMS_NO_CARDS:     return "no DRM cards";
    case VT_KMS_NO_CONNECTED: return "no connected output";
    case VT_KMS_NO_CRTC:      return "no free CRTC";
    case VT_KMS_NO_MASTER:    return "DRM master refused";
    case VT_KMS_NO_SCANOUT:   return "no usable scanout buffers";
    default:                  return "unknown";
    }
}
