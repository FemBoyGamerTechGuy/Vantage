/*
 * dock.c — panel docking: layer-shell (Wayland) / EWMH dock (X11)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * The SAME panel binary docks on both compositors:
 *
 *   Wayland: gtk4-layer-shell puts the window in the TOP layer,
 *            anchored to the top edge across the full width, with an
 *            exclusive zone so maximized/fullscreen windows leave room
 *            for the panel. Keyboard mode ON_DEMAND: the search bar
 *            gets keys when the user clicks into the panel.
 *
 *   X11:     an undecorated _NET_WM_WINDOW_TYPE_DOCK window with
 *            _NET_WM_STATE_ABOVE and _NET_WM_STRUT_PARTIAL — the
 *            EWMH way every real panel docks, honored by the Vantage
 *            X11 WM and any other conforming WM.
 */
#include "dock.h"

#include <string.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#ifdef HAVE_LAYER_SHELL
#include <gtk4-layer-shell.h>
#endif

#if defined(GDK_WINDOWING_X11) && defined(HAVE_X11_DOCK)
#define _X11_DOCK 1
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#elif defined(GDK_WINDOWING_X11)
#define _X11_DOCK 0
#endif

#define DOCK_H 40

int dock_height(void) {
    return DOCK_H;
}

gboolean dock_is_wayland(void) {
#ifdef GDK_WINDOWING_WAYLAND
    return gdk_display_get_default() != NULL &&
           GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
#else
    return FALSE;
#endif
}

#if _X11_DOCK

static Atom _a(Display *d, const char *name) {
    return XInternAtom(d, name, False);
}

/* GTK 4.18 deprecated the raw-Xlib accessors with NO functional
 * replacement: setting EWMH atoms (_NET_WM_WINDOW_TYPE_DOCK, struts)
 * has no GTK API, and the functions remain fully supported — GTK's
 * own GDK_SURFACE_XID/GDK_DISPLAY_XDISPLAY macros call them. Scoped
 * opt-out for exactly these accessors; every other line of the panel
 * stays fully warning-checked. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static Display *_xdisplay(void) {
    return gdk_x11_display_get_xdisplay(gdk_display_get_default());
}
static Window _xid(GtkWindow *win) {
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(win));
    return surf ? gdk_x11_surface_get_xid(surf) : 0;
}
#pragma GCC diagnostic pop

static int _monitor_width(void) {
    GdkDisplay *disp = gdk_display_get_default();
    GListModel *mons = disp ? gdk_display_get_monitors(disp) : NULL;
    if (mons && g_list_model_get_n_items(mons) > 0) {
        GdkMonitor *mon = g_list_model_get_item(mons, 0);
        if (mon) {
            GdkRectangle geom;
            gdk_monitor_get_geometry(mon, &geom);
            g_object_unref(mon);
            if (geom.width > 0) return geom.width;
        }
    }
    return 1024;
}

static void _x11_set_strut(GtkWindow *win, int h);

/* EWMH dock properties, applied while the window is realized but not
 * yet mapped, so the WM manages it as a dock from the first moment */
static void _x11_dock_props(GtkWidget *w) {
    GtkWindow *win = GTK_WINDOW(w);
    Display *d = _xdisplay();
    Window xid = _xid(win);
    if (!d || !xid) return;

    Atom dock = _a(d, "_NET_WM_WINDOW_TYPE_DOCK");
    XChangeProperty(d, xid, _a(d, "_NET_WM_WINDOW_TYPE"), XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&dock, 1);

    Atom states[2];
    states[0] = _a(d, "_NET_WM_STATE_ABOVE");
    states[1] = _a(d, "_NET_WM_STATE_SKIP_TASKBAR");
    XChangeProperty(d, xid, _a(d, "_NET_WM_STATE"), XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)states, 2);

    XStoreName(d, xid, "Vantage Panel");
    XClassHint ch;
    ch.res_name = (char *)"vantage-panel";
    ch.res_class = (char *)"Vantage-panel";
    XSetClassHint(d, xid, &ch);
    XFlush(d);
    _x11_set_strut(win, gtk_widget_get_height(w));
}

/* the strut must reserve EXACTLY what the panel covers: the natural
 * height of the bar (CSS margins can lift it above DOCK_H) — the
 * workarea and maximized windows would otherwise overlap the panel */
