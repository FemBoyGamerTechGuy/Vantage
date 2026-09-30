/*
 * picon.c — safe icon lookups for the panel (see picon.h)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#define _GNU_SOURCE /* readlink */

#include "picon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef VP_PKGDATADIR
#define VP_PKGDATADIR "/usr/local/share/vantage-panel"
#endif

/* ---------------------------------------------------- safety-net dir */

static char _dir_buf[PATH_MAX];
static const char *_icon_dir = NULL;

static char *_exe_dir(void) {
    ssize_t n = readlink("/proc/self/exe", _dir_buf, sizeof(_dir_buf) - 1);
    if (n <= 0) return NULL;
    _dir_buf[n] = 0;
    char *slash = strrchr(_dir_buf, '/');
    if (!slash) return NULL;
    *slash = 0;
    return _dir_buf;
}

/* the directory with the shipped PNGs — located wherever the panel
 * was built or installed:
 *   1. $VANTAGE_PANEL_ICON_DIR (explicit override)
 *   2. next to the executable (in-tree subproject build):
 *      <builddir>/subprojects/panel → <repo>/subprojects/panel/data/icons
 *   3. standalone build: <subprojects/panel>/build → ../data/icons
 *   4. compiled installation prefix: <prefix>/share/vantage-panel/icons
 *   5. XDG data dirs: $XDG_DATA_HOME/vantage-panel/icons …
 * Accepted only when image-missing.png is actually there. */
static const char *_find_icon_dir(void) {
    static char found[PATH_MAX];

    const char *env = getenv("VANTAGE_PANEL_ICON_DIR");
    if (env && *env) {
        snprintf(found, sizeof(found), "%s", env);
        return found;
    }

    /* candidates as full paths, checked in order */
    char paths[8][PATH_MAX];
    int n = 0;
    char *exe = _exe_dir();
    if (exe) {
        /* in-tree subproject: <builddir>/subprojects/panel */
        snprintf(paths[n++], PATH_MAX, "%s/../../../subprojects/panel/data/icons", exe);
        /* standalone meson build: <panel>/build */
        snprintf(paths[n++], PATH_MAX, "%s/../data/icons", exe);
        /* installed layout: <prefix>/bin → <prefix>/share/vantage-panel/icons */
        snprintf(paths[n++], PATH_MAX, "%s/../share/vantage-panel/icons", exe);
    }
    snprintf(paths[n++], PATH_MAX, "%s/icons", VP_PKGDATADIR);
    const char *xdh = getenv("XDG_DATA_HOME");
    char xdhbuf[PATH_MAX];
    if (!xdh || !*xdh) {
        const char *home = getenv("HOME");
        snprintf(xdhbuf, sizeof(xdhbuf), "%s/.local/share",
                 home && *home ? home : "");
        xdh = xdhbuf;
    }
    if (*xdh)
        snprintf(paths[n++], PATH_MAX, "%.*s/vantage-panel/icons",
                 (int)(PATH_MAX - 32), xdh);

    for (int i = 0; i < n; i++) {
        char probe[PATH_MAX + 32];
        snprintf(probe, sizeof(probe), "%s/image-missing.png", paths[i]);
        if (access(probe, R_OK) == 0) {
            snprintf(found, sizeof(found), "%s", paths[i]);
            return found;
        }
    }
    return NULL;
}

void vp_icon_safety_net(void) {
    if (_icon_dir) return;                     /* already applied */
    const char *dir = _find_icon_dir();
    if (!dir) {
        /* no shipped icons found — the panel still never crashes: all
         * of its OWN lookups are gated through has_icon with drawn
         * fallbacks; this is about GTK's internal widgets */
        g_warning("vantage-panel: safety-net icon directory not found "
                  "(install vantage-panel or set VANTAGE_PANEL_ICON_DIR)");
        _icon_dir = "";
        return;
    }
    _icon_dir = dir;
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(
        gdk_display_get_default());
    if (theme)
        gtk_icon_theme_add_search_path(theme, dir);
}

