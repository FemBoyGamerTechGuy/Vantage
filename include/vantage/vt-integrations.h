/*
 * vt-integrations.h — Optional integrations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These are all OPTIONAL. Vantage never requires any of them. Each module
 * probes the system at runtime and gracefully degrades when unavailable.
 *
 * - D-Bus:        selected IPC, portal/accessibility/events  (optional)
 * - Audio:        PipeWire > PulseAudio > ALSA                (optional)
 * - Network:      NetworkManager > ConnMan > /proc fallback   (optional)
 * - Power:        logind/elogind > UPower > direct ioctl      (optional)
 * - Init system:  systemd/s6/openrc detected; none required  (optional)
 */
#ifndef VANTAGE_INTEGRATIONS_H
#define VANTAGE_INTEGRATIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------- D-Bus (optional) ------------------------------ */
typedef struct vt_dbus vt_dbus_t;
vt_dbus_t *vt_dbus_new(void);
void       vt_dbus_free(vt_dbus_t *d);
bool       vt_dbus_available(void);
int        vt_dbus_connect(vt_dbus_t *d);
void       vt_dbus_disconnect(vt_dbus_t *d);

/* ----------------------- Audio (optional) ----------------------------- */
typedef enum {
    VT_AUDIO_BACKEND_NONE = 0,
    VT_AUDIO_BACKEND_PIPEWIRE,
    VT_AUDIO_BACKEND_PULSE,
    VT_AUDIO_BACKEND_ALSA,
} vt_audio_backend_t;

typedef struct vt_audio vt_audio_t;
vt_audio_t        *vt_audio_new(void);
void               vt_audio_free(vt_audio_t *a);
vt_audio_backend_t vt_audio_detect(void);
const char        *vt_audio_backend_str(vt_audio_backend_t b);
int                vt_audio_init(vt_audio_t *a);
int                vt_audio_set_volume(vt_audio_t *a, int pct);
int                vt_audio_get_volume(vt_audio_t *a, int *pct);
int                vt_audio_set_mute(vt_audio_t *a, bool mute);
bool               vt_audio_get_mute(vt_audio_t *a);
const char        *vt_audio_default_sink(vt_audio_t *a);

/* ----------------------- Network (optional) --------------------------- */
typedef enum {
    VT_NET_BACKEND_NONE = 0,
    VT_NET_BACKEND_NM,
    VT_NET_BACKEND_CONNMAN,
    VT_NET_BACKEND_PROC,           /* /proc/net/dev fallback */
} vt_net_backend_t;

typedef struct vt_net vt_net_t;
vt_net_t        *vt_net_new(void);
void             vt_net_free(vt_net_t *n);
vt_net_backend_t vt_net_detect(void);
const char      *vt_net_backend_str(vt_net_backend_t b);
int              vt_net_init(vt_net_t *n);
bool             vt_net_online(vt_net_t *n);
int              vt_net_get_signal(vt_net_t *n);        /* 0..100 or -1 */
const char      *vt_net_get_ssid(vt_net_t *n);
const char      *vt_net_get_iface(vt_net_t *n);

/* ----------------------- Power (optional) ----------------------------- */
typedef enum {
    VT_POWER_NONE = 0,
    VT_POWER_LOGIND,        /* systemd OR elogind */
    VT_POWER_UPOWER,
    VT_POWER_IOCTL_ACPI,
} vt_power_backend_t;

typedef struct vt_power vt_power_t;
vt_power_t        *vt_power_new(void);
void               vt_power_free(vt_power_t *p);
vt_power_backend_t vt_power_detect(void);
const char        *vt_power_backend_str(vt_power_backend_t b);
int                vt_power_init(vt_power_t *p);
bool               vt_power_can_suspend(vt_power_t *p);
bool               vt_power_can_hibernate(vt_power_t *p);
int                vt_power_suspend(vt_power_t *p);
int                vt_power_hibernate(vt_power_t *p);
int                vt_power_shutdown(vt_power_t *p);
int                vt_power_reboot(vt_power_t *p);
int                vt_power_logout(vt_power_t *p);

/* Battery level via power supply class */
int                vt_power_battery_pct(vt_power_t *p);
bool               vt_power_on_ac(vt_power_t *p);

/* ----------------------- Init system (informational) ----------------- */
typedef enum {
    VT_INIT_UNKNOWN = 0,
    VT_INIT_SYSTEMD,
    VT_INIT_OPENRC,
    VT_INIT_S6,
    VT_INIT_RUNIT,
    VT_INIT_SYSV,
    VT_INIT_MBUS,
    VT_INIT_BUSYBOX,
} vt_init_kind_t;

vt_init_kind_t vt_init_detect(void);
const char    *vt_init_str(vt_init_kind_t k);

#ifdef __cplusplus
}
#endif
#endif
