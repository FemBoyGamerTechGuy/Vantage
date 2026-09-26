/*
 * vt-seat.h — Session / seat management for the native Wayland backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Acquires the seat, VT and (indirectly) DRM-master rights that a real
 * compositor session needs:
 *
 *   preferred path : libseat — talks to logind *or* elogind *or* a
 *                    running seatd daemon. Whatever answers first wins;
 *                    nothing here requires systemd specifically.
 *   fallback path  : direct VT ioctls — open the VT named by
 *                    $VANTAGE_VT / $XDG_VTNR / /sys/class/tty/tty0/active,
 *                    VT_SETMODE(VT_PROCESS) + drmSetMaster(). Works with
 *                    no session manager at all when the user owns the TTY.
 *
 * $VT_SEAT_BACKEND forces the choice: logind|elogind|seatd|builtin|direct.
 *
 * The X11 backend never needs this module — Xorg/XLibre already owns the
 * seat and Vantage is just another X client there.
 */
#ifndef VANTAGE_SEAT_H
#define VANTAGE_SEAT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_seat vt_seat_t;

typedef enum {
    VT_SEAT_MODE_NONE = 0,
    VT_SEAT_MODE_LIBSEAT,     /* logind / elogind / seatd / builtin */
    VT_SEAT_MODE_DIRECT,      /* raw VT ioctls, no session manager */
} vt_seat_mode_t;

/* Notifications delivered from the seat's event source (dispatched by
 * vt_seat_dispatch(), which the owner polls via vt_seat_fd()). */
typedef enum {
    VT_SEAT_NOTIFY_DISABLE = 0,  /* VT switch away: drop DRM master now */
    VT_SEAT_NOTIFY_ENABLE,       /* VT switched back: retake master, repaint */
} vt_seat_notify_kind_t;

typedef void (*vt_seat_notify_fn)(vt_seat_t *seat,
                                  vt_seat_notify_kind_t kind, void *ud);

/* Set the notification callback (delivered from vt_seat_dispatch()). */
void vt_seat_set_notify(vt_seat_t *s, vt_seat_notify_fn fn, void *ud);

/* Try to acquire the seat. Logs the whole negotiation. Returns NULL when
 * neither libseat nor the direct path could give us a VT (e.g. started
 * from a graphical terminal with no session manager). */
vt_seat_t *vt_seat_acquire(void);
void       vt_seat_release(vt_seat_t *s);

/* which mechanism actually runs + informational accessors */
vt_seat_mode_t vt_seat_mode(const vt_seat_t *s);
const char    *vt_seat_mode_str(const vt_seat_t *s);
const char    *vt_seat_name(const vt_seat_t *s);   /* e.g. "seat0" */
int            vt_seat_vt(const vt_seat_t *s);     /* VT number or -1 */
bool           vt_seat_is_active(const vt_seat_t *s);

/* Pollable fd carrying seat/VT events (libseat socket or signalfd).
 * -1 when there is nothing to poll (should not normally happen). */
int  vt_seat_fd(const vt_seat_t *s);
/* Process pending events; delivers ENABLE/DISABLE notifications. */
void vt_seat_dispatch(vt_seat_t *s);

/* Open/close a device (e.g. /dev/dri/card0, /dev/input/event3) through
 * the seat — logind/seatd then grant the necessary permissions. */
int  vt_seat_open_device(vt_seat_t *s, const char *path);
void vt_seat_close_device(vt_seat_t *s, int fd);

/* DRM master control. With libseat the fd may already be master once the
 * session is active; both functions tolerate that (EACCES/unneeded is
 * not an error when the seat reports we already hold master). */
int  vt_seat_drm_set_master(vt_seat_t *s, int drm_fd);
int  vt_seat_drm_drop_master(vt_seat_t *s, int drm_fd);

/* Switch to our VT and wait (bounded) until it is active. */
int  vt_seat_vt_activate(vt_seat_t *s, int timeout_ms);

/* KD_GRAPHICS / KD_TEXT on our VT. Only call KD_GRAPHICS after the first
 * scanout buffer is on the CRTC — switching early leaves a black screen
 * with no text console behind it. Restoring text on close/destroy. */
int  vt_seat_vt_set_graphics(vt_seat_t *s, bool graphics);

#ifdef __cplusplus
}
#endif
#endif
