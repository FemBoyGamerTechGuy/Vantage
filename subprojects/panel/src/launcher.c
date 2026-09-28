/*
 * launcher.c — the Programs menu
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * A searchable, categorized application menu built on GLib's
 * GDesktopAppInfo database (locale-aware .desktop parsing, exactly what
 * GTK applications use) and GtkIconTheme icons rendered by GTK itself —
 * full theme fidelity at every size, on both backends.
 *
 * The menu is a GtkPopover with a GtkScrolledWindow list: scrolling
 * INSIDE the menu scrolls the list (it never closes the menu — the
 * old panel dismissed itself on wheel events).
 */
#include "launcher.h"
#include "wm.h"
#include "picon.h"

#include <gio/gdesktopappinfo.h>

#include <string.h>

typedef struct {
    const char *id;        /* .desktop category */
    const char *label;     /* menu label */
} _cat_t;

/* the display table — specific-first matching, same set as the
 * compositor-side menus used */
static const _cat_t _cats[] = {
    { "Utility",            "Accessories" },
    { "Development",        "Development" },
    { "Education",          "Education" },
    { "Game",               "Games" },
    { "Graphics",           "Graphics" },
    { "Network",            "Internet" },
    { "AudioVideo",         "Multimedia" },
    { "Office",             "Office" },
    { "Science",            "Science" },
    { "Settings",           "Settings" },
    { "System",             "System" },
    { NULL,                 "Other" },   /* catch-all, index 11 */
    { NULL,                 NULL },      /* terminator */
};

typedef struct {
    GAppInfo *info;
    char     *name;
    char     *exec_key;    /* lowercase search blob */
    int       cat;         /* index into _cats */
} _app_t;

typedef struct {
    GtkWidget  *button;     /* the panel Programs button */
    GtkWidget  *menu;       /* popover */
    GtkWidget  *search;     /* GtkSearchEntry */
    GtkWidget  *cat_list;   /* GtkListBox (left column) */
    GtkWidget  *app_list;   /* GtkListBox (right column) */
    GtkWidget  *app_scroll;
    GPtrArray  *apps;       /* _app_t* */
    int         cur_cat;
    char        filter[64];
    gpointer    wm;         /* vp_wm_t — XWayland env queries at launch */
} _launcher_t;

static _launcher_t L = {0};

/* ---------------------------------------------------------- helpers */

static gchar *_search_blob(GAppInfo *info) {
    const char *name = g_app_info_get_name(info);
    const char *exec = g_app_info_get_executable(info);
    const char *desc = g_app_info_get_description(info);
    GString *s = g_string_new(name ? name : "");
    if (exec) { g_string_append(s, " "); g_string_append(s, exec); }
    if (desc) { g_string_append(s, " "); g_string_append(s, desc); }
    gchar *down = g_utf8_strdown(s->str, -1);
    g_string_free(s, TRUE);
    return down;
}

static int _pick_cat(GDesktopAppInfo *dai) {
    const char *cats = g_desktop_app_info_get_categories(dai);
    if (!cats || !*cats) return 11;   /* Other */
    for (int ci = 0; _cats[ci].id; ci++) {
        size_t len = strlen(_cats[ci].id);
        const char *p = cats;
        while ((p = strstr(p, _cats[ci].id)) != NULL) {
            /* whole-word match (Utility vs Utilityx) */
            bool start_ok = (p == cats) || (p[-1] == ';' || p[-1] == ' ');
            bool end_ok = p[len] == 0 || p[len] == ';' || p[len] == ' ';
            if (start_ok && end_ok) return ci;
            p += len;
        }
    }
    return 11;
}

static int _app_cmp(gconstpointer a, gconstpointer b) {
    const _app_t *x = *(const _app_t * const *)a;
    const _app_t *y = *(const _app_t * const *)b;
    return g_utf8_collate(x->name ? x->name : "", y->name ? y->name : "");
}

static void _free_app(gpointer p) {
    _app_t *a = p;
    if (!a) return;
    if (a->info) g_object_unref(a->info);
    g_free(a->name);
    g_free(a->exec_key);
    g_free(a);
}

static void _reload_apps(void) {
    if (L.apps) g_ptr_array_free(L.apps, TRUE);
    L.apps = g_ptr_array_new_with_free_func(_free_app);
    GList *all = g_app_info_get_all();
    for (GList *it = all; it; it = it->next) {
        GAppInfo *info = it->data;
        if (!G_IS_DESKTOP_APP_INFO(info)) continue;
        if (!g_app_info_should_show(info)) continue;
        GDesktopAppInfo *dai = G_DESKTOP_APP_INFO(info);
        _app_t *a = g_new0(_app_t, 1);
        a->info = g_object_ref(info);
        a->name = g_strdup(g_app_info_get_name(info));
        a->exec_key = _search_blob(info);
        a->cat = _pick_cat(dai);
        g_ptr_array_add(L.apps, a);
    }
    g_list_free(all);
    g_ptr_array_sort(L.apps, _app_cmp);
}

