/*
 * vt-integ-dbus.c — Optional D-Bus integration
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * D-Bus is NOT required for Vantage to run. This module exposes a thin
 * wrapper around libdbus (if linked) so optional integrations (portal,
 * accessability, system event forwarding) can be enabled when D-Bus is
 * available.
 *
 * If libdbus is NOT compiled in, all calls return false / unavailable.
 */

#define VT_LOG_DOMAIN "integ-dbus"
#include <vantage/vt-integrations.h>

#if defined(VT_HAVE_DBUS)
#include <dbus/dbus.h>
#endif
#include <string.h>

struct vt_dbus {
    int dummy;  /* placeholder until real D-Bus dispatch loop */
};

vt_dbus_t *vt_dbus_new(void) { return vt_malloc0(sizeof(vt_dbus_t)); }
void       vt_dbus_free(vt_dbus_t *d) { vt_free(d); }

bool vt_dbus_available(void) {
#if defined(VT_HAVE_DBUS)
    DBusError err; dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    bool ok = (c != NULL);
    if (c) dbus_connection_unref(c);
    dbus_error_free(&err);
    return ok;
#else
    return false;
#endif
}

int vt_dbus_connect(vt_dbus_t *d) {
    (void)d;
    return vt_dbus_available() ? VT_OK : VT_ERR_NOTSUPP;
}
void vt_dbus_disconnect(vt_dbus_t *d) { (void)d; }
