/*
 * vt-x11.h — Shared X11 glue used by backend, WM, compositor and UI
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Internal header (not installed). All X11-flavored components share one
 * Display connection owned by the backend. Any conforming X11 server
 * works — Xorg, XLibre, Xvfb, or others; the running implementation is
 * queried for diagnostics via vt_x11_server_name().
 */
#ifndef VANTAGE_X11_H
#define VANTAGE_X11_H

#if defined(VT_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the process-wide X11 connection (NULL if not initialized). */
Display *vt_x11_display(void);
/* Informational: normalized name of the running X server implementation
 * ("Xorg", "XLibre", Xvfb's vendor, or the raw vendor string).
 * Never selects code paths — the X11 API is the same for all of them. */
const char *vt_x11_server_name(void);
Window   vt_x11_root(void);
int      vt_x11_screen(void);

/* EWMH atom cache, initialized once per process. */
typedef struct {
    Atom net_supported;
    Atom net_client_list;
    Atom net_client_list_stacking;
    Atom net_active_window;
    Atom net_current_desktop;
    Atom net_number_of_desktops;
    Atom net_desktop_names;
    Atom net_desktop_viewport;
    Atom net_workarea;
    Atom net_wm_name;
    Atom net_wm_icon_name;
    Atom net_wm_visible_name;
    Atom net_wm_icon;
    Atom net_wm_state;
    Atom net_wm_state_modal;
    Atom net_wm_state_sticky;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_maximized_horz;
    Atom net_wm_state_fullscreen;
    Atom net_wm_state_hidden;
    Atom net_wm_state_above;
    Atom net_wm_state_below;
    Atom net_wm_state_demands_attention;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_state_skip_pager;
    Atom net_wm_window_type;
    Atom net_wm_window_type_normal;
    Atom net_wm_window_type_dock;
    Atom net_wm_window_type_desktop;
    Atom net_wm_window_type_toolbar;
    Atom net_wm_window_type_menu;
    Atom net_wm_window_type_splash;
    Atom net_wm_window_type_dialog;
    Atom net_wm_window_type_utility;
    Atom net_wm_window_type_notification;
    Atom net_wm_allowed_actions;
    Atom net_wm_action_move;
    Atom net_wm_action_resize;
    Atom net_wm_action_minimize;
    Atom net_wm_action_maximize_horz;
    Atom net_wm_action_maximize_vert;
    Atom net_wm_action_fullscreen;
    Atom net_wm_action_close;
    Atom net_wm_action_above;
    Atom net_wm_action_below;
    Atom net_wm_desktop;
    Atom net_wm_pid;
    Atom net_wm_window_opacity;
    Atom net_wm_cm;
    Atom net_close_window;
    Atom net_wm_moveresize;
    Atom net_restack_window;
    Atom net_request_frame_extents;
    Atom net_frame_extents;
    Atom net_system_tray;
    Atom net_supporting_wm_check;
    Atom net_fullscreen_monitors;
    Atom wm_protocols;
    Atom wm_delete_window;
    Atom wm_take_focus;
    Atom wm_state;
    Atom wm_change_state;
    Atom wm_client_leader;
    Atom utf8_string;
    Atom cardinal;
    Atom atom_atom;
    Atom string;
    Atom pixmap_atom;
    Atom wm_class_atom;
} vt_x11_atoms_t;

const vt_x11_atoms_t *vt_x11_atoms(void);

/* UTF-8 property helpers */
char *vt_x11_get_utf8_property(Window w, Atom prop);
bool  vt_x11_set_utf8_property(Window w, Atom prop, const char *val);
/* CARDINAL helpers */
bool  vt_x11_get_cardinal_property(Window w, Atom prop, unsigned long *out);
bool  vt_x11_get_window_property(Window w, Atom prop, Atom type,
                                 unsigned char **out_data,
                                 unsigned long *out_nitems);
/* _NET_WM_STATE membership check */
bool  vt_x11_has_state(Window w, Atom state);
/* Motif / functional: read _MOTIF_WM_HINTS decorations flag */
bool  vt_x11_motif_decorations_off(Window w);

/* EWMH root property writers used by the WM */
void vt_x11_set_net_supported(Window root, const Atom *list, size_t n);
void vt_x11_set_net_wm_check(Window root, Window wmwin);

#ifdef __cplusplus
}
#endif
#endif /* VT_HAVE_X11 */
#endif