/* Escape in the search entry closes the menu */
static gboolean _on_search_key(GtkEventControllerKey *k, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer pop) {
    (void)k; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        gtk_popover_popdown(GTK_POPOVER(pop));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

/* icon by name with the standard fallback chain; the FINAL fallback is
 * a drawn 3x3 dot grid with NO background plate (the old panel drew a
 * colored square behind the grid that showed through as a stray
 * background on both backends — this one cannot).
 * vp_icon_image_new gates every name through gtk_icon_theme_has_icon:
 * a lookup for a name the theme lacks would send GTK 4.18 into its
 * unbounded image-missing fallback recursion (stack overflow) on
 * themes that do not ship image-missing (minimal/private themes) */
static GtkWidget *_icon_for_app(GAppInfo *info, int px) {
    const char *id = g_app_info_get_id(info);   /* foo.desktop */
    char base[128] = {0};
    if (id) {
        snprintf(base, sizeof(base), "%s", id);
        char *dot = strstr(base, ".desktop");
        if (dot) *dot = 0;
    }
    const char *names[8];
    int n = 0;
    if (base[0]) names[n++] = base;
    const char *icon_name = NULL;
    GIcon *icon = g_app_info_get_icon(info);
    if (icon && G_IS_THEMED_ICON(icon))
        icon_name = g_themed_icon_get_names(G_THEMED_ICON(icon))[0];
    if (icon_name) names[n++] = icon_name;
    names[n++] = "application-x-executable";
    names[n++] = "text-x-generic";

    for (int i = 0; i < n; i++) {
        GtkWidget *img = vp_icon_image_new(names[i], px, VP_GLYPH_NONE);
        if (img) return img;
    }
    /* drawn fallback: 9 dots, transparent background */
    return vp_icon_image_new(NULL, px, VP_GLYPH_DOTS);
}

/* --------------------------------------------------------- app rows */

static void _launch(_app_t *a) {
    if (!a) return;
    /* XWayland apps: this process cannot inherit the compositor's
     * DISPLAY/XAUTHORITY (setenv does not cross process boundaries) —
     * ask the WM for the live Xwayland display and export it BEFORE
     * spawning, so X11 apps launched from the Programs menu actually
     * start under the Wayland session. Native Wayland apps are
     * unaffected: WAYLAND_DISPLAY is already ours and GTK prefers it. */
    vp_wm_xwl_env_apply(L.wm);
    GError *err = NULL;
    if (!g_app_info_launch(a->info, NULL, NULL, &err)) {
        g_warning("launcher: cannot launch %s: %s",
                  a->name ? a->name : "?", err ? err->message : "?");
        g_clear_error(&err);
    } else {
        gtk_popover_popdown(GTK_POPOVER(L.menu));
    }
}

static void _on_row_activated(GtkListBox *box, GtkListBoxRow *row,
                              gpointer user) {
    (void)box; (void)user;
    _app_t *a = g_object_get_data(G_OBJECT(row), "app");
    _launch(a);
}

static void _rebuild_app_list(void) {
    gtk_list_box_unselect_all(GTK_LIST_BOX(L.app_list));

    /* remove all rows */
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(
        GTK_LIST_BOX(L.app_list), 0);
    while (row) {
        gtk_list_box_remove(GTK_LIST_BOX(L.app_list), GTK_WIDGET(row));
        row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(L.app_list), 0);
    }

    const char *filt = L.filter;
    for (guint i = 0; i < L.apps->len; i++) {
        _app_t *a = g_ptr_array_index(L.apps, i);
        if (L.cur_cat >= 0 && L.cur_cat != 11 && a->cat != L.cur_cat)
            continue;
        if (L.cur_cat == 11 && a->cat != 11) {
            /* "Other" shows everything not in the named table */
            continue;
        }
        if (filt && *filt) {
            if (!a->exec_key || !strstr(a->exec_key, filt))
                continue;
        }
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(row_box, 3);
        gtk_widget_set_margin_bottom(row_box, 3);
        gtk_widget_set_margin_start(row_box, 6);
        gtk_widget_set_margin_end(row_box, 6);
        gtk_box_append(GTK_BOX(row_box), _icon_for_app(a->info, 22));
        GtkWidget *lbl = gtk_label_new(a->name);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row_box), lbl);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        g_object_set_data(G_OBJECT(row), "app", a);
        gtk_list_box_append(GTK_LIST_BOX(L.app_list), row);
    }
}

static void _on_cat_activated(GtkListBox *box, GtkListBoxRow *row,
                              gpointer user) {
    (void)box; (void)user;
    if (!row) return;
    L.cur_cat = gtk_list_box_row_get_index(row);
    _rebuild_app_list();
}

