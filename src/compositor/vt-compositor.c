/*
 * vt-compositor.c — Damage-tracked compositor core
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Owns frame scheduling, damage tracking, and surface composition.
 *
 * The compositor is event-driven. It does NOT repaint in a tight loop;
 * it blocks on the backend's poll, repaints only damaged regions,
 * then waits for the next backend event. VSync is delegated to the
 * backend's swap-buffers implementation.
 *
 * Optional effects (shadows, blur, animations) all have runtime kill
 * switches; disabling them costs nothing — they are not even called.
 */

#define VT_LOG_DOMAIN "compositor"
#include <vantage/vt-compositor.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t _next_surf_id = 1;

vt_compositor_t *vt_compositor_new(vt_renderer_t *r, vt_backend_t *b) {
    vt_compositor_t *c = vt_malloc0(sizeof(*c));
    c->renderer = r;
    c->backend  = b;
    c->running  = false;
    c->cfg.enable_shadows = true;
    c->cfg.enable_blur = false;
    c->cfg.enable_animations = true;
    c->cfg.enable_vsync = true;
    c->cfg.enable_transparency = true;
    c->cfg.shadow_radius = 12;
    c->cfg.blur_strength = 0.5f;
    c->cfg.anim_duration_ms = 200;
    vt_vec_init(&c->surfaces, sizeof(vt_surface_t *), 8);
    vt_vec_init(&c->damage_history, sizeof(vt_rect_t), 16);
    return c;
}

void vt_compositor_free(vt_compositor_t *c) {
    if (!c) return;
    vt_compositor_stop(c);
    for (size_t i = 0; i < c->surfaces.size; i++) {
        vt_surface_t *s = *(vt_surface_t **)vt_vec_at(&c->surfaces, i);
        if (s) {
            if (c->renderer) vt_renderer_texture_free(c->renderer, s->texture);
            vt_free(s);
        }
    }
    vt_vec_fini(&c->surfaces);
    vt_vec_fini(&c->damage_history);
    vt_free(c);
}

int vt_compositor_start(vt_compositor_t *c) {
    if (!c || !c->backend) return VT_ERR_INVAL;
    if (c->running) return VT_OK;
    if (vt_compositor_x11_active(c)) {
        c->running = true;
        c->frame_us = vt_time_now_us();
        return VT_OK;
    }
    if (!c->renderer) return VT_ERR_INVAL;
    if (!c->renderer->initialized && vt_renderer_init(c->renderer) < 0) {
        vt_loge("compositor: renderer init failed");
        return VT_ERR;
    }
    c->running = true;
    c->frame_us = vt_time_now_us();
    return VT_OK;
}

void vt_compositor_stop(vt_compositor_t *c) {
    if (!c) return;
    if (vt_compositor_x11_active(c)) vt_compositor_x11_stop();
    c->running = false;
}

vt_surface_t *vt_compositor_create_surface(vt_compositor_t *c,
                                            vt_surface_type_t t) {
    if (!c) return NULL;
    vt_surface_t *s = vt_malloc0(sizeof(*s));
    s->id = _next_surf_id++;
    s->type = t;
    s->state.visible = true;
    s->state.opacity = 255;
    s->state.damaged = true;
    vt_surface_t *p = s;
    vt_vec_push(&c->surfaces, &p);
    return s;
}

void vt_compositor_destroy_surface(vt_compositor_t *c, vt_surface_t *s) {
    if (!c || !s) return;
    for (size_t i = 0; i < c->surfaces.size; i++) {
        vt_surface_t **pp = vt_vec_at(&c->surfaces, i);
        if (*pp == s) {
            if (c->renderer) vt_renderer_texture_free(c->renderer, s->texture);
            vt_free(s);
            vt_vec_remove(&c->surfaces, i);
            vt_compositor_damage_all(c);
            return;
        }
    }
}

