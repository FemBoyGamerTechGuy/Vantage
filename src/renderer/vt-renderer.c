/*
 * vt-renderer.c — Vantage renderer abstraction
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides a backend-agnostic renderer. Probes available backends at
 * runtime (opengl, vulkan, software) and selects the best one. Renderers
 * are looked up by priority and fall back to software if no hardware
 * acceleration is available.
 */

#define VT_LOG_DOMAIN "renderer"
#include <vantage/vt-renderer.h>
#include <string.h>
#include <stdlib.h>

/* Software renderer (always compiled) */
extern const vt_renderer_ops_t vt_renderer_sw_ops;

#if defined(VT_HAVE_OPENGL) && defined(VT_HAVE_EGL)
extern const vt_renderer_ops_t vt_renderer_gl_ops;
#endif

#if defined(VT_HAVE_VULKAN)
extern const vt_renderer_ops_t vt_renderer_vulkan_ops;
#endif

static const vt_renderer_ops_t *const _renderers[] = {
#if defined(VT_HAVE_OPENGL) && defined(VT_HAVE_EGL)
    &vt_renderer_gl_ops,
#endif
#if defined(VT_HAVE_VULKAN)
    &vt_renderer_vulkan_ops,
#endif
    &vt_renderer_sw_ops,
};

static const size_t _renderers_n =
    sizeof(_renderers) / sizeof(_renderers[0]);

vt_renderer_t *vt_renderer_new(vt_renderer_kind_t preferred) {
    for (size_t i = 0; i < _renderers_n; i++) {
        const vt_renderer_ops_t *ops = _renderers[i];
        if (preferred != VT_RENDERER_AUTO) {
            vt_renderer_kind_t k = VT_RENDERER_AUTO;
            if (ops == &vt_renderer_sw_ops) k = VT_RENDERER_SW;
#if defined(VT_HAVE_OPENGL) && defined(VT_HAVE_EGL)
            else if (ops == &vt_renderer_gl_ops) k = VT_RENDERER_GL;
#endif
            if (k != preferred) continue;
        }
        vt_renderer_caps_t caps;
        if (!ops->probe(&caps)) continue;
        vt_renderer_t *r = vt_malloc0(sizeof(*r));
        r->ops = ops;
        r->kind = (ops == &vt_renderer_sw_ops) ? VT_RENDERER_SW :
#if defined(VT_HAVE_OPENGL) && defined(VT_HAVE_EGL)
                  (ops == &vt_renderer_gl_ops) ? VT_RENDERER_GL :
#endif
#if defined(VT_HAVE_VULKAN)
                  (ops == &vt_renderer_vulkan_ops) ? VT_RENDERER_VULKAN :
#endif
                  VT_RENDERER_INVALID;
        r->caps = caps;
        r->initialized = false;
        return r;
    }
    /* fallback to software with stub caps */
    vt_renderer_t *r = vt_malloc0(sizeof(*r));
    r->ops = &vt_renderer_sw_ops;
    r->kind = VT_RENDERER_SW;
    memset(&r->caps, 0, sizeof(r->caps));
    r->caps.hw_accel = false;
    r->caps.max_texture_size = 4096;
    r->caps.texture_units = 1;
    snprintf(r->caps.vendor, sizeof(r->caps.vendor), "Vantage");
    snprintf(r->caps.renderer, sizeof(r->caps.renderer), "software");
    snprintf(r->caps.version, sizeof(r->caps.version), "1.0");
    snprintf(r->caps.glsl_version, sizeof(r->caps.glsl_version), "n/a");
    return r;
}

void vt_renderer_free(vt_renderer_t *r) {
    if (!r) return;
    if (r->initialized && r->ops && r->ops->fini) r->ops->fini(r);
    vt_free(r);
}

int vt_renderer_init(vt_renderer_t *r) {
    if (!r || !r->ops) return VT_ERR_INVAL;
    if (r->initialized) return VT_OK;
    if (!r->ops->init) return VT_ERR_NOTSUPP;
    int rc = r->ops->init(r);
    if (rc == 0) r->initialized = true;
    return rc;
}

bool vt_renderer_probe(vt_renderer_kind_t kind, vt_renderer_caps_t *out) {
    for (size_t i = 0; i < _renderers_n; i++) {
        const vt_renderer_ops_t *ops = _renderers[i];
        vt_renderer_kind_t k = (ops == &vt_renderer_sw_ops) ? VT_RENDERER_SW :
#if defined(VT_HAVE_OPENGL) && defined(VT_HAVE_EGL)
                              (ops == &vt_renderer_gl_ops) ? VT_RENDERER_GL :
#endif
#if defined(VT_HAVE_VULKAN)
                              (ops == &vt_renderer_vulkan_ops) ? VT_RENDERER_VULKAN :
#endif
                              VT_RENDERER_INVALID;
        if (kind != VT_RENDERER_AUTO && k != kind) continue;
        if (ops->probe && ops->probe(out)) return true;
    }
    return false;
}

const uint8_t *vt_renderer_framebuffer(const vt_renderer_t *r, int *w, int *h) {
    if (!r || !r->priv) return NULL;
    if (r->kind != VT_RENDERER_SW) return NULL;
    /* _sw_state_t layout from vt-renderer-sw.c (w, h, buf) */
    struct { int w, h; uint8_t *buf; void *bound; } *s = (void *)r->priv;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return s->buf;
}

const char *vt_renderer_name(const vt_renderer_t *r) {
    return (r && r->ops && r->ops->name) ? r->ops->name() : "invalid";
}

const char *vt_renderer_kind_str(vt_renderer_kind_t k) {
    switch (k) {
    case VT_RENDERER_AUTO:   return "auto";
    case VT_RENDERER_GL:     return "opengl";
    case VT_RENDERER_VULKAN: return "vulkan";
    case VT_RENDERER_SW:     return "software";
    default:                 return "invalid";
    }
}

void vt_renderer_begin(vt_renderer_t *r, int w, int h) {
    if (r && r->ops && r->ops->begin) r->ops->begin(r, w, h);
}
void vt_renderer_end(vt_renderer_t *r) {
    if (r && r->ops && r->ops->end) r->ops->end(r);
}
void vt_renderer_clear(vt_renderer_t *r, vt_color_t c) {
    if (r && r->ops && r->ops->clear) r->ops->clear(r, c);
}
void vt_renderer_fill_rect(vt_renderer_t *r, vt_rect_t rct, vt_color_t c) {
    if (r && r->ops && r->ops->fill_rect) r->ops->fill_rect(r, rct, c);
}
void vt_renderer_present(vt_renderer_t *r) {
    if (r && r->ops && r->ops->present) r->ops->present(r);
}
vt_texture_t *vt_renderer_texture_create(vt_renderer_t *r, uint32_t w, uint32_t h,
                                          vt_pixel_format_t fmt) {
    if (!r || !r->ops || !r->ops->texture_create) return NULL;
    return r->ops->texture_create(r, w, h, fmt);
}
void vt_renderer_texture_upload(vt_renderer_t *r, vt_texture_t *t, const void *pixels) {
    if (r && r->ops && r->ops->texture_upload) r->ops->texture_upload(r, t, pixels);
}
void vt_renderer_texture_draw(vt_renderer_t *r, vt_texture_t *t, vt_rect_t dst,
                                vt_rect_t *src, vt_color_t *tint) {
    if (r && r->ops && r->ops->texture_draw) r->ops->texture_draw(r, t, dst, src, tint);
}
void vt_renderer_texture_free(vt_renderer_t *r, vt_texture_t *t) {
    if (r && r->ops && r->ops->texture_free) r->ops->texture_free(r, t);
}
