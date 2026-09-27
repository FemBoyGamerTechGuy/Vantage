/*
 * main.c — vantage-panel: the GTK4 desktop panel
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * ONE panel binary for both Vantage compositors (X11 and the native
 * Wayland compositor) and, thanks to standard EWMH/layer-shell
 * docking, usable under other compositors too. Independent subproject:
 * own build, own license, no compositor code — the only coupling is
 * the documented WM IPC protocol.
 *
 * Layout:
 *   [ Programs ] [ pager (live miniatures) ] [ tasklist ...... ]
 *   [ net ] [ vol ] [ clock ] [ user/session ]
 */
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>

#include "dock.h"
#include "wm.h"
#include "launcher.h"
#include "tasklist.h"
#include "pager.h"
#include "clock.h"
#include "volume.h"
#include "network.h"
#include "session.h"
#include "picon.h"

static GtkWidget *win;
static vp_wm_t *wm;

static gboolean _on_refresh(gpointer user) {
    (void)user;
    tasklist_refresh(wm);
    pager_refresh(wm);
    return G_SOURCE_CONTINUE;
}

static void _on_wm_changed(gpointer user) {
    (void)user;
    tasklist_refresh(wm);
    pager_refresh(wm);
}

static const char _css[] =
/* the panel WINDOW: dark to the very edges — without this the GTK
 * default light background showed as bright strips in the layout
 * margins above/below the bar (measured #f6f5f4 rows around a
 * near-black bar). The window also carries the bottom border at the
 * panel's true lower edge. */
"window.vantage-panel-root { background-color: #16181c; "
"                 border-bottom: 1px solid #3a3e46; }\n"
/* the bar itself — with its LIGHT foreground: the default theme's
 * label color is near-black, which rendered the tasklist/clock/net
 * text as dark-on-dark (measured: 0 readable text pixels on the
 * bar). Everything inside the bar inherits this color; the Programs
 * menu is a separate popover surface and keeps the theme's own
 * (light) styling. */
".vantage-panel { background-color: alpha(#16181c, 0.96); "
"                 color: #e8eaed; }\n"
/* taskbar buttons */
".vantage-task { padding: 2px 8px; border-radius: 4px; "
"                color: #dfe3e8; }\n"
".vantage-task:hover { background-color: alpha(#ffffff, 0.09); }\n"
".vantage-task-focused { background-color: alpha(#4f9adc, 0.30); }\n"
".vantage-task-min { opacity: 0.62; }\n"
/* start button */
".vantage-start { padding: 2px 10px; border-radius: 4px; "
"                 color: #dfe3e8; }\n"
".vantage-start:hover { background-color: alpha(#ffffff, 0.09); }\n"
/* a touch of brand color on the bar edge */
".vantage-accent { color: #4f9adc; }\n";

static void _on_activate(GtkApplication *app, gpointer user) {
    (void)user;
    /* BEFORE any widget exists: the shipped safety-net icons make
     * every icon lookup resolvable (GTK 4.18's missing-icon fallback
     * recurses without bound when image-missing is absent from the
     * active theme — a real stack-overflow crash on minimal themes) */
    vp_icon_safety_net();

    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Vantage Panel");
    gtk_widget_set_name(win, "vantage-panel");
    gtk_widget_add_css_class(win, "vantage-panel-root");

    {
        int css_stage = getenv("VANTAGE_PANEL_STAGE")
                            ? atoi(getenv("VANTAGE_PANEL_STAGE")) : 99;
        if (css_stage >= 1) {
            GtkCssProvider *css = gtk_css_provider_new();
            gtk_css_provider_load_from_string(css, _css);
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
        }
    }

    /* the bar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(bar, 3);
    gtk_widget_set_margin_bottom(bar, 3);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_add_css_class(bar, "vantage-panel");

    int stage = getenv("VANTAGE_PANEL_STAGE")
                    ? atoi(getenv("VANTAGE_PANEL_STAGE")) : 99;
    wm = vp_wm_new(_on_wm_changed, NULL);
    if (stage >= 2)
        gtk_box_append(GTK_BOX(bar), launcher_button_new());
    if (stage >= 3)
        gtk_box_append(GTK_BOX(bar), pager_new(wm));
    if (stage >= 4)
        gtk_box_append(GTK_BOX(bar), tasklist_new(wm));
    if (stage >= 5)
        gtk_box_append(GTK_BOX(bar), network_new());
    if (stage >= 6)
        gtk_box_append(GTK_BOX(bar), volume_new());
    if (stage >= 7)
        gtk_box_append(GTK_BOX(bar), clock_new());
    if (stage >= 8)
        gtk_box_append(GTK_BOX(bar), session_button_new());
    if (stage < 2)
        gtk_box_append(GTK_BOX(bar), gtk_label_new("MINIMAL PANEL"));

    gtk_window_set_child(GTK_WINDOW(win), bar);

    dock_prepare(GTK_WINDOW(win));
    gtk_window_present(GTK_WINDOW(win));
    dock_finish(GTK_WINDOW(win));

    /* periodic refresh in addition to WM events (geometry moves while
     * dragging, xwayland windows, workspace state) */
    g_timeout_add(400, _on_refresh, NULL);

    g_print("vantage-panel: ready (%s backend)\n",
            dock_is_wayland() ? "wayland/layer-shell" : "x11/ewmh");
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "org.vantage.Panel", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(_on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
