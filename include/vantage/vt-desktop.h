/*
 * vt-desktop.h — Vantage desktop surface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owns the root desktop surface: hosts the wallpaper, optional desktop
 * icons, and the desktop context menu.
 */
#ifndef VANTAGE_DESKTOP_H
#define VANTAGE_DESKTOP_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_desktop      vt_desktop_t;
typedef struct vt_desktop_icon vt_desktop_icon_t;

struct vt_desktop_icon {
    char  *name;
    char  *path;            /* launchable path or URL */
    char  *icon;
    int    x, y, w, h;
};

struct vt_desktop {
    vt_renderer_t *renderer;
    vt_vec_t       icons;    /* vt_desktop_icon_t */
    bool           show_icons;
    bool           running;
    void          *priv;
};

vt_desktop_t *vt_desktop_new(vt_renderer_t *r);
void          vt_desktop_free(vt_desktop_t *d);
int           vt_desktop_start(vt_desktop_t *d);
void          vt_desktop_stop(vt_desktop_t *d);

void          vt_desktop_set_show_icons(vt_desktop_t *d, bool on);
int           vt_desktop_add_icon(vt_desktop_t *d, const char *name,
                                    const char *path, const char *icon,
                                    int x, int y);
void          vt_desktop_remove_icon(vt_desktop_t *d, const char *name);
void          vt_desktop_render(vt_desktop_t *d);
void          vt_desktop_show_menu(vt_desktop_t *d, int x, int y);

#ifdef __cplusplus
}
#endif
#endif