/* ------------------------------------------------------- drawn glyphs */

static void _g_dots(cairo_t *cr, int w, int h) {
    const double gap = 3.5;
    double cell = (w - 2 * gap) / 3.0;
    double dot = cell * 0.42;
    cairo_set_source_rgba(cr, 0.93, 0.94, 0.95, 1.0);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            double x = gap + c * (cell + gap) + (cell - dot) / 2;
            double y = gap + r * (cell + gap) + (cell - dot) / 2;
            cairo_arc(cr, x + dot / 2, y + dot / 2, dot / 2, 0, 6.28);
            cairo_fill(cr);
        }
    (void)h;
}

static void _g_window(cairo_t *cr, int w, int h) {
    double m = 2.0, tb = w * 0.22;
    cairo_set_source_rgba(cr, 0.62, 0.64, 0.68, 1.0);
    cairo_set_line_width(cr, 1.4);
    cairo_rectangle(cr, m, m, w - 2 * m, h - 2 * m);
    cairo_stroke(cr);
    cairo_move_to(cr, m, m + tb);
    cairo_line_to(cr, w - m, m + tb);
    cairo_stroke(cr);
    cairo_arc(cr, m + 3.4, m + tb / 2, 1.1, 0, 6.28);
    cairo_fill(cr);
}

static void _g_power(cairo_t *cr, int w, int h) {
    double cx = w / 2.0, cy = h / 2.0, r = w * 0.30;
    cairo_set_source_rgba(cr, 0.93, 0.94, 0.95, 1.0);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, cx, cy, r, -0.55, 2.60);   /* ring with a top gap */
    cairo_stroke(cr);
    cairo_move_to(cr, cx, cy - r - 1.5);
    cairo_line_to(cr, cx, cy - r + 2.5);     /* the power bar */
    cairo_stroke(cr);
}

static void _g_volume(cairo_t *cr, int w, int h, int waves, bool mute) {
    double u = w / 24.0;   /* design unit: 24px grid */
    cairo_set_source_rgba(cr, 0.93, 0.94, 0.95, 1.0);
    /* speaker */
    cairo_move_to(cr, 3 * u, 10 * u);
    cairo_line_to(cr, 7 * u, 10 * u);
    cairo_line_to(cr, 11 * u, 6 * u);
    cairo_line_to(cr, 11 * u, 18 * u);
    cairo_line_to(cr, 7 * u, 14 * u);
    cairo_line_to(cr, 3 * u, 14 * u);
    cairo_close_path(cr);
    cairo_fill(cr);
    if (mute) {
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, 14 * u, 9 * u);
        cairo_line_to(cr, 20 * u, 15 * u);
        cairo_move_to(cr, 20 * u, 9 * u);
        cairo_line_to(cr, 14 * u, 15 * u);
        cairo_stroke(cr);
    } else {
        cairo_set_line_width(cr, 1.5);
        for (int i = 1; i <= waves; i++) {
            cairo_save(cr);
            cairo_translate(cr, 13 * u + i * 2.6 * u, 12 * u);
            cairo_scale(cr, i * 0.55, i * 0.55);
            cairo_arc(cr, 0, 0, 4 * u / (double)i, -1.05, 1.05);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }
    (void)h;
}

static void _g_network(cairo_t *cr, int w, int h, bool offline) {
    double u = w / 24.0;
    cairo_set_source_rgba(cr, 0.93, 0.94, 0.95, 1.0);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 3; i++) {
        double bx = (3.0 + i * 2.6) * u;
        double bh = (5.0 + i * 4.0) * u;
        double by = 18.0 * u - bh;
        cairo_rectangle(cr, bx, by, 1.9 * u, bh);
        cairo_fill(cr);
    }
    if (offline) {
        cairo_set_source_rgba(cr, 0.85, 0.35, 0.35, 1.0);
        cairo_move_to(cr, 2.4 * u, 19.6 * u);
        cairo_line_to(cr, 13.6 * u, 3.4 * u);
        cairo_stroke(cr);
    }
    (void)h;
}

