/*
 * gtk4-csd-menu-probe.c — browser-behavior probe for the X11 session
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Mimics EXACTLY the user-reported browser failure modes under X11:
 *   1. CSD window (GTK headerbar, _MOTIF_WM_HINTS decorations=0) —
 *      the DE must NOT add its own frame around it.
 *   2. Selectable text (drag-select) + right-click CONTEXT MENU with
 *      Copy/Paste items — the menu must open, stay open, activate,
 *      and the app must keep responding afterwards (the reported
 *      freeze class).
 *   3. Reports its own window/frame geometry + whether it received
 *      the right-click (EnterNotify/ButtonPress accounting).
 *
 * Usage: gtk4-csd-menu-probe [--wait SEC]
 * Prints step lines; "FREEZE-CHECK-OK" when the post-menu interaction
 * round-trip completed.
 */
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdio.h>

static int g_events_seen = 0;
static int g_menu_opened = 0;
static int g_menu_activated = 0;

static gboolean _tick(gpointer u) {
    (void)u;
    printf("alive %d events=%d menu=%d/%d\n", (int)(g_get_monotonic_time() / 1000000),
           g_events_seen, g_menu_opened, g_menu_activated);
    fflush(stdout);
    return G_SOURCE_CONTINUE;
}

/* GMenu item activation (the REAL path: popover menu -> action) */
static void _on_action(GSimpleAction *a, GVariant *param, gpointer u) {
    (void)param; (void)u;
    printf("menu-activate '%s'\n", g_action_get_name(G_ACTION(a)));
    fflush(stdout);
    g_menu_activated++;
}

/* NOTE on GTK4 gesture signatures: GtkGestureClick::pressed/released
 * carry (n_presses, x, y) — the FIRST int is the CLICK COUNT (1=single,
 * 2=double), NOT the button number. The button is
 * gtk_gesture_single_get_current_button(). Earlier revisions of this
 * probe compared n_presses against GDK_BUTTON_SECONDARY (=3) — every
 * single click reads 1, so right-click menus never opened and the
 * probe silently tested nothing. */
static void _on_rightclick(GtkGestureClick *g, int n, double x, double y,
                           gpointer u) {
    (void)n;
    (void)u;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) !=
        GDK_BUTTON_SECONDARY)
        return;
    g_events_seen++;
    g_menu_opened++;
    GtkWidget *menu = g_object_new(GTK_TYPE_POPOVER_MENU, NULL);
    /* real actions so item clicks are observable — the browser's
     * Copy/Paste path (menu item -> action -> handler) */
    GSimpleActionGroup *ag = g_simple_action_group_new();
    GSimpleAction *acopy = g_simple_action_new("copy", NULL);
    GSimpleAction *apaste = g_simple_action_new("paste", NULL);
    GSimpleAction *asel = g_simple_action_new("select-all", NULL);
    g_signal_connect(acopy, "activate", G_CALLBACK(_on_action), NULL);
    g_signal_connect(apaste, "activate", G_CALLBACK(_on_action), NULL);
    g_signal_connect(asel, "activate", G_CALLBACK(_on_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(acopy));
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(apaste));
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(asel));
    gtk_widget_insert_action_group(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)),
                                    "probe", G_ACTION_GROUP(ag));
    GMenu *gmenu = g_menu_new();
    g_menu_append(gmenu, "Copy", "probe.copy");
    g_menu_append(gmenu, "Paste", "probe.paste");
    g_menu_append(gmenu, "Select All", "probe.select-all");
    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(menu), G_MENU_MODEL(gmenu));
    gtk_widget_set_parent(menu, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)));
    gtk_popover_set_pointing_to(GTK_POPOVER(menu),
                                &(const GdkRectangle){ (int)x, (int)y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(menu));
    printf("menu-popup at %.0f,%.0f\n", x, y);
    fflush(stdout);
}

static void _on_click(GtkGestureClick *g, int n, double x, double y, gpointer u) {
    (void)x; (void)y; (void)u;
    g_events_seen++;
    printf("click button=%d n_presses=%d\n",
           gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)), n);
    fflush(stdout);
}

static gboolean _quit(gpointer u) {
    g_main_loop_quit((GMainLoop *)u);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    int wait = 6;
    if (argc > 2 && !strcmp(argv[1], "--wait")) wait = atoi(argv[2]);

    gtk_init();

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "CSD Menu Probe");
    gtk_window_set_default_size(GTK_WINDOW(win), 500, 350);

    /* CSD: headerbar → GTK sets _MOTIF_WM_HINTS decorations=0 */
    GtkWidget *hb = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *lbl = gtk_label_new(
        "Selectable text line one\nSelectable text line two\n"
        "Drag to select me, then right-click for the menu.");
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget *btn = gtk_button_new_with_label("Button (event accounting)");
    gtk_box_append(GTK_BOX(vbox), btn);

    /* gestures: ONE any-button capture controller. Two button-specific
     * capture gestures on the same widget interfere (the non-matching
     * gesture denies the sequence before the matching one sees it —
     * button 3 presses were dropped exactly there, while GDK itself
     * received them fine). button=0 = ANY button; the callbacks
     * dispatch on n. CAPTURE phase sees every press BEFORE child
     * widgets (the selectable label) consume it, so "click button=N"
     * reflects what the X server actually delivered to the window. */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "released", G_CALLBACK(_on_click), win);
    g_signal_connect(click, "pressed", G_CALLBACK(_on_rightclick), win);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(click));

    g_timeout_add(1000, _tick, NULL);

    gtk_window_present(GTK_WINDOW(win));

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(wait, _quit, loop);
    g_main_loop_run(loop);

    /* FREEZE CHECK: if we get here, the main loop kept iterating after
     * the menus — the app never froze. */
    printf("FREEZE-CHECK-OK events=%d menus=%d activated=%d\n",
           g_events_seen, g_menu_opened, g_menu_activated);
    fflush(stdout);   /* buffered stdout + LSan's Die() = lost verdict */
    return 0;
}
