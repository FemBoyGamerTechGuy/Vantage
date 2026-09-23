/*
 * vt-wm.h — Vantage window manager
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Window placement, stacking, focus, workspaces, shortcuts. Has both an
 * X11 EWMH/ICCCM backend and a Wayland xdg-shell backend.
 */
#ifndef VANTAGE_WM_H
#define VANTAGE_WM_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-backend.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_WM_LAYER_DESKTOP   = 0,
    VT_WM_LAYER_BELOW     = 1,
    VT_WM_LAYER_NORMAL    = 2,
    VT_WM_LAYER_ABOVE     = 3,
    VT_WM_LAYER_PANEL     = 4,
    VT_WM_LAYER_OVERLAY   = 5,
} vt_wm_layer_t;

typedef enum {
    VT_WM_TILE_NONE = 0,
    VT_WM_TILE_LEFT,
    VT_WM_TILE_RIGHT,
    VT_WM_TILE_TOP,
    VT_WM_TILE_BOTTOM,
    VT_WM_TILE_MAX,
    VT_WM_TILE_FULLSCREEN,
} vt_wm_tile_t;

typedef struct vt_window {
    uint32_t        id;
    char           *title;
    char           *app_id;
    char           *class_str;
    int             x, y, w, h;
    int             prev_x, prev_y, prev_w, prev_h;   /* for un-maximize */
    vt_wm_layer_t   layer;
    vt_wm_tile_t    tile;
    bool            focused;
    bool            minimized;
    bool            maximized;
    bool            fullscreen;
    bool            sticky;
    int             workspace;
    void           *backend_priv;
    void           *user_data;
} vt_window_t;

typedef struct vt_wm {
    vt_backend_t   *backend;
    vt_vec_t        windows;
    vt_vec_t        workspaces;        /* char* */
    int             cur_ws;
    void           *priv;
} vt_wm_t;

vt_wm_t  *vt_wm_new(vt_backend_t *backend);
void      vt_wm_free(vt_wm_t *wm);
int       vt_wm_start(vt_wm_t *wm);
int       vt_wm_step(vt_wm_t *wm, int timeout_ms);

vt_window_t *vt_wm_lookup(vt_wm_t *wm, uint32_t id);
vt_window_t *vt_wm_focused(vt_wm_t *wm);
void      vt_wm_focus(vt_wm_t *wm, vt_window_t *w);
void      vt_wm_close(vt_wm_t *wm, vt_window_t *w);
void      vt_wm_move(vt_wm_t *wm, vt_window_t *w, int x, int y);
void      vt_wm_resize(vt_wm_t *wm, vt_window_t *w, int w_, int h_);
void      vt_wm_maximize(vt_wm_t *wm, vt_window_t *w, bool on);
void      vt_wm_minimize(vt_wm_t *wm, vt_window_t *w, bool on);
void      vt_wm_fullscreen(vt_wm_t *wm, vt_window_t *w, bool on);
void      vt_wm_tile(vt_wm_t *wm, vt_window_t *w, vt_wm_tile_t t);
void      vt_wm_send_below(vt_wm_t *wm, vt_window_t *w);
void      vt_wm_send_above(vt_wm_t *wm, vt_window_t *w);

int       vt_wm_workspace_count(vt_wm_t *wm);
int       vt_wm_workspace_current(vt_wm_t *wm);
void      vt_wm_workspace_switch(vt_wm_t *wm, int ws);
void      vt_wm_workspace_move(vt_wm_t *wm, vt_window_t *w, int ws);

/* Placement helper (smart placement near cursor or last-focus point) */
void      vt_wm_place(vt_wm_t *wm, vt_window_t *w);

/* Keyboard shortcut dispatcher */
typedef bool (*vt_wm_shortcut_cb_t)(vt_wm_t *wm, void *ud);
int  vt_wm_shortcut_register(vt_wm_t *wm, const char *key_combo,
                              vt_wm_shortcut_cb_t cb, void *ud);
void vt_wm_shortcut_unregister(vt_wm_t *wm, int id);
bool vt_wm_shortcut_handle(vt_wm_t *wm, const char *key_combo);

#ifdef __cplusplus
}
#endif
#endif
