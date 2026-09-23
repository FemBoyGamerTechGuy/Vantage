/*
 * vt-panel.h — Vantage panel
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A lightweight panel that hosts plugins. Plugins are tiny C modules
 * (built-in or runtime-loadable via dlopen). The panel exposes a stable
 * ABI for plugins.
 */
#ifndef VANTAGE_PANEL_H
#define VANTAGE_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_PANEL_POS_TOP = 0,
    VT_PANEL_POS_BOTTOM,
    VT_PANEL_POS_LEFT,
    VT_PANEL_POS_RIGHT,
} vt_panel_pos_t;

typedef struct vt_panel      vt_panel_t;
typedef struct vt_panel_plugin vt_panel_plugin_t;
typedef struct vt_panel_applet vt_panel_applet_t;

typedef enum {
    VT_PANEL_APPLET_LAUNCHER = 0,
    VT_PANEL_APPLET_TASKLIST,
    VT_PANEL_APPLET_CLOCK,
    VT_PANEL_APPLET_WORKSPACES,
    VT_PANEL_APPLET_TRAY,
    VT_PANEL_APPLET_VOLUME,
    VT_PANEL_APPLET_NETWORK,
    VT_PANEL_APPLET_BATTERY,
    VT_PANEL_APPLET_USER,
} vt_panel_applet_kind_t;

typedef struct vt_panel_plugin_api {
    void  *(*init)(vt_panel_t *p);
    void   (*fini)(void *inst);
    void   (*render)(void *inst, vt_renderer_t *r, vt_rect_t area);
    void   (*on_event)(void *inst, int ev, void *ev_data);
    size_t (*preferred_size)(void *inst);
    const char *name;
    int    api_version;
} vt_panel_plugin_api_t;

struct vt_panel_applet {
    vt_panel_applet_kind_t kind;
    const vt_panel_plugin_api_t *api;
    void *inst;
    vt_rect_t area;
};

struct vt_panel {
    vt_panel_pos_t pos;
    int            height;
    bool           visible;
    bool           running;
    vt_vec_t       applets;          /* vt_panel_applet_t */
    vt_renderer_t *renderer;
    void          *priv;
};

vt_panel_t *vt_panel_new(vt_renderer_t *r);
void        vt_panel_free(vt_panel_t *p);
int         vt_panel_start(vt_panel_t *p);
void        vt_panel_stop(vt_panel_t *p);
int         vt_panel_add_applet(vt_panel_t *p, vt_panel_applet_kind_t kind);
int         vt_panel_remove_applet(vt_panel_t *p, int idx);
void        vt_panel_render(vt_panel_t *p);
void        vt_panel_set_pos(vt_panel_t *p, vt_panel_pos_t pos);
void        vt_panel_set_height(vt_panel_t *p, int h);

/* Built-in applets */
extern const vt_panel_plugin_api_t vt_panel_applet_launcher;
extern const vt_panel_plugin_api_t vt_panel_applet_tasklist;
extern const vt_panel_plugin_api_t vt_panel_applet_clock;
extern const vt_panel_plugin_api_t vt_panel_applet_workspaces;
extern const vt_panel_plugin_api_t vt_panel_applet_tray;
extern const vt_panel_plugin_api_t vt_panel_applet_volume;
extern const vt_panel_plugin_api_t vt_panel_applet_network;
extern const vt_panel_plugin_api_t vt_panel_applet_battery;

/* Runtime-loadable plugin discovery */
int  vt_panel_applet_register(const vt_panel_plugin_api_t *api);

#ifdef __cplusplus
}
#endif
#endif