static void _on_search_changed(GtkEditable *e, gpointer user) {
    (void)user;
    const char *t = gtk_editable_get_text(e);
    snprintf(L.filter, sizeof(L.filter), "%s", t);
    for (guint i = 0; i < strlen(L.filter); i++)
        L.filter[i] = (char)g_ascii_tolower(L.filter[i]);
    if (L.filter[0]) {
        /* searching: show everything that matches, category-agnostic */
        int saved = L.cur_cat;
        L.cur_cat = -1;
        _rebuild_app_list();
        L.cur_cat = saved;
    } else {
        _rebuild_app_list();
    }
}

/* ---------------------------------------------------------- the menu */

static void _build_menu(void) {
    L.menu = gtk_popover_new();
    /* the shell's own identity: the vantage-menu CSS class pins the
     * popover's palette and row geometry so the menu looks (and
     * hit-tests) the same under every GTK theme — see main.c */
    gtk_widget_add_css_class(L.menu, "vantage-menu");
    gtk_widget_set_size_request(L.menu, 560, 420);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);

    /* search bar — the FIRST thing focused when the menu opens */
    L.search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(L.search),
                                          "Search applications…");
    g_signal_connect(L.search, "search-changed",
                     G_CALLBACK(_on_search_changed), NULL);
    gtk_box_append(GTK_BOX(vbox), L.search);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* categories (left) */
    GtkWidget *cat_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cat_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(cat_scroll, 150, -1);
    L.cat_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(L.cat_list),
                                    GTK_SELECTION_BROWSE);
    g_signal_connect(L.cat_list, "row-activated",
                     G_CALLBACK(_on_cat_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(cat_scroll),
                                  L.cat_list);
    for (int i = 0; _cats[i].label; i++) {
        GtkWidget *lbl = gtk_label_new(_cats[i].label);  /* safe now */
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_widget_set_margin_top(lbl, 3);
        gtk_widget_set_margin_bottom(lbl, 3);
        gtk_widget_set_margin_start(lbl, 8);
        gtk_list_box_append(GTK_LIST_BOX(L.cat_list), lbl);
    }
    gtk_box_append(GTK_BOX(hbox), cat_scroll);

    /* applications (right) — scrollable: wheel events scroll this list
     * and NEVER close the menu */
    L.app_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(L.app_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(L.app_scroll, TRUE);
    gtk_widget_set_vexpand(L.app_scroll, TRUE);
    L.app_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(L.app_list),
                                    GTK_SELECTION_NONE);
    g_signal_connect(L.app_list, "row-activated",
                     G_CALLBACK(_on_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(L.app_scroll),
                                  L.app_list);
    gtk_box_append(GTK_BOX(hbox), L.app_scroll);

    gtk_box_append(GTK_BOX(vbox), hbox);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_popover_set_child(GTK_POPOVER(L.menu), vbox);

    /* Escape in the search bar closes the menu */
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(_on_search_key), L.menu);
    gtk_widget_add_controller(L.search, k);

    L.cur_cat = 0;
    _rebuild_app_list();
}

/* --------------------------------------------------------- the button */

/* the Programs button icon: themed start icon with a broad fallback
 * chain; final fallback = the drawn 9-dot grid (transparent) */
static GtkWidget *_start_icon(int px) {
    const char *names[] = {
        "start-here", "view-app-grid-symbolic", "applications-all",
        "start-here-symbolic", "org.gnome.Apps", "open-menu-symbolic",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        GtkWidget *img = vp_icon_image_new(names[i], px, VP_GLYPH_NONE);
        if (img) return img;
    }
    return vp_icon_image_new(NULL, px, VP_GLYPH_DOTS);
}

GtkWidget *launcher_button_new(void) {
    _reload_apps();
    _build_menu();

    L.button = gtk_menu_button_new();
    /* A CUSTOM child (icon + label) instead of set_label(): a label
     * child would still make GtkMenuButton snapshot its builtin arrow
     * (pan-down-symbolic) — an unchecked icon lookup that recurses to
     * stack overflow in GTK 4.18 when the active theme lacks it. With
     * a custom child NO arrow is drawn at all: the XFCE-style start
     * button (icon + text) with zero theme dependencies. */
    GtkWidget *child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(child), _start_icon(22));
    GtkWidget *lbl = gtk_label_new("Programs");
    gtk_box_append(GTK_BOX(child), lbl);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(L.button), child);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(L.button), L.menu);
    gtk_widget_set_tooltip_text(L.button, "Application menu");

    gtk_widget_add_css_class(L.button, "flat");
    gtk_widget_add_css_class(L.button, "vantage-start");
    return L.button;
}

void launcher_set_model(gpointer wm) {
    L.wm = wm;   /* for XWayland DISPLAY queries at launch time */
}
