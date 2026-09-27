/*
 * pager.h — workspace switcher with live window miniatures
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#ifndef VPANEL_PAGER_H
#define VPANEL_PAGER_H

#include <gtk/gtk.h>

GtkWidget *pager_new(gpointer wm);
void       pager_refresh(gpointer wm);

#endif
