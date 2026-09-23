/*
 * vt-wm.c — Vantage window manager (backend-agnostic core)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Window placement, focus, stacking, workspaces, keyboard shortcuts.
 * The public model operations are routed to the active backend engine
 * (X11: vt-wm-x11.c). On Wayland the equivalent lives in the Wayland
 * backend's xdg-shell implementation; on headless the model ops act on
 * the in-memory model only (useful for tests).
 */

#define VT_LOG_DOMAIN "wm"
#include <vantage/vt-wm.h>
#include <vantage/vt-backend.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

vt_wm_t *vt_wm_new(vt_backend_t *backend) {
    vt_wm_t *wm = vt_malloc0(sizeof(*wm));
    wm->backend = backend;
    vt_vec_init(&wm->windows, sizeof(vt_window_t *), 16);
    vt_vec_init(&wm->workspaces, sizeof(char *), 4);
    vt_vec_init(&wm->shortcuts, sizeof(vt_wm_shortcut_t), 8);
    for (int i = 0; i < 4; i++) {
        char *name = vt_strprintf("Workspace %d", i + 1);
        vt_vec_push(&wm->workspaces, &name);
    }
    wm->cur_ws = 0;
    return wm;
}

void vt_wm_free(vt_wm_t *wm) {
    if (!wm) return;
    if (wm->engine) vt_wm_x11_free((struct vt_wm_x11 *)wm->engine);
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (w && !w->backend_priv) {
            vt_free(w->title); vt_free(w->app_id);
            vt_free(w->class_str); vt_free(w);
        }
    }
    vt_vec_fini(&wm->windows);
    for (size_t i = 0; i < wm->workspaces.size; i++) {
        char **p = vt_vec_at(&wm->workspaces, i);
        vt_free(*p);
    }
    vt_vec_fini(&wm->workspaces);
    for (size_t i = 0; i < wm->shortcuts.size; i++) {
        vt_wm_shortcut_t *sc = vt_vec_at(&wm->shortcuts, i);
        vt_free(sc->combo);
    }
    vt_vec_fini(&wm->shortcuts);
    vt_free(wm);
}

int vt_wm_start(vt_wm_t *wm) {
    if (!wm) return VT_ERR_INVAL;
    if (wm->backend &&
        (wm->backend->kind == VT_BACKEND_XORG ||
         wm->backend->kind == VT_BACKEND_XLIBRE)) {
        wm->engine = vt_wm_x11_new_impl(wm);
        int rc = vt_wm_x11_start((struct vt_wm_x11 *)wm->engine);
        if (rc != VT_OK) {
            vt_wm_x11_free((struct vt_wm_x11 *)wm->engine);
            wm->engine = NULL;
            return rc;
        }
    }
    vt_logi("wm: started (%zu workspaces)", wm->workspaces.size);
    return VT_OK;
}

void vt_wm_stop(vt_wm_t *wm) {
    if (!wm) return;
    if (wm->engine) vt_wm_x11_stop((struct vt_wm_x11 *)wm->engine);
}

int vt_wm_step(vt_wm_t *wm, int timeout_ms) {
    if (!wm || !wm->backend) return VT_ERR_INVAL;
    return vt_backend_dispatch(wm->backend, timeout_ms);
}

vt_window_t *vt_wm_lookup(vt_wm_t *wm, uint32_t id) {
    if (!wm) return NULL;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (w && w->id == id) return w;
    }
    return NULL;
}

vt_window_t *vt_wm_focused(vt_wm_t *wm) {
    if (!wm) return NULL;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (w && w->focused) return w;
    }
    return NULL;
}

void vt_wm_focus(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_focus_id((struct vt_wm_x11 *)wm->engine, w->id);
        return;
    }
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *p = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (p) p->focused = (p == w);
    }
}

void vt_wm_close(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_close_id((struct vt_wm_x11 *)wm->engine, w->id);
        return;
    }
    vt_wm_remove_window(wm, w);
}

void vt_wm_move(vt_wm_t *wm, vt_window_t *w, int x, int y) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_move_id((struct vt_wm_x11 *)wm->engine, w->id, x, y);
        return;
    }
    w->x = x; w->y = y;
}

void vt_wm_resize(vt_wm_t *wm, vt_window_t *w, int w_, int h_) {
    if (!wm || !w || w_ <= 0 || h_ <= 0) return;
    if (wm->engine) {
        vt_wm_x11_resize_id((struct vt_wm_x11 *)wm->engine, w->id, w_, h_);
        return;
    }
    w->w = w_; w->h = h_;
}

