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

/* Load any image (png via the built-in decoder, svg via librsvg when
 * compiled in, everything gdk-pixbuf understands — xpm/jpeg — when it
 * was compiled in) into ARGB-8888 pixels. Caller frees *out_pixels.
 * Returns 0 on success. */
int vt_icon_load_argb(const char *path, uint32_t **out_pixels,
                      int *out_w, int *out_h);

/* Load an icon and rasterize SVG sources at the TARGET size (SVGs are
 * resolution-independent: rendering them at exactly the display size
 * is what keeps icons crisp instead of upscaled 16px rasters).
 * Returns 0 on success. */
int vt_icon_load_argb_sized(const char *path, int target,
                            uint32_t **out_pixels, int *out_w, int *out_h);

/* High-quality resampler: box-filter when downscaling (area-average,
 * no aliasing), bilinear when upscaling. Straight nearest-neighbour
 * scaling is what made panel icons look "low resolution": 48px icons
 * crushed to 18px lost 3 of every 4 pixel columns and aliased badly. */
uint32_t *vt_icon_scale_argb(const uint32_t *src, int sw, int sh,
                             int dw, int dh);

/* One-call helper: resolve NAME in the theme and return ARGB pixels
 * scaled to exactly size x size (freeing the original). Returns NULL
 * when the icon cannot be resolved — callers draw their honest
 * fallback glyph then. size <= 0 means "native size". */
uint32_t *vt_icon_lookup_argb(const vt_icon_theme_t *t, const char *name,
                              int size);

#ifdef __cplusplus
}
#endif
#endif
