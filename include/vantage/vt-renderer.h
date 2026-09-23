/*
 * vt-renderer.h — Vantage renderer abstraction
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides a backend-agnostic rendering API used by the compositor,
 * wallpaper engine, and panel. Available renderers:
 *
 *   - vt_renderer_gl   : OpenGL 4.x core / OpenGL ES 3.x via EGL
 *   - vt_renderer_sw    : software rasterizer (always built)
 *   - vt_renderer_vulkan: optional Vulkan renderer (advanced)
 *
 * The compositor picks the highest-priority renderer available at runtime,
 * falling back to software if no GPU acceleration is available.
 */
#ifndef VANTAGE_RENDERER_H
#define VANTAGE_RENDERER_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_RENDERER_AUTO = 0,
    VT_RENDERER_GL,
    VT_RENDERER_VULKAN,
    VT_RENDERER_SW,
    VT_RENDERER_INVALID,
} vt_renderer_kind_t;

typedef enum {
    VT_PF_UNKNOWN = 0,
    VT_PF_ARGB8888,
    VT_PF_XRGB8888,
    VT_PF_ABGR8888,
    VT_PF_RGB565,
    VT_PF_RGBA1010102,
} vt_pixel_format_t;

typedef struct vt_renderer   vt_renderer_t;
typedef struct vt_renderer_caps vt_renderer_caps_t;
typedef struct vt_texture     vt_texture_t;
typedef struct vt_buffer      vt_buffer_t;

struct vt_renderer_caps {
    bool      hw_accel;
    bool      vsync;
    bool      shaders;
    bool      blur;
    bool      video_decode;
    int        max_texture_size;
    int        texture_units;
    char       vendor[64];
    char       renderer[128];
    char       version[64];
    char       glsl_version[64];
};

struct vt_texture {
    uint32_t  id;          /* renderer-private handle */
    uint32_t  w, h;
    vt_pixel_format_t fmt;
    bool      owns_pixels;
    void     *pixels;      /* for software renderer */
    bool     *dirty;       /* renderer private */
};

typedef struct vt_rect { int x, y, w, h; } vt_rect_t;
typedef struct vt_color { float r, g, b, a; } vt_color_t;

typedef struct vt_renderer_vt_ops {
    int  (*init)(vt_renderer_t *r);
    void (*fini)(vt_renderer_t *r);
    void (*begin)(vt_renderer_t *r, int w, int h);
    void (*end)(vt_renderer_t *r);
    void (*clear)(vt_renderer_t *r, vt_color_t c);
    vt_texture_t *(*texture_create)(vt_renderer_t *r, uint32_t w, uint32_t h,
                                      vt_pixel_format_t fmt);
    void (*texture_upload)(vt_renderer_t *r, vt_texture_t *t,
                             const void *pixels);
    void (*texture_free)(vt_renderer_t *r, vt_texture_t *t);
    void (*texture_draw)(vt_renderer_t *r, vt_texture_t *t, vt_rect_t dst,
                          vt_rect_t *src, vt_color_t *tint);
    void (*fill_rect)(vt_renderer_t *r, vt_rect_t rct, vt_color_t c);
    void (*blit)(vt_renderer_t *r, vt_texture_t *src, vt_rect_t dst, vt_rect_t src_rect);
    void (*present)(vt_renderer_t *r);
    bool (*probe)(vt_renderer_caps_t *caps);
    const char *(*name)(void);
} vt_renderer_ops_t;

struct vt_renderer {
    const vt_renderer_ops_t *ops;
    vt_renderer_kind_t kind;
    vt_renderer_caps_t caps;
    void *priv;          /* backend state */
    bool initialized;
};

vt_renderer_t *vt_renderer_new(vt_renderer_kind_t preferred);
void           vt_renderer_free(vt_renderer_t *r);
int            vt_renderer_init(vt_renderer_t *r);
bool           vt_renderer_probe(vt_renderer_kind_t kind,
                                  vt_renderer_caps_t *out);
const char    *vt_renderer_name(const vt_renderer_t *r);
const char    *vt_renderer_kind_str(vt_renderer_kind_t k);

/* Convenience wrappers */
void  vt_renderer_begin(vt_renderer_t *r, int w, int h);
void  vt_renderer_end(vt_renderer_t *r);
void  vt_renderer_clear(vt_renderer_t *r, vt_color_t c);
void  vt_renderer_fill_rect(vt_renderer_t *r, vt_rect_t rct, vt_color_t c);
void  vt_renderer_present(vt_renderer_t *r);
vt_texture_t *vt_renderer_texture_create(vt_renderer_t *r, uint32_t w, uint32_t h,
                                          vt_pixel_format_t fmt);
void  vt_renderer_texture_upload(vt_renderer_t *r, vt_texture_t *t, const void *pixels);
void  vt_renderer_texture_draw(vt_renderer_t *r, vt_texture_t *t, vt_rect_t dst,
                                vt_rect_t *src, vt_color_t *tint);
void  vt_renderer_texture_free(vt_renderer_t *r, vt_texture_t *t);

#ifdef __cplusplus
}
#endif
#endif
