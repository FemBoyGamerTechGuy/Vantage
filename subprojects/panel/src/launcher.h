/*
 * launcher.h — Programs menu (app launcher)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 */
#ifndef VPANEL_LAUNCHER_H
#define VPANEL_LAUNCHER_H

#include <gtk/gtk.h>

GtkWidget *launcher_button_new(void);
void       launcher_set_model(gpointer wm);

#endif
