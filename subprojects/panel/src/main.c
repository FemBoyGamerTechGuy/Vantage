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
static int g_exit_code = 0;

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
 * bar). Everything inside the bar inherits this color. */
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
".vantage-accent { color: #4f9adc; }\n"
/* The PROGRAMS MENU carries the shell's OWN identity, not the GTK
 * theme's: a desktop panel must look like itself on every machine.
 * Under a light theme the stock popover rendered as a bright sheet
 * (and a dark theme flipped it the other way), so the menu's look
 * followed the user's GTK settings instead of the desktop's brand.
 * Pinning the popover's palette, text, entry and row geometry also
 * makes the row hit boxes — and therefore launcher clicks —
 * identical on every theme. NOTE the "> contents"/"> arrow" nodes:
 * GTK4 popovers paint the visible sheet on CHILD nodes of the
 * popover node (Adwaita keeps the parent itself transparent), so
 * styling only "popover" would be covered by the theme's sheet. */
"popover.vantage-menu,\n"
"popover.vantage-menu > contents,\n"
"popover.vantage-menu > arrow {\n"
"  background-color: #1f2126;\n"
"  color: #e8eaed;\n"
"}\n"
"popover.vantage-menu label { color: #e8eaed; }\n"
"popover.vantage-menu entry { background-color: #2a2d33; "
"                             color: #e8eaed; "
"                             caret-color: #e8eaed; }\n"
/* the category/app GtkListBoxes: Adwaita paints `list` white (light) /
 * dark (dark) — the sheet must keep the shell's palette instead */
"popover.vantage-menu list { background-color: transparent; }\n"
"popover.vantage-menu list row { background-color: transparent; "
"                                min-height: 30px; "
"                                color: #e8eaed; }\n"
"popover.vantage-menu row:hover { background-color: #2e3238; }\n"
/* the session menu popover shares the same pinned sheet (its confirm
 * view — the logout confirmation — lives INSIDE this popover: a modal
 * GtkAlertDialog would be a separate xdg toplevel transient for a
 * layer-shell parent that has no xdg_toplevel, so it never maps) */
"popover.vantage-session-pop,\n"
"popover.vantage-session-pop > contents,\n"
"popover.vantage-session-pop > arrow {\n"
"  background-color: #1f2126;\n"
"  color: #e8eaed;\n"
"}\n"
"popover.vantage-session-pop label { color: #e8eaed; }\n"
"popover.vantage-session-pop button { color: #e8eaed; }\n"
/* Adwaita paints GtkButtons with a background-IMAGE (light gradient)
 * layered OVER background-color — a plain background-color rule is
 * invisible under it. The taskbar buttons dodge this with the "flat"
 * class (which the theme maps to image: none); the confirm button
 * wants a SOLID look, so clear the image layer explicitly. The
 * :hover companion keeps the primary action blue while generic
 * buttons darken (specificity (0,3,2) beats the (0,2,2) below). */
"popover.vantage-session-pop button.vantage-confirm-ok,\n"
"popover.vantage-session-pop button.vantage-confirm-ok:hover {\n"
"  background-image: none;\n"
"  background-color: #4f9adc;\n"
"  border-color: #4f9adc;\n"
"  color: #10131a; font-weight: bold;\n"
"}\n"
"popover.vantage-session-pop button:hover { "
"  background-image: none; background-color: #2e3238; }\n";

/* Popovers open INSTANTLY. GTK's stock fade-in is driven by frame
 * callbacks; on a software renderer (headless runs, slow boxes) the
 * animation can still be mid-flight long after a real user — or an
 * automated check — expects the menu to be fully there. A desktop
 * shell wants snappy menus anyway (XFCE opens its launchers with no
 * transition), so animations are off for the whole panel process. */
static void _disable_animations(void) {
    GtkSettings *s = gtk_settings_get_default();
    if (s)
        g_object_set(s, "gtk-enable-animations", FALSE, NULL);
}

static void _on_activate(GtkApplication *app, gpointer user) {
    (void)user;
    /* EARLY dockability probe — before any widget, CSS or icon-theme
     * work. A panel that cannot dock (Wayland without wlr-layer-shell,
     * or a build without gtk4-layer-shell) exits NOW: the diagnostic is
     * the first and only thing on stderr (no applet probing noise
     * drowning it — observed on real machines, where ALSA/EGL chatter
     * buried the actual fault line in every failure dump), and the
     * early exit allocates nothing of its own (the only residual at
     * process exit under LSan is fontconfig's ~320-byte config cache,
     * initialized by gtk_init and owned by the library — not panel
     * memory, and not safely freeable under a live Pango font map).
     * dock_prepare() below remains the enforcement point. */
    if (!dock_can_dock()) {
        g_printerr("vantage-panel: cannot dock: %s\n",
                   dock_support_hint());
        g_exit_code = 1;
        return;   /* no window exists: the application exits now */
    }
    /* BEFORE any widget exists: the shipped safety-net icons make
     * every icon lookup resolvable (GTK 4.18's missing-icon fallback
     * recurses without bound when image-missing is absent from the
     * active theme — a real stack-overflow crash on minimal themes) */
    vp_icon_safety_net();
    _disable_animations();

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

    /* A panel MUST dock. When the compositor offers no docking
     * protocol — a Wayland compositor without wlr-layer-shell, or a
     * build without gtk4-layer-shell — showing the window would just
     * float it in the middle of the screen: a silently broken
     * desktop whose every downstream consumer (tasklist, pager,
     * menus) fails far from the real cause. Refuse loudly instead:
     * one actionable line on stderr, non-zero exit (the waybar
     * contract). The exit travels through the normal GTK shutdown
     * path: destroying the application's only window releases the
     * GtkApplication hold and the main loop ends cleanly. */
    if (!dock_prepare(GTK_WINDOW(win))) {
        g_printerr("vantage-panel: cannot dock: %s\n",
                   dock_support_hint());
        /* clean teardown: the WM client and its IPC/poll sources are
         * released (LSan-clean early exit), the window is destroyed,
         * which releases the application hold and ends the loop */
        vp_wm_free(wm);
        wm = NULL;
        gtk_window_destroy(GTK_WINDOW(win));
        g_exit_code = 1;
        return;
    }
    gtk_window_present(GTK_WINDOW(win));
    dock_finish(GTK_WINDOW(win));

    /* periodic refresh in addition to WM events (geometry moves while
     * dragging, xwayland windows, workspace state) */
    g_timeout_add(400, _on_refresh, NULL);

    /* the backend string is the REAL dock mode (dock_mode_name), not
     * a display-type guess: the old line printed "layer-shell" even
     * while the panel floated without it, misleading failure dumps */
    g_print("vantage-panel: ready (%s backend)\n", dock_mode_name());
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "org.vantage.Panel", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(_on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    if (g_exit_code != 0)
        rc = g_exit_code;   /* refusal-to-dock is a hard error */
    g_object_unref(app);
    return rc;
}