static void _draw_glyph(cairo_t *cr, int w, int h, vp_glyph_t g) {
    switch (g) {
    case VP_GLYPH_DOTS:        _g_dots(cr, w, h); break;
    case VP_GLYPH_WINDOW:      _g_window(cr, w, h); break;
    case VP_GLYPH_POWER:       _g_power(cr, w, h); break;
    case VP_GLYPH_VOLUME_HIGH: _g_volume(cr, w, h, 2, false); break;
    case VP_GLYPH_VOLUME_MED:  _g_volume(cr, w, h, 1, false); break;
    case VP_GLYPH_VOLUME_LOW:  _g_volume(cr, w, h, 0, false); break;
    case VP_GLYPH_VOLUME_MUTE: _g_volume(cr, w, h, 0, true); break;
    case VP_GLYPH_NET_ONLINE:  _g_network(cr, w, h, false); break;
    case VP_GLYPH_NET_OFFLINE: _g_network(cr, w, h, true); break;
    case VP_GLYPH_NONE: break;
    }
}

/* --------------------------------------------------------- theme path */

static GtkIconTheme *_theme(void) {
    return gtk_icon_theme_get_for_display(gdk_display_get_default());
}

gboolean vp_icon_exists(const char *name) {
    if (!name || !*name) return FALSE;
    return gtk_icon_theme_has_icon(_theme(), name);
}

static GdkPaintable *_lookup(const char *name, int px) {
    /* only called after vp_icon_exists() said YES — the fallback
     * recursion can never be reached from here */
    return GDK_PAINTABLE(gtk_icon_theme_lookup_icon(
        _theme(), name, NULL, px, 1, GTK_TEXT_DIR_NONE, 0));
}

static GdkPaintable *_glyph_texture(int px, vp_glyph_t g) {
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    _draw_glyph(cr, px, px, g);
    cairo_destroy(cr);
    /* cairo ARGB32 memory layout on little endian IS
     * GDK_MEMORY_DEFAULT (premultiplied B8G8R8A8) — wrap the pixels
     * without conversion. gdk_memory_texture_new copies the bytes. */
    int stride = cairo_image_surface_get_stride(surf);
    GBytes *bytes = g_bytes_new(
        cairo_image_surface_get_data(surf), (gsize)stride * (gsize)px);
    GdkTexture *tex = gdk_memory_texture_new(
        px, px, GDK_MEMORY_DEFAULT, bytes, stride);
    g_bytes_unref(bytes);
    cairo_surface_destroy(surf);
    return GDK_PAINTABLE(tex);
}

GtkWidget *vp_icon_image_new(const char *name, int px, vp_glyph_t glyph) {
    if (vp_icon_exists(name)) {
        GdkPaintable *p = _lookup(name, px);
        if (p) {
            GtkWidget *img = gtk_image_new_from_paintable(p);
            gtk_image_set_pixel_size(GTK_IMAGE(img), px);
            g_object_unref(p);
            return img;
        }
    }
    if (glyph == VP_GLYPH_NONE) return NULL;
    GdkPaintable *p = _glyph_texture(px, glyph);
    GtkWidget *img = gtk_image_new_from_paintable(p);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    g_object_unref(p);
    return img;
}

gboolean vp_icon_image_set(GtkImage *img, const char *name, int px,
                           vp_glyph_t glyph) {
    GdkPaintable *p = NULL;
    if (vp_icon_exists(name))
        p = _lookup(name, px);
    if (!p) {
        if (glyph == VP_GLYPH_NONE) return FALSE;
        p = _glyph_texture(px, glyph);
    }
    gtk_image_set_from_paintable(img, p);
    gtk_image_set_pixel_size(img, px);
    g_object_unref(p);
    return TRUE;
}