void vt_wm_maximize(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_maximize_id((struct vt_wm_x11 *)wm->engine, w->id, on);
        return;
    }
    if (on && !w->maximized) {
        w->prev_x = w->x; w->prev_y = w->y;
        w->prev_w = w->w; w->prev_h = w->h;
        w->maximized = true;
    } else if (!on && w->maximized) {
        w->x = w->prev_x; w->y = w->prev_y;
        w->w = w->prev_w; w->h = w->prev_h;
        w->maximized = false;
    }
}

void vt_wm_minimize(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_minimize_id((struct vt_wm_x11 *)wm->engine, w->id, on);
        return;
    }
    w->minimized = on;
}

void vt_wm_fullscreen(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_fullscreen_id((struct vt_wm_x11 *)wm->engine, w->id, on);
        return;
    }
    if (on && !w->fullscreen) {
        w->prev_x = w->x; w->prev_y = w->y;
        w->prev_w = w->w; w->prev_h = w->h;
        w->fullscreen = true;
    } else if (!on && w->fullscreen) {
        w->x = w->prev_x; w->y = w->prev_y;
        w->w = w->prev_w; w->h = w->prev_h;
        w->fullscreen = false;
    }
}

void vt_wm_tile(vt_wm_t *wm, vt_window_t *w, vt_wm_tile_t t) {
    if (!wm || !w) return;
    if (wm->engine) {
        vt_wm_x11_tile_id((struct vt_wm_x11 *)wm->engine, w->id, t);
        return;
    }
    vt_wm_maximize(wm, w, t == VT_WM_TILE_MAX);
    vt_wm_fullscreen(wm, w, t == VT_WM_TILE_FULLSCREEN);
    w->tile = t;
}

void vt_wm_send_below(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    w->layer = VT_WM_LAYER_BELOW;
}
void vt_wm_send_above(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    w->layer = VT_WM_LAYER_ABOVE;
}

void vt_wm_remove_window(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t **pp = vt_vec_at(&wm->windows, i);
        if (*pp == w) { vt_vec_remove(&wm->windows, i); return; }
    }
}

int vt_wm_workspace_count(vt_wm_t *wm) {
    return (int)(wm ? wm->workspaces.size : 0);
}
int vt_wm_workspace_current(vt_wm_t *wm) {
    return wm ? wm->cur_ws : 0;
}
void vt_wm_workspace_switch(vt_wm_t *wm, int ws) {
    if (!wm || ws < 0 || ws >= (int)wm->workspaces.size) return;
    if (wm->engine) {
        vt_wm_x11_desktop((struct vt_wm_x11 *)wm->engine, ws);
        return;
    }
    wm->cur_ws = ws;
    if (wm->on_desktop_changed) wm->on_desktop_changed(wm, ws);
    vt_logi("wm: switched to workspace %d", ws + 1);
}
void vt_wm_workspace_move(vt_wm_t *wm, vt_window_t *w, int ws) {
    if (!wm || !w || ws < 0) return;
    if (wm->engine) {
        vt_wm_x11_move_to_desktop_id((struct vt_wm_x11 *)wm->engine, w->id, ws);
        return;
    }
    w->workspace = ws;
}

/* placement: center on the primary output (model-level helper) */
void vt_wm_place(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    int sw = 1024, sh = 768;
    if (wm->backend && vt_backend_output_count(wm->backend) > 0) {
        const vt_output_t *o = vt_backend_output_at(wm->backend, 0);
        if (o) { sw = o->w; sh = o->h; }
    }
    w->x = (sw - w->w) / 2;
    w->y = (sh - w->h) / 2;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
}

/* ------------------------------------------------------------- shortcuts */
int vt_wm_shortcut_register(vt_wm_t *wm, const char *key_combo,
                            vt_wm_shortcut_cb_t cb, void *ud) {
    if (!wm || !key_combo || !cb) return -1;
    vt_wm_shortcut_t sc = { .combo = vt_strdup(key_combo), .cb = cb, .ud = ud };
    int id = (int)wm->shortcuts.size + 1;
    vt_vec_push(&wm->shortcuts, &sc);
    return id;
}

void vt_wm_shortcut_unregister(vt_wm_t *wm, int id) {
    if (!wm || id <= 0) return;
    if ((size_t)id > wm->shortcuts.size) return;
    vt_wm_shortcut_t *sc = vt_vec_at(&wm->shortcuts, (size_t)id - 1);
    vt_free(sc->combo);
    vt_vec_remove(&wm->shortcuts, (size_t)id - 1);
}

bool vt_wm_shortcut_handle(vt_wm_t *wm, const char *key_combo) {
    if (!wm || !key_combo) return false;
    for (size_t i = 0; i < wm->shortcuts.size; i++) {
        vt_wm_shortcut_t *sc = vt_vec_at(&wm->shortcuts, i);
        if (vt_strcaseeq(sc->combo, key_combo)) {
            sc->cb(wm, sc->ud);
            return true;
        }
    }
    return false;
}
