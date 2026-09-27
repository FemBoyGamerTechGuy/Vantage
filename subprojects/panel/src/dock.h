/*
 * dock.h — panel docking on both backends
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#ifndef VPANEL_DOCK_H
#define VPANEL_DOCK_H

#include <gtk/gtk.h>

/* Dock modes, in order of preference:
 *   LAYER_SHELL — Wayland via wlr-layer-shell (gtk4-layer-shell):
 *                 TOP layer, top edge, full width, exclusive zone,
 *                 on-demand keyboard.
 *   X11_EWMH    — undecorated _NET_WM_WINDOW_TYPE_DOCK window with
 *                 _NET_WM_STATE_ABOVE + _NET_WM_STRUT_PARTIAL.
 *   NONE        — undockable: a Wayland compositor without
 *                 zwlr_layer_shell_v1, or a panel built without
 *                 gtk4-layer-shell. A desktop panel MUST dock —
 *                 floating in the middle of the screen is a silently
 *                 broken desktop, so the panel REFUSES to run in this
 *                 state (the same contract as waybar): dock_prepare()
 *                 returns FALSE and the caller exits non-zero with
 *                 dock_support_hint() as the diagnostic. */
typedef enum {
    VP_DOCK_LAYER_SHELL,
    VP_DOCK_X11_EWMH,
    VP_DOCK_NONE,
} vp_dock_mode_t;

/* Cheap EARLY probe (call at activate, BEFORE building any widget):
 * TRUE when the panel will be able to dock on the current display —
 * X11 always docks via EWMH; Wayland needs wlr-layer-shell. When
 * FALSE, exit now with dock_support_hint(): refusing before any
 * widget/CSS/icon-theme work means the diagnostic is the FIRST line
 * on stderr (no applet probing noise drowning it) and the early exit
 * allocates nothing of the panel's own (only GTK's init-time caches
 * remain, owned by the libraries). */
gboolean dock_can_dock(void);

/* Prepare the window for docking (call BEFORE the window is shown).
 * Returns FALSE when the panel cannot dock at all (mode NONE): do not
 * show the window — print dock_support_hint() and exit non-zero.
 * (dock_can_dock() is the early probe; this is the enforcement —
 * layer-shell support cannot change between the two, but the window
 * is only safe to show once this has actually initialized it.) */
gboolean dock_prepare(GtkWindow *win);

/* After mapping: pins position/struts (X11 only, no-op elsewhere). */
void     dock_finish(GtkWindow *win);

/* The mode dock_prepare() selected (NONE before it is called). */
vp_dock_mode_t dock_mode(void);

/* Stable, honest names for logs: "wayland/layer-shell" (docked),
 * "x11/ewmh" (docked), "undocked". The old ready line printed
 * "wayland/layer-shell" merely because the DISPLAY was Wayland — even
 * when the panel had no layer-shell support and floated, which sent
 * users (and harness failure dumps) hunting in the wrong place. */
const char *dock_mode_name(void);

/* Actionable reason why docking is impossible (mode NONE): names the
 * missing library AND the fix when the panel was built without it,
 * the missing compositor protocol when it was built with it. */
const char *dock_support_hint(void);

int      dock_height(void);
gboolean dock_is_wayland(void);

#endif
