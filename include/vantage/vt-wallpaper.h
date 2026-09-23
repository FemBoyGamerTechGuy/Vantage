/*
 * vt-wallpaper.h — Vantage wallpaper engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Supports static images, gradients, programmatically-generated backgrounds,
 * and **live video wallpapers** using ffmpeg (libavcodec/libavformat).
 *
 * The wallpaper pipeline is part of the compositor: it does NOT continuously
 * repaint when nothing visible has changed. When the wallpaper is occluded
 * or the user disabled video wallpaper, the pipeline is paused.
 */
#ifndef VANTAGE_WALLPAPER_H
#define VANTAGE_WALLPAPER_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_WALLPAPER_NONE = 0,
    VT_WALLPAPER_COLOR,
    VT_WALLPAPER_GRADIENT,
    VT_WALLPAPER_IMAGE,
    VT_WALLPAPER_VIDEO,
    VT_WALLPAPER_SHADER,
    VT_WALLPAPER_GENERATED,
} vt_wallpaper_kind_t;

typedef struct vt_wallpaper {
    vt_wallpaper_kind_t kind;
    char     *path;          /* image/video file path */
    char     *shader;        /* shader source */
    vt_color_t color_a;
    vt_color_t color_b;
    int        gradient_dir; /* 0=H, 1=V, 2=D, 3=RD */
    float      scale;        /* 1=fit, 2=fill, 3=stretch */
    float      volume;       /* video wallpaper audio volume (0..1) */
    bool       mute;
    bool       loop;
    bool       running;
    int        per_output;   /* index, or -1 for all */
    vt_texture_t *texture;
    void      *decoder;      /* avcodec context */
    void      *priv;
} vt_wallpaper_t;

vt_wallpaper_t *vt_wallpaper_new(void);
void            vt_wallpaper_free(vt_wallpaper_t *w);

int  vt_wallpaper_load(vt_wallpaper_t *w, const char *spec);
int  vt_wallpaper_load_from_config(vt_wallpaper_t *w);

void vt_wallpaper_set_kind(vt_wallpaper_t *w, vt_wallpaper_kind_t k);
void vt_wallpaper_set_color(vt_wallpaper_t *w, vt_color_t a, vt_color_t b, int dir);
void vt_wallpaper_set_path(vt_wallpaper_t *w, const char *p);
void vt_wallpaper_set_volume(vt_wallpaper_t *w, float v);
void vt_wallpaper_set_loop(vt_wallpaper_t *w, bool on);
void vt_wallpaper_pause(vt_wallpaper_t *w);
void vt_wallpaper_resume(vt_wallpaper_t *w);

/* Called by compositor each frame — updates texture if needed (e.g. video). */
int  vt_wallpaper_step(vt_wallpaper_t *w, vt_renderer_t *r, uint32_t output_w,
                       uint32_t output_h);
void vt_wallpaper_render(vt_wallpaper_t *w, vt_renderer_t *r,
                          vt_rect_t area);

#ifdef __cplusplus
}
#endif
#endif
