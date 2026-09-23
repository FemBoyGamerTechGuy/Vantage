/*
 * vt-wm.c — Vantage window manager
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Window placement, focus, stacking, workspaces, keyboard shortcuts.
 * Works against the chosen display backend (X11 or Wayland).
 *
 * For X11, the actual EWMH/ICCCM wire-protocol interactions happen in
 * vt-wm-x11.c. For Wayland (xdg-shell), the equivalent lives in
 * vt-wm-wayland.c. This file contains the backend-agnostic logic.
 */

#define VT_LOG_DOMAIN "wm"
#include <vantage/vt-wm.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static uint32_t _next_win_id_unused = 1;

vt_wm_t *vt_wm_new(vt_backend_t *backend) {
    vt_wm_t *wm = vt_malloc0(sizeof(*wm));
    wm->backend = backend;
    vt_vec_init(&wm->windows, sizeof(vt_window_t *), 16);
    vt_vec_init(&wm->workspaces, sizeof(char *), 4);
    /* Default workspaces */
    for (int i = 0; i < 4; i++) {
        char *name = vt_strprintf("Workspace %d", i + 1);
        char *p = name;
        vt_vec_push(&wm->workspaces, &p);
    }
    wm->cur_ws = 0;
    return wm;
}

void vt_wm_free(vt_wm_t *wm) {
    if (!wm) return;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *w = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (w) { vt_free(w->title); vt_free(w->app_id); vt_free(w->class_str); vt_free(w); }
    }
    vt_vec_fini(&wm->windows);
    for (size_t i = 0; i < wm->workspaces.size; i++) {
        char **p = vt_vec_at(&wm->workspaces, i);
        vt_free(*p);
    }
    vt_vec_fini(&wm->workspaces);
    vt_free(wm);
}

