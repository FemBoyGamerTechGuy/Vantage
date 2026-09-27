/*
 * session.c — the user/session menu
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Logout/Reboot/Shutdown go to the SESSION manager over its IPC socket
 * (supervised end of every child — no restart loops, power actions run
 * after the compositor released the screen).
 */
#include "session.h"
#include "wm.h"
#include "picon.h"

#include <string.h>
#include <unistd.h>

static void _act(const char *action) {
    vp_session_action(action);
}

typedef struct {
    GtkWidget *button;
} _session_t;
static _session_t S = {0};
static GtkWidget *session_button_current(void);

/* response index 0 = Cancel, 1 = the action button */
static void _on_alert_response(GtkAlertDialog *dlg, gint response,
                               gpointer user) {
    (void)user;
    if (response == 1) {
        const char *action = g_object_get_data(G_OBJECT(dlg), "action");
        if (action)
            _act(action);
    }
}

static void _confirm(GtkWindow *parent, const char *title,
                     const char *ok_label, const char *action) {
    /* GtkAlertDialog (4.10+) replaces the deprecated GtkMessageDialog;
     * show() keeps the object alive until "response" has fired, so the
     * initial ref can be dropped right here */
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", title);
    const char *buttons[3] = { "Cancel", ok_label, NULL };
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);
    g_object_set_data_full(G_OBJECT(dlg), "action", g_strdup(action),
                           g_free);
    g_signal_connect(dlg, "response", G_CALLBACK(_on_alert_response),
                     NULL);
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
}

static void _on_item_act(GSimpleAction *a, GVariant *p, gpointer user) {
    (void)a; (void)p;
    /* ask before destructive actions */
    const char *action = user;
    GtkWindow *panel = GTK_WINDOW(gtk_widget_get_ancestor(
        GTK_WIDGET(session_button_current()), GTK_TYPE_WINDOW));
    if (!strcmp(action, "logout"))
        _confirm(panel, "Log out of this session?", "Log Out", action);
    else if (!strcmp(action, "reboot"))
        _confirm(panel, "Reboot the machine?", "Restart", action);
    else if (!strcmp(action, "shutdown"))
        _confirm(panel, "Power off the machine?", "Power Off", action);
}

static GtkWidget *session_button_current(void) {
    return S.button;
}

GtkWidget *session_button_new(void) {
    GtkWidget *btn = gtk_menu_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, "Session");

    const char *user = g_get_user_name();
    char label[64];
    snprintf(label, sizeof(label), " %s ", user && *user ? user : "user");
    GtkWidget *lbl = gtk_label_new(label);
    GtkWidget *ic = vp_icon_image_new("system-shutdown-symbolic", 18,
                                      VP_GLYPH_POWER);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox), ic);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), hbox);

    GSimpleActionGroup *ag = g_simple_action_group_new();
    static const struct { const char *name; const char *action; } items[] = {
        { "logout",   "logout" },
        { "reboot",   "reboot" },
        { "shutdown", "shutdown" },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        GSimpleAction *act = g_simple_action_new(items[i].name, NULL);
        g_signal_connect(act, "activate", G_CALLBACK(_on_item_act),
                         (gpointer)items[i].action);
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(act));
        g_object_unref(act);
    }
    gtk_widget_insert_action_group(btn, "session", G_ACTION_GROUP(ag));

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Log Out",   "session.logout");
    g_menu_append(menu, "Restart",   "session.reboot");
    g_menu_append(menu, "Shut Down", "session.shutdown");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn),
                                   G_MENU_MODEL(menu));
    g_object_unref(menu);
    S.button = btn;
    return btn;
}
