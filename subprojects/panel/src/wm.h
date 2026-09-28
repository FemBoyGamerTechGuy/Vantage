/*
 * wm.h — window/workspace model for the panel
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * One model, both backends: the WM IPC serves the same window list,
 * geometry and actions on X11 and on the native Wayland compositor
 * (Xwayland windows included). The panel polls + listens to events and
 * re-renders applets.
 */
#ifndef VPANEL_WM_H
#define VPANEL_WM_H

#include <glib.h>
#include "ipc.h" 

typedef struct {
    guint32 id;
    gchar  *title;
    gchar  *app_id;      /* Wayland app_id or X11 WM_CLASS class */
    gchar  *cls;         /* X11 class / "wayland" */
    gint    ws;
    gint    x, y, w, h;  /* real geometry — pager miniatures */
    gboolean focused;
    gboolean minimized;
    gboolean maximized;
    gboolean fullscreen;
    gboolean dock;       /* _NET_WM_WINDOW_TYPE_DOCK — never a task */
    gboolean desktop;    /* _NET_WM_WINDOW_TYPE_DESKTOP — never a task */
} vp_win_t;

typedef void (*vp_wm_changed_cb)(gpointer user);

typedef struct {
    GPtrArray   *wins;         /* vp_win_t*, ownership */
    gint         ws_count;
    gint         ws_cur;
    vp_wm_changed_cb on_change;
    gpointer     user;
    guint        poll_id;      /* g_timeout source */
    vp_ipc_t    *ipc;
    guint        misses;       /* consecutive dead WM calls */
    guint        have_ipc : 1;
} vp_wm_t;

vp_wm_t *vp_wm_new(vp_wm_changed_cb cb, gpointer user);
void     vp_wm_free(vp_wm_t *wm);

/* full refresh (query + events drain); TRUE when something changed */
gboolean vp_wm_refresh(vp_wm_t *wm);

/* actions */
void vp_wm_focus(vp_wm_t *wm, guint32 id);
void vp_wm_close(vp_wm_t *wm, guint32 id);
void vp_wm_minimize(vp_wm_t *wm, guint32 id);
void vp_wm_restore(vp_wm_t *wm, guint32 id);
void vp_wm_move_to_ws(vp_wm_t *wm, guint32 id, gint ws);
void vp_wm_switch_ws(vp_wm_t *wm, gint ws);

/* session actions — sent to the SESSION manager socket
 * ($XDG_RUNTIME_DIR/vantage-session.sock), which ends the whole
 * supervised session (every child, WM included) instead of just
 * stopping the compositor and letting the supervisor restart it. */
void vp_session_action(const char *action);

#endif
