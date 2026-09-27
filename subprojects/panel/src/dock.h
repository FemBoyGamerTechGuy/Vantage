/*
 * dock.h — panel docking on both backends
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#ifndef VPANEL_DOCK_H
#define VPANEL_DOCK_H

#include <gtk/gtk.h>

/* Prepare the window for docking (call BEFORE the window is shown):
 *   Wayland → wlr-layer-shell (top edge, exclusive zone, on-demand
 *             keyboard — the gtk4-layer-shell path every independent
 *             panel uses)
 *   X11      → undecorated dock window: _NET_WM_WINDOW_TYPE_DOCK +
 *             _NET_WM_STATE_ABOVE so the WM frames nothing and keeps
 *             the panel above normal windows
 * After mapping: dock_finish() pins position/struts (X11). */
void     dock_prepare(GtkWindow *win);
void     dock_finish(GtkWindow *win);

int      dock_height(void);
gboolean dock_is_wayland(void);

#endif
