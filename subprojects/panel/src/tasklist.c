/*
 * tasklist.c — taskbar window buttons
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Click focuses/restores, middle click closes, and the icon comes from
 * the icon theme via the window's app_id/WM_CLASS (lowercased) — the
 * same names .desktop files install, so Chromium, Firefox and friends
 * get their real logo.
 */
#include "tasklist.h"
#include "wm.h"
#include "picon.h"

#include <string.h>

typedef struct {
    GtkWidget  *box;        /* the horizontal button container */
    gpointer   wm;          /* vp_wm_t* */
    guint32    last_count;
} _tasklist_t;

static _tasklist_t T = {0};

static GtkWidget *_win_icon(const vp_win_t *w, int px) {
    char candidates[6][96];
    int n = 0;
    const char *app = w->app_id ? w->app_id : "";
    const char *cls = w->cls ? w->cls : "";
    /* Wayland app_id is usually the .desktop base name already;
     * X11 WM_CLASS class is CamelCase → lowercased desktop name */
    if (*app) snprintf(candidates[n++], sizeof(candidates[n - 1]), "%s", app);
    if (*cls) snprintf(candidates[n++], sizeof(candidates[n - 1]),
                       "%s", cls);
    if (*app) {
        char low[96];
        snprintf(low, sizeof(low), "%s", app);
        for (char *p = low; *p; p++) *p = (char)g_ascii_tolower(*p);
        snprintf(candidates[n++], sizeof(candidates[n - 1]), "%s", low);
    }
    if (*cls) {
        char low[96];
        snprintf(low, sizeof(low), "%s", cls);
        for (char *p = low; *p; p++) *p = (char)g_ascii_tolower(*p);
        snprintf(candidates[n++], sizeof(candidates[n - 1]), "%s", low);
    }
    snprintf(candidates[n++], sizeof(candidates[n - 1]),
             "application-x-executable");

    /* vp_icon_image_new checks has_icon() first: an unchecked lookup
     * for a name the theme lacks sends GTK 4.18 into an unbounded
     * image-missing recursion (stack overflow) on themes without
     * that icon; the drawn window glyph is the safe final fallback */
    for (int i = 0; i < n; i++) {
        GtkWidget *img = vp_icon_image_new(candidates[i], px,
                                           VP_GLYPH_NONE);
        if (img) return img;
    }
    return vp_icon_image_new(NULL, px, VP_GLYPH_WINDOW);
}

static void _on_clicked(GtkButton *b, gpointer user) {
    vp_wm_t *wm = T.wm;
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "win"));
    (void)user;
    /* find the window's state to decide the action */
    for (guint i = 0; wm && i < wm->wins->len; i++) {
        vp_win_t *w = g_ptr_array_index(wm->wins, i);
        if (w->id != id) continue;
        if (w->focused && !w->minimized)
            vp_wm_minimize(wm, id);      /* focused click minimizes */
        else
            vp_wm_restore(wm, id), vp_wm_focus(wm, id);
        return;
    }
}

static void _on_act_min(GSimpleAction *a, GVariant *p, gpointer user) {
    (void)a; (void)p;
    guint32 id = GPOINTER_TO_UINT(user);
    vp_wm_minimize(T.wm, id);
}

static void _on_act_close(GSimpleAction *a, GVariant *p, gpointer user) {
    (void)a; (void)p;
    guint32 id = GPOINTER_TO_UINT(user);
    vp_wm_close(T.wm, id);
}

static void _pop_closed(GtkWidget *pop, gpointer user) {
    (void)user;
    /* unparent returns the (sunk-at-set_parent) widget to us; drop it */
    gtk_widget_unparent(pop);
    g_object_unref(pop);
}

static gboolean _on_button_press(GtkGestureClick *g, int n, double x,
                                 double y, gpointer user) {
    (void)x; (void)y; (void)user;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "win"));
    if (btn == 2 && n == 1) {          /* middle click: close */
        vp_wm_close(T.wm, id);
        return GDK_EVENT_STOP;
    }
    if (btn == 3 && n == 1) {          /* right click: context menu */
        GSimpleActionGroup *ag = g_simple_action_group_new();
        GSimpleAction *act;
        act = g_simple_action_new("min", NULL);
        g_signal_connect(act, "activate", G_CALLBACK(_on_act_min),
                         GUINT_TO_POINTER(id));
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(act));
        g_object_unref(act);
        act = g_simple_action_new("close", NULL);
        g_signal_connect(act, "activate", G_CALLBACK(_on_act_close),
                         GUINT_TO_POINTER(id));
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(act));
        g_object_unref(act);
        gtk_widget_insert_action_group(w, "task", G_ACTION_GROUP(ag));

        GMenu *menu = g_menu_new();
        g_menu_append(menu, "Minimize", "task.min");
        g_menu_append(menu, "Close", "task.close");
        GtkWidget *pop = gtk_popover_menu_new_from_model(
            G_MENU_MODEL(menu));
        g_object_unref(menu);
        g_object_ref(pop);          /* our lifetime handle */
        gtk_widget_set_parent(pop, w);   /* sinks the floating ref */
        GdkRectangle rect = { .x = 0, .y = 0, .width = 1, .height = 1 };
        gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
        g_signal_connect(pop, "closed", G_CALLBACK(_pop_closed), NULL);
        gtk_popover_popup(GTK_POPOVER(pop));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

void tasklist_refresh(gpointer wm) {
    T.wm = wm;
    vp_wm_t *w = wm;
    if (!w || !T.box) return;

    /* remove old buttons */
    GtkWidget *child = gtk_widget_get_first_child(T.box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(T.box), child);
        child = next;
    }

    int shown = 0;
    for (guint i = 0; i < w->wins->len && shown < 24; i++) {
        vp_win_t *win = g_ptr_array_index(w->wins, i);
        /* every workspace: the taskbar is a window list, not a
         * per-desktop switcher; the PAGER shows per-desktop layout */
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "vantage-task");
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(hbox, 2);
        gtk_widget_set_margin_bottom(hbox, 2);
        gtk_widget_set_margin_start(hbox, 6);
        gtk_widget_set_margin_end(hbox, 6);
        gtk_box_append(GTK_BOX(hbox), _win_icon(win, 18));
        GtkWidget *lbl = gtk_label_new(
            win->title && *win->title ? win->title : "(untitled)");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(lbl, 10, -1);   /* flex shrink */
        gtk_widget_set_hexpand(lbl, FALSE);
        gtk_box_append(GTK_BOX(hbox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), hbox);
        gtk_widget_set_tooltip_text(btn, win->title ? win->title : "");
        g_object_set_data(G_OBJECT(btn), "win", GUINT_TO_POINTER(win->id));
        g_signal_connect(btn, "clicked", G_CALLBACK(_on_clicked), NULL);

        GtkGesture *mg = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mg), 0);
        g_signal_connect(mg, "pressed", G_CALLBACK(_on_button_press),
                         NULL);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mg));

        if (win->focused)
            gtk_widget_add_css_class(btn, "vantage-task-focused");
        if (win->minimized)
            gtk_widget_add_css_class(btn, "vantage-task-min");
        gtk_box_append(GTK_BOX(T.box), btn);
        shown++;
    }
    T.last_count = w->wins->len;
}

GtkWidget *tasklist_new(gpointer wm) {
    T.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(T.box, TRUE);
    gtk_widget_set_valign(T.box, GTK_ALIGN_CENTER);
    tasklist_refresh(wm);
    return T.box;
}
