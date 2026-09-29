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
    /* window id → button: buttons are UPDATED in place (label, css
     * classes, tooltip) instead of being destroyed and recreated on
     * every refresh. The teardown-everything rebuild reset hover
     * state 2.5x/second (the taskbar active/inactive FLICKER while
     * merely hovering) and destroyed the button a popover menu was
     * parented to — the dropdown closed itself before a menu item
     * could be clicked. */
    GHashTable *by_id;
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

static void _on_act_min(GtkButton *b, gpointer user) {
    (void)b;
    guint32 id = GPOINTER_TO_UINT(user);
    vp_wm_minimize(T.wm, id);
}

static void _on_act_close(GtkButton *b, gpointer user) {
    (void)b;
    guint32 id = GPOINTER_TO_UINT(user);
    vp_wm_close(T.wm, id);
}

/* a context-menu row (the session-menu pattern: plain buttons in a
 * GtkPopover — the mechanism PROVEN against this compositor; the
 * GMenu/GtkPopoverMenu variant's model items never received their
 * button events inside this panel) */
static GtkWidget *_menu_row(const char *label, int glyph,
                            GCallback cb, guint32 id) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(hbox, 4);
    gtk_widget_set_margin_bottom(hbox, 4);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    (void)glyph;
    g_signal_connect(btn, "clicked", cb, GUINT_TO_POINTER(id));
    return btn;
}

static void _pop_closed(GtkWidget *pop, gpointer user) {
    (void)user;
    /* unparent returns the (sunk-at-set_parent) widget to us; drop it */
    gtk_widget_unparent(pop);
    g_object_unref(pop);
}

/* right-click context menu: a plain GtkPopover with button rows —
 * the session/Programs-menu mechanism PROVEN against this
 * compositor. The previous gtk_popover_menu_new_from_model variant
 * had three independent defects, all observed live:
 *   (1) NO CSS class: the stock popover painted the THEME's white
 *       sheet while the bar's light foreground (#e8eaed, inherited
 *       through the button) colored the labels — WHITE TEXT ON
 *       WHITE, a fully functional but INVISIBLE menu.
 *   (2) pointing_to (0,0,1,1): anchored at the button's top-left
 *       CORNER, so the sheet opened OVER the button and the bar.
 *   (3) the GMenu model items never received their button-press
 *       events inside this panel (a minimal reproduction of the
 *       exact same widget/protocol sequence works standalone — an
 *       upstream GtkPopoverMenu interaction we refuse to depend
 *       on). Plain buttons with ::clicked work — the harness drives
 *       this path end-to-end. */
static gboolean _on_button_press(GtkGestureClick *g, int n, double x,
                                 double y, gpointer user) {
    (void)x; (void)y; (void)user;
    guint btn = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(g));
    GtkWidget *w = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(g));
    guint32 id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "win"));
    if (btn == 2 && n == 1) {          /* middle click: close */
        vp_wm_close(T.wm, id);
        return GDK_EVENT_STOP;
    }
    if (btn == 3 && n == 1) {          /* right click: context menu */
        GtkWidget *pop = gtk_popover_new();
        gtk_widget_add_css_class(pop, "vantage-menu");
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_top(vbox, 6);
        gtk_widget_set_margin_bottom(vbox, 6);
        gtk_widget_set_margin_start(vbox, 6);
        gtk_widget_set_margin_end(vbox, 6);
        GtkWidget *min_row = _menu_row("Minimize", VP_GLYPH_WINDOW,
                                       G_CALLBACK(_on_act_min), id);
        GtkWidget *close_row = _menu_row("Close", VP_GLYPH_WINDOW,
                                         G_CALLBACK(_on_act_close), id);
        gtk_box_append(GTK_BOX(vbox), min_row);
        gtk_box_append(GTK_BOX(vbox), close_row);
        /* activating a row also dismisses the menu (the session
         * menu's popover-swap discipline) */
        g_signal_connect_swapped(min_row, "clicked",
                                 G_CALLBACK(gtk_popover_popdown), pop);
        g_signal_connect_swapped(close_row, "clicked",
                                 G_CALLBACK(gtk_popover_popdown), pop);
        gtk_popover_set_child(GTK_POPOVER(pop), vbox);
        gtk_popover_set_autohide(GTK_POPOVER(pop), TRUE);
        g_object_ref(pop);          /* our lifetime handle */
        gtk_widget_set_parent(pop, w);   /* sinks the floating ref */
        /* anchor at the button's FULL rect (button-local coords) so
         * the menu hangs BELOW the button, never over the bar */
        GdkRectangle rect = { .x = 0, .y = 0,
                              .width = gtk_widget_get_width(w),
                              .height = gtk_widget_get_height(w) };
        if (rect.width < 1) rect.width = 1;
        if (rect.height < 1) rect.height = 1;
        gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);
        g_signal_connect(pop, "closed", G_CALLBACK(_pop_closed), NULL);
        gtk_popover_popup(GTK_POPOVER(pop));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static GtkWidget *_task_button_new(const vp_win_t *win) {
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
    g_signal_connect(mg, "pressed", G_CALLBACK(_on_button_press), NULL);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(mg));
    return btn;
}