int vt_wm_start(vt_wm_t *wm) {
    if (!wm) return VT_ERR_INVAL;
    vt_logi("wm: started (%zu workspaces)", wm->workspaces.size);
    return VT_OK;
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
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t *p = *(vt_window_t **)vt_vec_at(&wm->windows, i);
        if (p) p->focused = (p == w);
    }
}
void vt_wm_close(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    for (size_t i = 0; i < wm->windows.size; i++) {
        vt_window_t **pp = vt_vec_at(&wm->windows, i);
        if (*pp == w) {
            vt_free(w->title); vt_free(w->app_id);
            vt_free(w->class_str); vt_free(w);
            vt_vec_remove(&wm->windows, i);
            return;
        }
    }
}
void vt_wm_move(vt_wm_t *wm, vt_window_t *w, int x, int y) {
    if (!wm || !w) return;
    w->x = x; w->y = y;
    (void)wm;
}
void vt_wm_resize(vt_wm_t *wm, vt_window_t *w, int w_, int h_) {
    if (!wm || !w || w_ <= 0 || h_ <= 0) return;
    w->w = w_; w->h = h_;
    (void)wm;
}
void vt_wm_maximize(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    if (on && !w->maximized) {
        w->prev_x = w->x; w->prev_y = w->y;
        w->prev_w = w->w; w->prev_h = w->h;
        /* TODO: get screen size from backend */
        w->x = 0; w->y = 0; w->w = 1920; w->h = 1080;
        w->maximized = true;
    } else if (!on && w->maximized) {
        w->x = w->prev_x; w->y = w->prev_y;
        w->w = w->prev_w; w->h = w->prev_h;
        w->maximized = false;
    }
}
void vt_wm_minimize(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    w->minimized = on;
}
void vt_wm_fullscreen(vt_wm_t *wm, vt_window_t *w, bool on) {
    if (!wm || !w) return;
    if (on && !w->fullscreen) {
        w->prev_x = w->x; w->prev_y = w->y;
        w->prev_w = w->w; w->prev_h = w->h;
        w->x = 0; w->y = 0; w->w = 1920; w->h = 1080;
        w->fullscreen = true;
    } else if (!on && w->fullscreen) {
        w->x = w->prev_x; w->y = w->prev_y;
        w->w = w->prev_w; w->h = w->prev_h;
        w->fullscreen = false;
    }
}
void vt_wm_tile(vt_wm_t *wm, vt_window_t *w, vt_wm_tile_t t) {
    if (!wm || !w) return;
    int sw = 1920, sh = 1080;
    switch (t) {
    case VT_WM_TILE_LEFT:    w->x = 0;     w->y = 0;     w->w = sw/2; w->h = sh; break;
    case VT_WM_TILE_RIGHT:   w->x = sw/2;  w->y = 0;     w->w = sw/2; w->h = sh; break;
    case VT_WM_TILE_TOP:     w->x = 0;     w->y = 0;     w->w = sw;   w->h = sh/2; break;
    case VT_WM_TILE_BOTTOM:  w->x = 0;     w->y = sh/2;  w->w = sw;   w->h = sh/2; break;
    case VT_WM_TILE_MAX:     vt_wm_maximize(wm, w, true); break;
    case VT_WM_TILE_FULLSCREEN: vt_wm_fullscreen(wm, w, true); break;
    default: break;
    }
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
int vt_wm_workspace_count(vt_wm_t *wm) { return (int)(wm ? wm->workspaces.size : 0); }
int vt_wm_workspace_current(vt_wm_t *wm) { return wm ? wm->cur_ws : 0; }
void vt_wm_workspace_switch(vt_wm_t *wm, int ws) {
    if (!wm || ws < 0 || ws >= (int)wm->workspaces.size) return;
    wm->cur_ws = ws;
    vt_logi("wm: switched to workspace %d", ws + 1);
}
void vt_wm_workspace_move(vt_wm_t *wm, vt_window_t *w, int ws) {
    if (!wm || !w || ws < 0) return;
    w->workspace = ws;
}

/* placement: smart placement near cursor/last-focus */
void vt_wm_place(vt_wm_t *wm, vt_window_t *w) {
    if (!wm || !w) return;
    /* center on screen for now */
    int sw = 1920, sh = 1080;
    w->x = (sw - w->w) / 2;
    w->y = (sh - w->h) / 2;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
}

/* shortcuts */
typedef struct {
    char *combo;
    vt_wm_shortcut_cb_t cb;
    void *ud;
} _sc_t;

int vt_wm_shortcut_register(vt_wm_t *wm, const char *key_combo,
                              vt_wm_shortcut_cb_t cb, void *ud) {
    if (!wm || !key_combo || !cb) return -1;
    if (!wm->priv) {
        wm->priv = vt_malloc0(sizeof(vt_vec_t));
        vt_vec_init((vt_vec_t *)wm->priv, sizeof(_sc_t), 8);
    }
    vt_vec_t *arr = (vt_vec_t *)wm->priv;
    _sc_t sc = { .combo = vt_strdup(key_combo), .cb = cb, .ud = ud };
    int id = (int)arr->size + 1;
    vt_vec_push(arr, &sc);
    return id;
}
void vt_wm_shortcut_unregister(vt_wm_t *wm, int id) {
    if (!wm || !wm->priv || id <= 0) return;
    vt_vec_t *v = wm->priv;
    if ((size_t)id > v->size) return;
    _sc_t *sc = vt_vec_at(v, (size_t)id - 1);
    vt_free(sc->combo);
    vt_vec_remove(v, (size_t)id - 1);
}
bool vt_wm_shortcut_handle(vt_wm_t *wm, const char *key_combo) {
    if (!wm || !wm->priv || !key_combo) return false;
    vt_vec_t *v = wm->priv;
    for (size_t i = 0; i < v->size; i++) {
        _sc_t *sc = vt_vec_at(v, i);
        if (vt_strcaseeq(sc->combo, key_combo)) {
            return sc->cb(wm, sc->ud);
        }
    }
    return false;
}
