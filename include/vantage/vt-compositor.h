/*
 * vt-compositor.h — Vantage compositor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owns frame scheduling, damage tracking, and surface compositing.
 * Designed for performance: no work when idle, damage-tracked repaints,
 * optional effects (shadows, blur, animations) that all have runtime
 * kill switches.
 */
#ifndef VANTAGE_COMPOSITOR_H
#define VANTAGE_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-backend.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vt_wm;
struct vt_compositor_x11;

typedef struct vt_compositor   vt_compositor_t;
typedef struct vt_surface      vt_surface_t;
typedef struct vt_compositor_config {
    bool  enable_shadows;
    bool  enable_blur;
    bool  enable_animations;
    bool  enable_vsync;
    bool  enable_transparency;
    int   shadow_radius;
    float blur_strength;
    float anim_duration_ms;
} vt_compositor_config_t;

typedef enum {
    VT_SURFACE_TYPE_TOPLEVEL = 0,
    VT_SURFACE_TYPE_POPUP,
    VT_SURFACE_TYPE_OVERRIDE,
    VT_SURFACE_TYPE_DESKTOP,
    VT_SURFACE_TYPE_PANEL,
    VT_SURFACE_TYPE_WALLPAPER,
} vt_surface_type_t;

typedef struct vt_surface_state {
    int   x, y, w, h;
    int   opacity;        /* 0..255 */
    bool  visible;
    bool  has_alpha;
    vt_rect_t damage;
    bool  damaged;
} vt_surface_state_t;

struct vt_surface {
    uint32_t            id;
    vt_surface_type_t   type;
    vt_surface_state_t  state;
    vt_texture_t       *texture;
    void               *backend_priv;   /* wayland/x11 surface handle */
    void               *user_data;
};

struct vt_compositor {
    vt_renderer_t         *renderer;
    vt_backend_t          *backend;
    vt_compositor_config_t cfg;
    vt_vec_t               surfaces;       /* vt_surface_t * */
    vt_vec_t               damage_history; /* vt_rect_t */
    uint64_t               frame_us;
    uint64_t               last_frame_us;
    uint32_t               frames_drawn;
    bool                   running;
    void                  *priv;
};

vt_compositor_t *vt_compositor_new(vt_renderer_t *r, vt_backend_t *b);
void             vt_compositor_free(vt_compositor_t *c);
int              vt_compositor_start(vt_compositor_t *c);
void             vt_compositor_stop(vt_compositor_t *c);
int              vt_compositor_step(vt_compositor_t *c, int timeout_ms);
vt_surface_t    *vt_compositor_create_surface(vt_compositor_t *c,
                                                vt_surface_type_t t);
void             vt_compositor_destroy_surface(vt_compositor_t *c,
                                                vt_surface_t *s);
void             vt_compositor_damage(vt_compositor_t *c, vt_rect_t r);
void             vt_compositor_damage_all(vt_compositor_t *c);
void             vt_compositor_set_config(vt_compositor_t *c,
                                            const vt_compositor_config_t *cfg);
void             vt_compositor_get_config(vt_compositor_t *c,
                                            vt_compositor_config_t *out);
uint32_t         vt_compositor_fps(const vt_compositor_t *c);

/* X11 composite engine (XComposite + XDamage + XRender). Redirects the
 * root's subwindows, composites them damage-driven into a back buffer,
 * honors _NET_WM_WINDOW_OPACITY, and paints optional soft shadows.
 * Returns VT_OK or a negative error (VT_ERR_NOTSUPP when extensions are
 * missing). The engine runs inside vt_compositor_step(). */
int  vt_compositor_attach_x11(vt_compositor_t *c, struct vt_wm *wm);
bool vt_compositor_x11_active(const vt_compositor_t *c);
/* Query whether the running X server provides the needed extensions. */
bool vt_compositor_x11_available(void);
/* Engine integration (called from the compositor core) */
void vt_compositor_x11_step(void);
void vt_compositor_x11_stop(void);
void vt_compositor_x11_screen_resized(void);

#ifdef __cplusplus
}
#endif
#endif
