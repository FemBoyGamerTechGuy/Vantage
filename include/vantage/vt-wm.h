/*
 * vt-wm.h — Vantage window manager
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Window placement, focus, stacking, workspaces, shortcuts. The public
 * model (vt_window_t / vt_wm_t) is backend-agnostic; the concrete engines
 * (vt-wm-x11.c for Xorg/XLibre, the Wayland backend's xdg-shell logic)
 * keep the model synchronized and implement the actual wire protocol.
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

typedef enum {
    VT_WM_EVENT_OPEN = 0,      /* a window was managed */
    VT_WM_EVENT_CLOSE,         /* a window was unmanaged */
    VT_WM_EVENT_CLOSE_REQUEST, /* close was sent to a client */
    VT_WM_EVENT_FOCUS,         /* focus changed */
    VT_WM_EVENT_TITLE,         /* title changed */
    VT_WM_EVENT_STATE,         /* state flags changed */
    VT_WM_EVENT_GEOMETRY,      /* position/size changed */
} vt_wm_event_t;

typedef struct vt_window {
    uint32_t        id;          /* X11: the X Window id */
    char           *title;
    char           *app_id;      /* X11: WM_CLASS res_name */
    char           *class_str;   /* X11: WM_CLASS res_class */
    int             x, y, w, h;
    int             prev_x, prev_y, prev_w, prev_h;   /* pre-maximize geometry */
    vt_wm_layer_t   layer;
    vt_wm_tile_t    tile;
    bool            focused;
    bool            mapped;
    bool            urgent;
    bool            minimized;
    bool            maximized;
    bool            fullscreen;
    bool            sticky;
    int             workspace;
    void           *backend_priv;
    void           *user_data;
} vt_window_t;

/* Registered shortcut (shared between the core dispatcher and the
 * X11 engine, which converts `combo` into XGrabKey requests). */
struct vt_wm;
typedef struct {
    char *combo;
    void (*cb)(struct vt_wm *wm, void *ud);
    void *ud;
} vt_wm_shortcut_t;

struct vt_wm;

typedef void (*vt_wm_window_event_fn)(struct vt_wm *wm, vt_window_t *w,
                                      vt_wm_event_t ev);
typedef void (*vt_wm_desktop_event_fn)(struct vt_wm *wm, int desktop);

typedef struct vt_wm {
    vt_backend_t   *backend;
    vt_vec_t        windows;         /* vt_window_t* (focus order) */
    vt_vec_t        workspaces;      /* char* */
    int             cur_ws;
    vt_vec_t        shortcuts;       /* vt_wm_shortcut_t */
    void           *engine;          /* backend-specific engine (vt_wm_x11_t*) */
    vt_wm_window_event_fn   on_window_event;
    vt_wm_desktop_event_fn  on_desktop_changed;
    void           *hooks_ud;
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
/* Remove a window from the model (used when a client withdraws). */
void      vt_wm_remove_window(vt_wm_t *wm, vt_window_t *w);
void      vt_wm_stop(vt_wm_t *wm);

/* Keyboard shortcut dispatcher */
typedef void (*vt_wm_shortcut_cb_t)(vt_wm_t *wm, void *ud);
int  vt_wm_shortcut_register(vt_wm_t *wm, const char *key_combo,
                              vt_wm_shortcut_cb_t cb, void *ud);
void vt_wm_shortcut_unregister(vt_wm_t *wm, int id);
bool vt_wm_shortcut_handle(vt_wm_t *wm, const char *key_combo);
/* X11 engine (defined in vt-wm-x11.c; NULL on non-X11 backends) */
struct vt_wm_x11;
struct vt_wm_x11 *vt_wm_x11_from(vt_wm_t *wm);
/* Engine operations (only valid when vt_wm_x11_from(wm) != NULL).
 * IDs are the vt_window_t::id values (X11 Window ids). */
void vt_wm_x11_focus_id(struct vt_wm_x11 *eng, uint32_t id);
void vt_wm_x11_close_id(struct vt_wm_x11 *eng, uint32_t id);
void vt_wm_x11_minimize_id(struct vt_wm_x11 *eng, uint32_t id, bool on);
void vt_wm_x11_maximize_id(struct vt_wm_x11 *eng, uint32_t id, bool on);
void vt_wm_x11_fullscreen_id(struct vt_wm_x11 *eng, uint32_t id, bool on);
void vt_wm_x11_move_id(struct vt_wm_x11 *eng, uint32_t id, int x, int y);
void vt_wm_x11_resize_id(struct vt_wm_x11 *eng, uint32_t id, int w, int h);
void vt_wm_x11_tile_id(struct vt_wm_x11 *eng, uint32_t id, vt_wm_tile_t t);
void vt_wm_x11_desktop(struct vt_wm_x11 *eng, int d);
void vt_wm_x11_move_to_desktop_id(struct vt_wm_x11 *eng, uint32_t id, int d);
bool vt_wm_x11_is_dock(struct vt_wm_x11 *eng, uint32_t id);
unsigned long vt_wm_x11_opacity(struct vt_wm_x11 *eng, uint32_t id);
/* Engine lifecycle, called by vt_wm_start / vt_wm_free */
struct vt_wm_x11 *vt_wm_x11_new_impl(vt_wm_t *wm);
int  vt_wm_x11_start(struct vt_wm_x11 *eng);
void vt_wm_x11_stop(struct vt_wm_x11 *eng);
void vt_wm_x11_free(struct vt_wm_x11 *eng);

#ifdef __cplusplus
}
#endif
#endif
