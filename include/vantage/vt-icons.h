/*
 * vt-icons.h — system icon-theme lookup for Vantage panels
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * Respects the user's configured icon theme (Papirus, Adwaita, Breeze,
 * …) instead of shipping generated icons:
 *
 *   theme choice:  $VANTAGE_ICON_THEME
 *                  > ~/.config/gtk-3.0/settings.ini gtk-icon-theme-name
 *                  > ~/.config/gtk-4.0/settings.ini
 *                  > "hicolor" (the guaranteed fallback — never a
 *                    hard-coded brand)
 *
 *   lookup:        icon-theme spec subset — index.theme Directories
 *                  (best size match), then the Inherits chain, then
 *                  hicolor, then /usr/share/pixmaps/<name>.png
 */
#ifndef VANTAGE_ICONS_H
#define VANTAGE_ICONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_icon_theme vt_icon_theme_t;

/* Load the icon-theme resolver for the current user theme. Cheap
 * object; call once per surface. Never returns NULL (degrades to
 * hicolor). */
vt_icon_theme_t *vt_icon_theme_load(void);
void             vt_icon_theme_free(vt_icon_theme_t *t);

/* Name of the theme in use (diagnostics). */
const char *vt_icon_theme_name(const vt_icon_theme_t *t);

/* Resolve an icon NAME (or absolute path) at the requested size.
 * Returns 0 and fills out[] with an existing file path, else -1. */
int vt_icon_theme_lookup(const vt_icon_theme_t *t, const char *icon,
                         int size, char *out, size_t out_n);

/* Load any image (png via the built-in decoder, everything gdk-pixbuf
 * understands — svg/xpm/jpeg — when it was compiled in) into
 * ARGB-8888 pixels. Caller frees *out_pixels. Returns 0 on success. */
int vt_icon_load_argb(const char *path, uint32_t **out_pixels,
                      int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
#endif
