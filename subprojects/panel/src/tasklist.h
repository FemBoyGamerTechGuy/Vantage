/*
 * tasklist.h — window buttons (taskbar)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#ifndef VPANEL_TASKLIST_H
#define VPANEL_TASKLIST_H

#include <gtk/gtk.h>

GtkWidget *tasklist_new(gpointer wm);
void       tasklist_refresh(gpointer wm);

#endif
