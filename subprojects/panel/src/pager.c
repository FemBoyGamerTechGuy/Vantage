/*
 * pager.c — the workspace switcher
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Each cell is a live MINIATURE of that desktop: every non-minimized
 * window is drawn at its TRUE relative position and size — the pager
 * shows WHERE windows are, not what number the desktop has (the user
 * asked for no numbers; the miniature is the identity). Click a cell to
 * switch, wheel over the pager cycles desktops.
 */
#include "pager.h"
#include "wm.h"

#include <math.h>
#include <string.h>

typedef struct {
    GtkWidget  *box;
    gpointer    wm;
    GPtrArray  *cells;       /* GtkWidget* (GtkDrawingArea) */
} _pager_t;

static _pager_t P = {0};

#define CELL_W 64
#define CELL_H 34

typedef struct {
    int ws;
} _cell_data_t;

static void _draw_cell(GtkDrawingArea *area, cairo_t *cr, int w, int h,
                       gpointer d) {
    (void)area;
    _cell_data_t *cd = d;
    int ws = cd ? cd->ws : 0;
    vp_wm_t *wm = P.wm;

    /* cell background: current desktop gets the accent */
    bool current = wm && ws == wm->ws_cur;
    if (current)
        cairo_set_source_rgba(cr, 0.28, 0.46, 0.78, 1.0);
    else
        cairo_set_source_rgba(cr, 0.18, 0.19, 0.22, 1.0);
    cairo_paint(cr);

    /* window miniatures at their real relative geometry */
    if (wm) {
        /* the pager needs the DESKTOP extent, not the window extent:
         * windows are placed in the output's workarea. Docks and the
         * desktop window itself are SHELL chrome, not content — a
         * full-screen desktop window used to fill the whole cell
         * ("the first workspace shows as entirely used") */
        int desk_w = 1024, desk_h = 768;
        for (guint i = 0; i < wm->wins->len; i++) {
            vp_win_t *win = g_ptr_array_index(wm->wins, i);
            if (win->dock || win->desktop) continue;
            if (win->ws != ws || win->minimized) continue;
            if (win->w > desk_w) desk_w = win->w;
            if (win->h > desk_h) desk_h = win->h;
        }
        for (guint i = 0; i < wm->wins->len; i++) {
            vp_win_t *win = g_ptr_array_index(wm->wins, i);
            if (win->dock || win->desktop) continue;
            if (win->ws != ws || win->minimized) continue;
            double rx = (double)(win->x < 0 ? 0 : win->x) / desk_w;
            double ry = (double)(win->y < 0 ? 0 : win->y) / desk_h;
            double rw = (double)(win->w > 0 ? win->w : 1) / desk_w;
            double rh = (double)(win->h > 0 ? win->h : 1) / desk_h;
            if (rw > 1) rw = 1;
            if (rh > 1) rh = 1;
            double x = 2 + rx * (w - 4);
            double y = 2 + ry * (h - 4);
            double cw = rw * (w - 4);
            double ch = rh * (h - 4);
            if (cw < 3) cw = 3;
            if (ch < 3) ch = 3;
            if (win->focused && current)
                cairo_set_source_rgba(cr, 0.95, 0.96, 0.98, 1.0);
            else
                cairo_set_source_rgba(cr, 0.62, 0.64, 0.68, 1.0);
            cairo_rectangle(cr, x, y, cw, ch);
            cairo_fill(cr);
        }
    }
}

static void _on_cell_click(GtkGestureClick *g, int n, double x, double y,
                           gpointer d) {
    (void)g; (void)n; (void)x; (void)y;
    _cell_data_t *cd = d;
    if (cd && P.wm)
        vp_wm_switch_ws(P.wm, cd->ws);
}

static void _on_wheel(GtkEventControllerScroll *s, double dx, double dy,
                      gpointer user) {
    (void)s; (void)dx; (void)user;
    vp_wm_t *wm = P.wm;
    if (!wm || wm->ws_count < 1) return;
    int next = wm->ws_cur + (dy > 0 ? 1 : -1);
    if (next < 0) next = wm->ws_count - 1;
    if (next >= wm->ws_count) next = 0;
    vp_wm_switch_ws(wm, next);
}

void pager_refresh(gpointer wm) {
    P.wm = wm;
    vp_wm_t *w = wm;
    if (!w || !P.box) return;

    /* rebuild cells when the count changes */
    guint want = (guint)(w->ws_count > 0 ? w->ws_count : 1);
    if (P.cells->len != want) {
        GtkWidget *child = gtk_widget_get_first_child(P.box);
        while (child) {
            GtkWidget *next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(P.box), child);
            child = next;
        }
        g_ptr_array_set_size(P.cells, 0);
        for (guint i = 0; i < want; i++) {
            _cell_data_t *cd = g_new0(_cell_data_t, 1);
            cd->ws = (int)i;
            GtkWidget *cell = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(cell),
                                               CELL_W);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(cell),
                                                CELL_H);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(cell),
                                           _draw_cell, cd, g_free);
            gtk_widget_set_valign(cell, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_end(cell, 3);
            gtk_widget_set_margin_start(cell, 3);

            GtkGesture *click = gtk_gesture_click_new();
            g_signal_connect(click, "released",
                             G_CALLBACK(_on_cell_click), cd);
            gtk_widget_add_controller(cell,
                                      GTK_EVENT_CONTROLLER(click));

            gtk_box_append(GTK_BOX(P.box), cell);
            g_ptr_array_add(P.cells, cell);
        }
    }
    /* Invalidate the CELLS, not just their parent box: GTK caches
     * render nodes per widget, and gtk_widget_queue_draw() only
     * invalidates the widget it is called on. Queueing the box left
     * every cell frozen at its FIRST snapshot — the miniatures and
     * even the current-workspace highlight never updated (verified:
     * the accent stayed on cell 0 after switching desktops while the
     * WM model was correct all along). Each drawing area owns what
     * it renders from the model, so each one must be re-snapshotted. */
    for (guint i = 0; i < P.cells->len; i++)
        gtk_widget_queue_draw(GTK_WIDGET(g_ptr_array_index(P.cells, i)));
    gtk_widget_queue_draw(P.box);
}

GtkWidget *pager_new(gpointer wm) {
    P.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(P.box, GTK_ALIGN_CENTER);
    P.cells = g_ptr_array_new();
    pager_refresh(wm);

    GtkEventController *wheel = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(wheel, "scroll", G_CALLBACK(_on_wheel), NULL);
    gtk_widget_add_controller(P.box, wheel);

    return P.box;
}
