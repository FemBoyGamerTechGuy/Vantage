/*
 * vt-theme.h — Vantage theme engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A single JSON theme file describes the visual design system: colors,
 * radii, spacing, typography, icon theme, dark/light variant, widget
 * appearance. Integration layers (GTK and Qt6) consume the same JSON and
 * generate the appropriate CSS/QSS/INI on the fly.
 *
 * GTK and Qt6 are NOT dependencies of the core. The integration files
 * are generated at theme-apply time and dropped into the appropriate XDG
 * directories so the toolkits pick them up automatically.
 */
#ifndef VANTAGE_THEME_H
#define VANTAGE_THEME_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_theme {
    char  *name;
    bool   dark;
    char  *accent_hex;
    char  *bg_hex;
    char  *fg_hex;
    char  *surface_hex;
    char  *border_hex;
    int    radius;
    int    spacing;
    char  *font_family;
    int    font_size_pt;
    char  *icon_theme;
    char  *gtk_theme;        /* generated name */
    char  *qt_theme;        /* generated name */
    bool   rounded_windows;
    bool   shadows;
    int    shadow_radius;
    bool   generated;        /* set when fields are derived from accent */
} vt_theme_t;

vt_theme_t *vt_theme_new(void);
void        vt_theme_free(vt_theme_t *t);
vt_theme_t *vt_theme_load(const char *path);
vt_theme_t *vt_theme_load_by_name(const char *name);
int         vt_theme_save(const vt_theme_t *t, const char *path);

void        vt_theme_apply(vt_theme_t *t);
void        vt_theme_apply_to_gtk(const vt_theme_t *t);
void        vt_theme_apply_to_qt6(const vt_theme_t *t);

char      **vt_theme_list_installed(size_t *out_n);
char       *vt_theme_dir(void);
char       *vt_theme_path_for(const char *name);

/* Helpers for converting hex<->color */
bool        vt_theme_hex_to_rgba(const char *hex, vt_color_t *out);
char       *vt_theme_rgba_to_hex(vt_color_t c);

/* Material design color generator — derive a full palette from one accent. */
typedef struct vt_palette {
    vt_color_t accent;
    vt_color_t accent_fg;
    vt_color_t bg_light;
    vt_color_t bg_dark;
    vt_color_t fg_light;
    vt_color_t fg_dark;
    vt_color_t surface_light;
    vt_color_t surface_dark;
    vt_color_t border_light;
    vt_color_t border_dark;
    int         radius;
    int         spacing;
    char       font_family[128];
    int         font_size_pt;
} vt_palette_t;

void  vt_theme_palette_from_accent(const char *accent_hex, bool dark,
                                     vt_palette_t *out);

#ifdef __cplusplus
}
#endif
#endif