/* sync an EXISTING button with the window's current state */
static void _task_button_update(GtkWidget *btn, const vp_win_t *win) {
    GtkWidget *hbox = gtk_button_get_child(GTK_BUTTON(btn));
    if (hbox) {
        /* second child is the label (first is the icon image) */
        GtkWidget *lbl = gtk_widget_get_next_sibling(
            gtk_widget_get_first_child(hbox));
        if (lbl && GTK_IS_LABEL(lbl))
            gtk_label_set_text(GTK_LABEL(lbl),
                               win->title && *win->title ? win->title
                                                          : "(untitled)");
    }
    gtk_widget_set_tooltip_text(btn, win->title ? win->title : "");
    gboolean focused = FALSE, minimized = FALSE;
    if (gtk_widget_has_css_class(btn, "vantage-task-focused"))
        focused = TRUE;
    if (gtk_widget_has_css_class(btn, "vantage-task-min"))
        minimized = TRUE;
    if (focused != (win->focused != FALSE)) {
        if (win->focused)
            gtk_widget_add_css_class(btn, "vantage-task-focused");
        else
            gtk_widget_remove_css_class(btn, "vantage-task-focused");
    }
    if (minimized != (win->minimized != FALSE)) {
        if (win->minimized)
            gtk_widget_add_css_class(btn, "vantage-task-min");
        else
            gtk_widget_remove_css_class(btn, "vantage-task-min");
    }
}

static void _remove_button(GtkWidget *btn) {
    /* a popover may still be parented to this button — popping it down
     * first avoids destroying a menu mid-interaction from the menu's
     * own signal handlers */
    GtkWidget *child = gtk_widget_get_first_child(btn);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_POPOVER(child))
            gtk_popover_popdown(GTK_POPOVER(child));
        child = next;
    }
    gtk_box_remove(GTK_BOX(T.box), btn);
}

void tasklist_refresh(gpointer wm) {
    T.wm = wm;
    vp_wm_t *w = wm;
    if (!w || !T.box) return;
    if (!T.by_id)
        T.by_id = g_hash_table_new(g_direct_hash, g_direct_equal);

    /* pass 1: which existing buttons are still in the model? */
    GHashTable *live = g_hash_table_new(g_direct_hash, g_direct_equal);
    int shown = 0;
    for (guint i = 0; i < w->wins->len && shown < 24; i++) {
        vp_win_t *win = g_ptr_array_index(w->wins, i);
        /* EWMH: docks and desktops are SHELL windows, not tasks — the
         * panel itself and the wallpaper must never appear as
         * "running apps" (they used to, complete with a taskbar
         * button and a taskbar-focused highlight) */
        if (win->dock || win->desktop) continue;
        g_hash_table_insert(live, GUINT_TO_POINTER(win->id), win);
        shown++;
    }

    /* pass 2: drop buttons whose windows are gone */
    GList *keys = g_hash_table_get_keys(T.by_id);
    for (GList *k = keys; k; k = k->next) {
        guint32 id = GPOINTER_TO_UINT(k->data);
        if (!g_hash_table_contains(live, k->data)) {
            GtkWidget *btn = g_hash_table_lookup(T.by_id, k->data);
            if (btn) _remove_button(btn);
            g_hash_table_remove(T.by_id, k->data);
        }
        (void)id;
    }
    g_list_free(keys);

    /* pass 3: create missing buttons and update existing ones, in
     * MODEL order; reorder only when the sequence actually changed */
    GtkWidget *prev = NULL;
    for (guint i = 0; i < w->wins->len; i++) {
        vp_win_t *win = g_ptr_array_index(w->wins, i);
        if (win->dock || win->desktop) continue;
        if (!g_hash_table_contains(live, GUINT_TO_POINTER(win->id)))
            continue;
        GtkWidget *btn = g_hash_table_lookup(T.by_id,
                                             GUINT_TO_POINTER(win->id));
        if (!btn) {
            btn = _task_button_new(win);
            gtk_box_append(GTK_BOX(T.box), btn);
            g_hash_table_insert(T.by_id, GUINT_TO_POINTER(win->id), btn);
            if (win->focused)
                gtk_widget_add_css_class(btn, "vantage-task-focused");
            if (win->minimized)
                gtk_widget_add_css_class(btn, "vantage-task-min");
        } else {
            _task_button_update(btn, win);
            /* keep the button's position in sync with the model order */
            GtkWidget *cur_prev =
                gtk_widget_get_prev_sibling(btn);
            if (cur_prev != prev)
                gtk_box_reorder_child_after(GTK_BOX(T.box), btn, prev);
        }
        prev = btn;
    }
    g_hash_table_destroy(live);
    T.last_count = w->wins->len;
}

GtkWidget *tasklist_new(gpointer wm) {
    T.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(T.box, TRUE);
    gtk_widget_set_valign(T.box, GTK_ALIGN_CENTER);
    tasklist_refresh(wm);
    return T.box;
}
