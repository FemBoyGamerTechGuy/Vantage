/*
 * session.c — the user/session menu
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Logout/Reboot/Shutdown go to the SESSION manager over its IPC socket
 * (supervised end of every child — no restart loops, power actions run
 * after the compositor released the screen).
 *
 * The confirmation is a SECOND VIEW INSIDE the menu's own popover — NOT
 * a GtkAlertDialog. A modal dialog is a separate xdg toplevel transient
 * for the panel window; the panel is a LAYER-SHELL surface with no
 * xdg_toplevel to be transient for, so on the compositor the dialog
 * never mapped at all: clicking "Log Out" did nothing (the Wayland
 * logout failure). A popover swap keeps the whole flow inside the one
 * surface that is demonstrably on screen.
 */
#include "session.h"
#include "wm.h"
#include "picon.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GtkWidget *button;      /* the session menu button */
    GtkWidget *pop;         /* its popover */
    GtkWidget *menu_box;    /* primary view: action list */
    GtkWidget *confirm_box; /* secondary view: question + buttons */
    char       action[24];  /* pending action in the confirm view */
} _session_t;
static _session_t S = {0};

static void _act(const char *action) {
    vp_session_action(action);
}

/* ------------------------------------------------- primary menu view */

static void _ask(const char *action);

static void _on_action(GtkButton *b, gpointer user) {
    (void)b;
    _ask((const char *)user);
}

static GtkWidget *_menu_row(const char *icon_name, int glyph,
                            const char *label, const char *action) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(hbox),
                   vp_icon_image_new(icon_name, 16, glyph));
    GtkWidget *lbl = gtk_label_new(label);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    g_signal_connect(btn, "clicked", G_CALLBACK(_on_action),
                     (gpointer)action);
    return btn;
}

/* ------------------------------------------------ confirmation view */

static void _show_view(GtkWidget *view) {
    gtk_widget_set_visible(S.menu_box, view == S.menu_box);
    gtk_widget_set_visible(S.confirm_box, view == S.confirm_box);
}

static void _on_cancel(GtkButton *b, gpointer user) {
    (void)b; (void)user;
    _show_view(S.menu_box);
}

static void _on_confirm(GtkButton *b, gpointer user) {
    (void)b;
    const char *action = user ? user : "logout";
    _act(action);
    /* the session manager is ending everything including this panel;
     * pop the popover so the last frame is clean */
    gtk_popover_popdown(GTK_POPOVER(S.pop));
}

static void _ask(const char *action) {
    if (!S.confirm_box) return;
    snprintf(S.action, sizeof(S.action), "%s", action ? action : "logout");

    const char *question = "Log out of this session?";
    const char *ok = "Log Out";
    int ok_glyph = VP_GLYPH_POWER;
    if (!strcmp(action, "reboot")) {
        question = "Restart the machine?"; ok = "Restart";
    } else if (!strcmp(action, "shutdown")) {
        question = "Power off the machine?"; ok = "Power Off";
    }

    /* rebuild the confirm view for this action */
    GtkWidget *child = gtk_widget_get_first_child(S.confirm_box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(S.confirm_box), child);
        child = next;
    }
    GtkWidget *lbl = gtk_label_new(question);
    gtk_widget_add_css_class(lbl, "vantage-confirm-text");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(S.confirm_box), lbl);

    GtkWidget *brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(brow, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel, "flat");
    g_signal_connect(cancel, "clicked", G_CALLBACK(_on_cancel), NULL);
    gtk_box_append(GTK_BOX(brow), cancel);
    GtkWidget *okbtn = gtk_button_new_with_label(ok);
    gtk_widget_add_css_class(okbtn, "vantage-confirm-ok");
    g_signal_connect(okbtn, "clicked", G_CALLBACK(_on_confirm),
                     S.action);
    gtk_box_append(GTK_BOX(brow), okbtn);
    gtk_box_append(GTK_BOX(S.confirm_box), brow);

    _show_view(S.confirm_box);
    (void)ok_glyph;
}

/* ------------------------------------------------------------- build */

GtkWidget *session_button_new(void) {
    S.button = gtk_menu_button_new();
    gtk_widget_add_css_class(S.button, "flat");
    gtk_widget_set_tooltip_text(S.button, "Session");

    const char *user = g_get_user_name();
    char label[64];
    snprintf(label, sizeof(label), " %s ",
             user && *user ? user : "user");
    GtkWidget *lbl = gtk_label_new(label);
    GtkWidget *ic = vp_icon_image_new("system-shutdown-symbolic", 18,
                                      VP_GLYPH_POWER);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox), ic);
    gtk_box_append(GTK_BOX(hbox), lbl);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(S.button), hbox);

    /* the popover: primary action list + (swapped in) confirm view.
     * Same anchoring as the Programs menu (GtkMenuButton popover) —
     * the one popover mechanism proven against the live compositor
     * and real GTK themes. */
    S.pop = gtk_popover_new();
    gtk_widget_add_css_class(S.pop, "vantage-session-pop");
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(vbox, 6);
    gtk_widget_set_margin_bottom(vbox, 6);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);

    S.menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(S.menu_box),
                   _menu_row("system-log-out-symbolic", VP_GLYPH_POWER,
                             "Log Out", "logout"));
    gtk_box_append(GTK_BOX(S.menu_box),
                   _menu_row("system-reboot-symbolic", VP_GLYPH_POWER,
                             "Restart", "reboot"));
    gtk_box_append(GTK_BOX(S.menu_box),
                   _menu_row("system-shutdown-symbolic", VP_GLYPH_POWER,
                             "Shut Down", "shutdown"));
    gtk_box_append(GTK_BOX(vbox), S.menu_box);

    S.confirm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(S.confirm_box, 2);
    gtk_widget_set_margin_bottom(S.confirm_box, 2);
    gtk_widget_set_margin_start(S.confirm_box, 2);
    gtk_widget_set_margin_end(S.confirm_box, 2);
    gtk_widget_set_visible(S.confirm_box, FALSE);
    gtk_box_append(GTK_BOX(vbox), S.confirm_box);

    gtk_popover_set_child(GTK_POPOVER(S.pop), vbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(S.button), S.pop);
    return S.button;
}