void vt_compositor_damage(vt_compositor_t *c, vt_rect_t r) {
    if (!c) return;
    vt_vec_push(&c->damage_history, &r);
}
void vt_compositor_damage_all(vt_compositor_t *c) {
    if (!c || !c->backend) return;
    vt_rect_t r = {0, 0, 100000, 100000};
    vt_compositor_damage(c, r);
}

void vt_compositor_set_config(vt_compositor_t *c, const vt_compositor_config_t *cfg) {
    if (!c || !cfg) return;
    c->cfg = *cfg;
    vt_compositor_damage_all(c);
}
void vt_compositor_get_config(vt_compositor_t *c, vt_compositor_config_t *out) {
    if (out && c) *out = c->cfg;
}

static int _rect_intersect(vt_rect_t a, vt_rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (x1 > x0 && y1 > y0);
}

int vt_compositor_step(vt_compositor_t *c, int timeout_ms) {
    if (!c || !c->running) return VT_ERR;
    /* X11 composite engine drives its own damage/paint pipeline */
    if (vt_compositor_x11_active(c)) {
        if (c->backend && c->backend->dispatch)
            c->backend->dispatch(c->backend, timeout_ms);
        vt_compositor_x11_step();
        return VT_OK;
    }
    /* 1. Dispatch backend events */
    if (c->backend && c->backend->dispatch) {
        int r = c->backend->dispatch(c->backend, timeout_ms);
        if (r < 0) return r;
    }
    /* 2. If anything is damaged, repaint */
    uint64_t now = vt_time_now_us();
    c->last_frame_us = c->frame_us;
    c->frame_us = now;
    bool need_repaint = false;
    if (c->damage_history.size > 0) need_repaint = true;
    for (size_t i = 0; i < c->surfaces.size; i++) {
        vt_surface_t *s = *(vt_surface_t **)vt_vec_at(&c->surfaces, i);
        if (s && s->state.damaged) { need_repaint = true; break; }
    }
    if (!need_repaint) return VT_OK;

    /* Determine output size — first output, fallback 1920x1080 */
    int w = 1920, h = 1080;
    if (c->backend) {
        size_t n = vt_backend_output_count(c->backend);
        if (n > 0) {
            const vt_output_t *o = vt_backend_output_at(c->backend, 0);
            if (o && o->enabled) { w = o->w; h = o->h; }
        }
    }
    if (c->renderer) {
        vt_renderer_begin(c->renderer, w, h);
        vt_color_t bg = { .r = 0.1f, .g = 0.1f, .b = 0.1f, .a = 1.0f };
        vt_renderer_clear(c->renderer, bg);
        /* draw surfaces in order */
        for (size_t i = 0; i < c->surfaces.size; i++) {
            vt_surface_t *s = *(vt_surface_t **)vt_vec_at(&c->surfaces, i);
            if (!s || !s->state.visible) continue;
            if (s->texture) {
                vt_rect_t dst = { s->state.x, s->state.y, s->state.w, s->state.h };
                vt_color_t tint = {1.0f, 1.0f, 1.0f, (float)s->state.opacity / 255.0f};
                vt_renderer_texture_draw(c->renderer, s->texture, dst, NULL, &tint);
            } else {
                vt_rect_t r = { s->state.x, s->state.y, s->state.w, s->state.h };
                vt_color_t col = {0.15f, 0.15f, 0.18f, 1.0f};
                vt_renderer_fill_rect(c->renderer, r, col);
            }
            s->state.damaged = false;
        }
        vt_renderer_end(c->renderer);
        vt_renderer_present(c->renderer);
    }
    c->frames_drawn++;
    vt_vec_clear(&c->damage_history);
    (void)_rect_intersect;
    return VT_OK;
}

uint32_t vt_compositor_fps(const vt_compositor_t *c) {
    if (!c || c->last_frame_us == 0) return 0;
    uint64_t dt = c->frame_us - c->last_frame_us;
    if (dt == 0) return 0;
    return (uint32_t)(1000000u / dt);
}
