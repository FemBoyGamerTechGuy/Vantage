/*
 * vt-renderer-sw.c — Software rasterizer fallback
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Always built. Provides a simple CPU-side renderer with a single
 * in-memory framebuffer. Used when no GPU acceleration is available
 * (headless, console, no DRM/KMS).
 *
 * Performance: this is intentionally minimal — it draws solid rectangles
 * and copies rectangles of pixel data. No image scaling. It exists so
 * the desktop can boot and produce visible output even on systems
 * without GPU support; the OpenGL renderer takes over when available.
 */

#define VT_LOG_DOMAIN "renderer-sw"
#include <vantage/vt-renderer.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int w, h;
    uint8_t *buf;          /* RGBA8888, stride = w*4 */
    vt_texture_t *bound;
} _sw_state_t;

static bool _sw_probe(vt_renderer_caps_t *caps) {
    if (!caps) return false;
    memset(caps, 0, sizeof(*caps));
    caps->hw_accel = false;
    caps->vsync = false;
    caps->shaders = false;
    caps->blur = false;
    caps->video_decode = false;
    caps->max_texture_size = 8192;
    caps->texture_units = 1;
    snprintf(caps->vendor, sizeof(caps->vendor), "Vantage");
    snprintf(caps->renderer, sizeof(caps->renderer), "software");
    snprintf(caps->version, sizeof(caps->version), "1.0");
    snprintf(caps->glsl_version, sizeof(caps->glsl_version), "n/a");
    return true;
}

static int _sw_init(vt_renderer_t *r) {
    _sw_state_t *s = vt_malloc0(sizeof(*s));
    s->w = s->h = 0;
    s->buf = NULL;
    s->bound = NULL;
    r->priv = s;
    return 0;
}
static void _sw_fini(vt_renderer_t *r) {
    if (!r || !r->priv) return;
    _sw_state_t *s = r->priv;
    vt_free(s->buf);
    vt_free(s);
    r->priv = NULL;
}
static void _sw_begin(vt_renderer_t *r, int w, int h) {
    _sw_state_t *s = r->priv;
    if (s->w != w || s->h != h) {
        s->buf = vt_realloc(s->buf, (size_t)w * h * 4);
        s->w = w; s->h = h;
    }
}
static void _sw_end(vt_renderer_t *r) { (void)r; }
static void _sw_clear(vt_renderer_t *r, vt_color_t c) {
    _sw_state_t *s = r->priv;
    if (!s->buf) return;
    uint32_t col = ((uint32_t)(c.a * 255) << 24)
                 | ((uint32_t)(c.r * 255) << 16)
                 | ((uint32_t)(c.g * 255) << 8)
                 | ((uint32_t)(c.b * 255));
    size_t n = (size_t)s->w * s->h;
    uint32_t *p = (uint32_t *)s->buf;
    for (size_t i = 0; i < n; i++) p[i] = col;
}
static vt_texture_t *_sw_texture_create(vt_renderer_t *r, uint32_t w, uint32_t h,
                                          vt_pixel_format_t fmt) {
    vt_texture_t *t = vt_malloc0(sizeof(*t));
    t->w = w; t->h = h; t->fmt = fmt;
    t->pixels = vt_malloc0((size_t)w * h * 4);
    t->owns_pixels = true;
    (void)r;
    return t;
}
static void _sw_texture_free(vt_renderer_t *r, vt_texture_t *t) {
    if (!t) return;
    if (t->owns_pixels) vt_free(t->pixels);
    vt_free(t);
    (void)r;
}
static void _sw_texture_upload(vt_renderer_t *r, vt_texture_t *t,
                                 const void *pixels) {
    if (!t || !pixels || !t->pixels) return;
    size_t n = (size_t)t->w * t->h * 4;
    memcpy(t->pixels, pixels, n);
    (void)r;
}
static void _sw_texture_draw(vt_renderer_t *r, vt_texture_t *t, vt_rect_t dst,
                              vt_rect_t *src, vt_color_t *tint) {
    _sw_state_t *s = r->priv;
    if (!s->buf || !t || !t->pixels) return;
    vt_rect_t sr = src ? *src : (vt_rect_t){0, 0, (int)t->w, (int)t->h};
    /* nearest-neighbor blit */
    int dx = dst.x, dy = dst.y, dw = dst.w, dh = dst.h;
    if (dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = (sr.h > 0) ? sr.y + (y * sr.h) / dh : 0;
        if (sy < 0 || sy >= (int)t->h) continue;
        for (int x = 0; x < dw; x++) {
            int sx = (sr.w > 0) ? sr.x + (x * sr.w) / dw : 0;
            if (sx < 0 || sx >= (int)t->w) continue;
            int tx = dx + x, ty = dy + y;
            if (tx < 0 || tx >= s->w || ty < 0 || ty >= s->h) continue;
            uint8_t *src_px = (uint8_t *)t->pixels + (sy * t->w + sx) * 4;
            uint8_t a = src_px[3];
            if (tint && tint->a < 1.0f) {
                a = (uint8_t)(a * tint->a);
            }
            uint8_t *dp = s->buf + (ty * s->w + tx) * 4;
            if (a == 255) {
                dp[0] = src_px[0]; dp[1] = src_px[1];
                dp[2] = src_px[2]; dp[3] = 255;
            } else if (a > 0) {
                int inv = 255 - a;
                dp[0] = (uint8_t)((src_px[0] * a + dp[0] * inv) / 255);
                dp[1] = (uint8_t)((src_px[1] * a + dp[1] * inv) / 255);
                dp[2] = (uint8_t)((src_px[2] * a + dp[2] * inv) / 255);
                dp[3] = 255;
            }
        }
    }
}
static void _sw_fill_rect(vt_renderer_t *r, vt_rect_t rc, vt_color_t c) {
    _sw_state_t *s = r->priv;
    if (!s->buf) return;
    int x0 = rc.x < 0 ? 0 : rc.x;
    int y0 = rc.y < 0 ? 0 : rc.y;
    int x1 = rc.x + rc.w; if (x1 > s->w) x1 = s->w;
    int y1 = rc.y + rc.h; if (y1 > s->h) y1 = s->h;
    uint8_t cr = (uint8_t)(c.r * 255), cg = (uint8_t)(c.g * 255),
            cb = (uint8_t)(c.b * 255), ca = (uint8_t)(c.a * 255);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint8_t *p = s->buf + (y * s->w + x) * 4;
            if (ca == 255) {
                p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255;
            } else if (ca > 0) {
                int inv = 255 - ca;
                p[0] = (uint8_t)((cr * ca + p[0] * inv) / 255);
                p[1] = (uint8_t)((cg * ca + p[1] * inv) / 255);
                p[2] = (uint8_t)((cb * ca + p[2] * inv) / 255);
                p[3] = 255;
            }
        }
    }
}
static void _sw_present(vt_renderer_t *r) {
    /* nothing — caller reads s->buf */
    (void)r;
}
static const char *_sw_name(void) { return "software"; }

const vt_renderer_ops_t vt_renderer_sw_ops = {
    .init            = _sw_init,
    .fini            = _sw_fini,
    .begin           = _sw_begin,
    .end             = _sw_end,
    .clear           = _sw_clear,
    .texture_create  = _sw_texture_create,
    .texture_upload  = _sw_texture_upload,
    .texture_free    = _sw_texture_free,
    .texture_draw    = _sw_texture_draw,
    .fill_rect       = _sw_fill_rect,
    .blit            = NULL,
    .present         = _sw_present,
    .probe           = _sw_probe,
    .name            = _sw_name,
};
