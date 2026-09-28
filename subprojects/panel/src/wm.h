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

/* geometry-only callback: fires on window-geometry events (drag/
 * resize live updates) WITHOUT a full model re-query — the pager
 * redraws from the in-place update at the event rate (~30 fps). */
typedef void (*vp_wm_geometry_cb)(gpointer user);

typedef struct {
    GPtrArray   *wins;         /* vp_win_t*, ownership */
    gint         ws_count;
    gint         ws_cur;
    vp_wm_changed_cb on_change;
    gpointer     user;
    vp_wm_geometry_cb on_geometry;
    gpointer     geo_user;
    guint        poll_id;      /* g_timeout source */
    guint        coalesce_id;  /* one-shot structural refresh */
    gpointer     watch_id;     /* GSource watching the WM event socket */
    vp_ipc_t    *ipc;
    guint        misses;       /* consecutive dead WM calls */
    guint        have_ipc : 1;
    guint        watch_attached : 1;
    guint        xwl_confirmed : 1;
} vp_wm_t;

vp_wm_t *vp_wm_new(vp_wm_changed_cb cb, gpointer user);
void     vp_wm_free(vp_wm_t *wm);

/* full refresh (query + events drain); TRUE when something changed */
gboolean vp_wm_refresh(vp_wm_t *wm);

/* live geometry updates without a round-trip: the WM's geometry
 * events carry x/y/w/h. Installs the callback for them. */
void vp_wm_set_geometry_cb(vp_wm_t *wm, vp_wm_geometry_cb cb, gpointer user);

/* XWayland support: ask the WM for the Xwayland DISPLAY/XAUTHORITY
 * and export them into this process, so apps launched by the panel
 * (GAppInfo children inherit our env) can open X11 windows. Idempotent;
 * retries until Xwayland is up. TRUE when a display is available. */
gboolean vp_wm_xwl_env_apply(vp_wm_t *wm);

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
