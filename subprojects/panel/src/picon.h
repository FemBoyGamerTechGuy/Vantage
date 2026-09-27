/*
 * picon.h — safe icon lookups for the panel
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * gtk_icon_theme_lookup_icon() never returns NULL for a missing icon:
 * it falls back to "image-missing" — and when the active theme does
 * not ship that icon either (minimal/private themes, headless test
 * fixtures), GTK 4.18 recurses into itself without bound until the
 * stack overflows (real_choose_icon → image-missing → real_choose_
 * icon → …). Every lookup in this panel therefore goes through these
 * helpers: the name is checked with gtk_icon_theme_has_icon() (a pure
 * query that cannot hit the fallback path) and missing icons render
 * a DRAWN glyph instead — no theme dependency, no recursion, always
 * something sane on screen.
 */
#ifndef VANTAGE_PANEL_PICON_H
#define VANTAGE_PANEL_PICON_H

#include <gtk/gtk.h>

typedef enum {
    VP_GLYPH_NONE = 0,   /* no drawn fallback: returns NULL when absent */
    VP_GLYPH_DOTS,       /* application grid (start/app fallback) */
    VP_GLYPH_WINDOW,     /* generic window (tasklist fallback) */
    VP_GLYPH_POWER,      /* session/power button */
    VP_GLYPH_VOLUME_HIGH,
    VP_GLYPH_VOLUME_MED,
    VP_GLYPH_VOLUME_LOW,
    VP_GLYPH_VOLUME_MUTE,
    VP_GLYPH_NET_ONLINE,
    VP_GLYPH_NET_OFFLINE,
} vp_glyph_t;

/* TRUE when the icon theme really provides `name` (never triggers the
 * image-missing fallback — safe on every theme) */
gboolean vp_icon_exists(const char *name);

/* A GtkImage for `name` at `px`, or the drawn `glyph` when the theme
 * lacks the icon. glyph == VP_GLYPH_NONE + missing icon → NULL. */
GtkWidget *vp_icon_image_new(const char *name, int px, vp_glyph_t glyph);

/* Update an existing GtkImage in place (applet state changes).
 * Returns TRUE when the themed icon was set, FALSE when the glyph was
 * drawn. */
gboolean vp_icon_image_set(GtkImage *img, const char *name, int px,
                           vp_glyph_t glyph);

/* Add the panel's own icon directory (the shipped safety-net PNGs)
 * to the display icon theme's search path. The icons placed directly
 * in a search-path directory resolve as UNTHEMED icons — which GTK
 * consults BEFORE its image-missing fallback — so the unbounded
 * fallback recursion of GTK 4.18 can never fire, and GTK's own
 * widgets (GtkSearchEntry's find/clear icons, menu arrows) always
 * have something to render. Call once at startup, before the first
 * icon lookup. */
void vp_icon_safety_net(void);

#endif /* VANTAGE_PANEL_PICON_H */