static void _x11_set_strut(GtkWindow *win, int h) {
    Display *d = _xdisplay();
    Window xid = _xid(win);
    if (!d || !xid) return;
    if (h <= 0) h = DOCK_H;
    unsigned long strut[12] = {0};
    strut[2] = (unsigned long)h;               /* top reserved */
    strut[9] = (unsigned long)(_monitor_width() - 1);
    XChangeProperty(d, xid, _a(d, "_NET_WM_STRUT_PARTIAL"), XA_CARDINAL,
                    32, PropModeReplace,
                    (const unsigned char *)strut, 12);
    unsigned long strut4[4] = { 0, 0, strut[2], 0 };
    XChangeProperty(d, xid, _a(d, "_NET_WM_STRUT"), XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)strut4, 4);
    XFlush(d);
}

static void _x11_pin_position(GtkWindow *win) {
    Display *d = _xdisplay();
    Window xid = _xid(win);
    if (!d || !xid) return;
    XMoveWindow(d, xid, 0, 0);
    XFlush(d);
}

/* keep the strut + position current when the monitor set changes
 * (GTK4 exposes monitors as a list model) */
static void _x11_on_monitors_changed(GListModel *model, guint pos,
                                     guint removed, guint added,
                                     gpointer user) {
    (void)model; (void)pos; (void)removed; (void)added;
    GtkWindow *win = user;
    if (!win) return;
    _x11_set_strut(win, gtk_widget_get_height(GTK_WIDGET(win)));
    _x11_pin_position(win);
}

/* the bar's natural height can change (font metrics, applets) — keep
 * the reserved strut in sync with what is actually on screen. Only
 * height CHANGES rewrite the property: no PropertyNotify storm in
 * the WM, no workarea churn. (There is no "size-allocate" signal in
 * GTK4 — the toplevel allocation is watched with a cheap timeout.) */
static gboolean _x11_watch_height(gpointer user) {
    GtkWidget *w = user;
    static int last_h = -1;
    int h = gtk_widget_get_height(w);
    if (h > 0 && h != last_h) {
        last_h = h;
        _x11_set_strut(GTK_WINDOW(w), h);
    }
    return G_SOURCE_CONTINUE;
}
#endif

#if !_X11_DOCK
static void _x11_dock_props(GtkWidget *w) { (void)w; }
static void _x11_pin_position(GtkWindow *win) { (void)win; }
static void _x11_on_monitor_changed(GdkDisplay *d, GdkMonitor *m, gpointer u)
{ (void)d; (void)m; (void)u; }
#endif

void dock_prepare(GtkWindow *win) {
    gtk_window_set_decorated(win, FALSE);
    gboolean wl = dock_is_wayland();
    if (!wl) {
#ifdef GDK_WINDOWING_X11
        int sw = _monitor_width();
        gtk_window_set_default_size(win, sw, DOCK_H);
        gtk_window_set_resizable(win, FALSE);
#endif
    } else {
        gtk_window_set_default_size(win, 0, DOCK_H);
#if defined(GDK_WINDOWING_WAYLAND) && defined(HAVE_LAYER_SHELL)
        if (gtk_layer_is_supported()) {
            gtk_layer_init_for_window(win);
            gtk_layer_set_namespace(win, "vantage-panel");
            gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_TOP);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            gtk_layer_auto_exclusive_zone_enable(win);
            gtk_layer_set_keyboard_mode(
                win, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
            return;
        }
#endif
    }
#if _X11_DOCK
    if (!wl) {
        /* REALIZE is the last moment before the X server sees the map
         * request: the dock/type/strut atoms must be on the window by
         * then, or the WM frames the panel, cascade-places it (the
         * +40/+68 y-drift) and fights it for its position */
        g_signal_connect(win, "realize", G_CALLBACK(_x11_dock_props), NULL);
        g_timeout_add(1000, _x11_watch_height, win);
        GdkDisplay *disp = gdk_display_get_default();
        if (disp) {
            GListModel *mons = gdk_display_get_monitors(disp);
            if (mons)
                g_signal_connect(mons, "items-changed",
                                 G_CALLBACK(_x11_on_monitors_changed), win);
        }
    }
#endif
}

void dock_finish(GtkWindow *win) {
    if (!dock_is_wayland())
        _x11_pin_position(win);
}
