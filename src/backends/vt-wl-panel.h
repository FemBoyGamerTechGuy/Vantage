/*
 * vt-wl-panel.h — compositor-side panel for the native Wayland backend
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * Internal header (not installed): vt-backend-wayland.c owns one panel
 * instance and routes pointer/key input through it before any client.
 *
 * The panel shares the desktop-wide application database (vt-apps.h)
 * with the X11 panel — one .desktop parser, one category table, one
 * icon theme resolver. There is no backend-private app list.
 */
#ifndef VANTAGE_WL_PANEL_H
#define VANTAGE_WL_PANEL_H

#include <vantage/vt-core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vt_wl_panel vt_wl_panel_t;

typedef struct {
    uint64_t id;
    const char *title;
    bool focused;
} vt_wl_panel_win_t;

typedef struct {
    void (*focus_window)(uint64_t id, void *ud);
    void (*close_window)(uint64_t id, void *ud);
    void (*switch_ws)(int ws, void *ud);
    /* action: "" logout, "exit", "reboot", "shutdown" — routed through
     * the session manager when one is running */
    void (*logout)(const char *action, void *ud);
} vt_wl_panel_cbs_t;

vt_wl_panel_t *vt_wl_panel_create(int width, int bar_height);
void vt_wl_panel_destroy(vt_wl_panel_t *p);
void vt_wl_panel_resize(vt_wl_panel_t *p, int width);
int  vt_wl_panel_height(const vt_wl_panel_t *p);
void vt_wl_panel_set_callbacks(vt_wl_panel_t *p,
                               const vt_wl_panel_cbs_t *cbs, void *ud);
void vt_wl_panel_set_workspaces(vt_wl_panel_t *p, int count, int cur);
void vt_wl_panel_set_windows(vt_wl_panel_t *p,
                             const vt_wl_panel_win_t *wins, size_t n);

/* paint the bar + any open menu into the framebuffer */
void vt_wl_panel_paint(vt_wl_panel_t *p, uint32_t *fb, int fbw, int fbh);

/* input: returns true when the event was consumed (was inside the bar
 * or a menu). kind: 0=motion, 1=press, 2=release. */
bool vt_wl_panel_pointer(vt_wl_panel_t *p, int x, int y, int kind,
                         int button);
/* wheel: dir +1 = down/away, -1 = up/toward. Returns true consumed. */
bool vt_wl_panel_axis(vt_wl_panel_t *p, int x, int y, int dir);
bool vt_wl_panel_contains(const vt_wl_panel_t *p, int x, int y);
/* key routing while a menu is open. combo may be NULL; cp is a UTF-32
 * codepoint (0 when the key is not printable). */
bool vt_wl_panel_key(vt_wl_panel_t *p, const char *combo, uint32_t cp);

#endif
