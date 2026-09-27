/*
 * clock.c — panel clock with a calendar popover
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#include "clock.h"

#include <string.h>

static GtkWidget *label;

static gboolean _tick(gpointer user) {
    (void)user;
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "%H:%M");
    gtk_label_set_text(GTK_LABEL(label), t ? t : "--:--");
    gtk_widget_set_tooltip_text(label, NULL); /* tooltip set below */
    char *full = g_date_time_format(now, "%A, %e %B %Y, %H:%M");
    if (full) gtk_widget_set_tooltip_text(label, full);
    g_free(t);
    g_free(full);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static void _on_popover_show(GtkWidget *pop, gpointer user) {
    (void)user;
    /* find the calendar inside and set today */
    GtkWidget *cal = g_object_get_data(G_OBJECT(pop), "calendar");
    if (cal && GTK_IS_CALENDAR(cal)) {
        GDateTime *now = g_date_time_new_now_local();
        gtk_calendar_select_day(GTK_CALENDAR(cal), now);
        g_date_time_unref(now);
    }
}

GtkWidget *clock_new(void) {
    label = gtk_label_new("--:--");
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);

    GtkWidget *btn = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), label);
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, "Clock and calendar");

    GtkWidget *pop = gtk_popover_new();
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);

    GDateTime *now = g_date_time_new_now_local();
    char *date = g_date_time_format(now, "%A, %e %B %Y");
    GtkWidget *dl = gtk_label_new(date ? date : "");
    g_free(date);
    gtk_widget_set_margin_bottom(dl, 4);
    gtk_box_append(GTK_BOX(vbox), dl);

    GtkWidget *cal = gtk_calendar_new();
    gtk_box_append(GTK_BOX(vbox), cal);
    g_object_set_data(G_OBJECT(pop), "calendar", cal);
    g_date_time_unref(now);

    gtk_popover_set_child(GTK_POPOVER(pop), vbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), pop);
    g_signal_connect(pop, "show", G_CALLBACK(_on_popover_show), NULL);

    _tick(NULL);
    g_timeout_add_seconds(10, _tick, NULL);
    return btn;
}
