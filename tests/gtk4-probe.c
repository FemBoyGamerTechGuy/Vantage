/*
 * gtk4-probe.c — a REAL GTK4 application used by the harnesses to test
 * the compositor against actual-toolkit behavior (the synthetic
 * vt-wayland-testclient acks every configure blindly and ignores the
 * configured size, which hides protocol bugs real apps hit).
 *
 * Modes:
 *   default        : one toplevel, 400x300 default size, CSD headerbar
 *   --ssd          : request server-side decorations (xdg-deco) so the
 *                    compositor draws its SSD frame
 *   --label TEXT   : window title
 *   --wait SECONDS : lifetime
 *
 * Prints state transitions on stdout (flushed):
 *   mapped WxH            after first map (GDK surface size)
 *   configured WxH        every toplevel configure (from the widget's
 *                         allocation after applying it)
 *   alloc WxH             widget size-allocate
 */
#include <gtk/gtk.h>
#include <string.h>

static GtkWidget *g_draw = NULL;
static char g_title[128] = "GTK Probe";
static gboolean g_ssd = FALSE;

static void _on_alloc(GtkWidget *w, gint width, gint height, gint baseline,
                      gpointer u) {
    (void)w; (void)baseline; (void)u;
    static int last_w = -1, last_h = -1;
    if (width != last_w || height != last_h) {
        last_w = width; last_h = height;
        printf("alloc %dx%d\n", width, height);
        fflush(stdout);
    }
}

static void _on_map(GtkWidget *w, gpointer u) {
    (void)u;
    printf("mapped %dx%d\n",
           gtk_widget_get_width(w), gtk_widget_get_height(w));
    fflush(stdout);
}

/* poll the window size: prints "size WxH" whenever it changes —
 * verifies that compositor configures (resize/maximize) actually
 * APPLY in a real toolkit. (A size-allocate signal connection warned
 * "signal invalid" on this GTK build; polling is robust everywhere.) */
static GtkWidget *g_win = NULL;
static gboolean _poll_size(gpointer u) {
    (void)u;
    static int lw = -1, lh = -1;
    if (g_win) {
        int w = gtk_widget_get_width(g_win);
        int h = gtk_widget_get_height(g_win);
        if (w != lw || h != lh) {
            lw = w; lh = h;
            printf("size %dx%d\n", w, h);
            fflush(stdout);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* probe content: solid magenta-ish fill with a white cross so frame
 * dumps can locate the client area precisely; label bottom-left */
static void _draw_fn(GtkDrawingArea *da, cairo_t *cr, int w, int h,
                     gpointer u) {
    (void)u;
    cairo_set_source_rgb(cr, 0.62, 0.18, 0.46);   /* #9e2e75-ish */
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 3.0);
    cairo_move_to(cr, 0, h / 2.0);
    cairo_line_to(cr, w, h / 2.0);
    cairo_move_to(cr, w / 2.0, 0);
    cairo_line_to(cr, w / 2.0, h);
    cairo_stroke(cr);
    (void)da;
}

static void _activate(GApplication *app, gpointer u) {
    (void)u;
    GtkWidget *win = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(win), g_title);
    gtk_window_set_default_size(GTK_WINDOW(win), 400, 300);
    g_signal_connect(win, "map", G_CALLBACK(_on_map), NULL);
    g_win = win;
    g_timeout_add(100, _poll_size, NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (!g_ssd) {
        GtkWidget *hdr = gtk_header_bar_new();
        gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), TRUE);
        gtk_box_append(GTK_BOX(box), hdr);
    }
    g_draw = gtk_drawing_area_new();
    gtk_widget_set_vexpand(g_draw, TRUE);
    gtk_widget_set_hexpand(g_draw, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_draw), _draw_fn,
                                   NULL, NULL);
    gtk_box_append(GTK_BOX(box), g_draw);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_widget_set_visible(win, TRUE);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ssd")) g_ssd = TRUE;
        else if (!strcmp(argv[i], "--label") && i + 1 < argc)
            snprintf(g_title, sizeof(g_title), "%s", argv[++i]);
    }
    GtkApplication *app = gtk_application_new(
        "org.vantage.gtkprobe", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(_activate), NULL);
    /* server-side decorations request: the xdg-decoration protocol */
    if (g_ssd) g_setenv("GTK_DECORATION_SERVER", "1", TRUE);
    int rc = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return rc;
}
