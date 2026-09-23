/*
 * vt-session.h — Vantage session manager
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owns the DE lifecycle: startup, autostart, environment setup, logout,
 * restart, suspend hooks, watchdog.
 */
#ifndef VANTAGE_SESSION_H
#define VANTAGE_SESSION_H

#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_SESSION_STAGE_INIT = 0,
    VT_SESSION_STAGE_EARLY,
    VT_SESSION_STAGE_COMPONENTS,
    VT_SESSION_STAGE_READY,
    VT_SESSION_STAGE_SHUTDOWN,
} vt_session_stage_t;

typedef enum {
    VT_SESSION_END_LOGOUT   = 0,
    VT_SESSION_END_RESTART  = 1,
    VT_SESSION_END_SHUTDOWN = 2,
    VT_SESSION_END_SUSPEND  = 3,
    VT_SESSION_END_HIBERNATE= 4,
    VT_SESSION_END_REBOOT   = 5,
} vt_session_end_t;

typedef struct vt_session vt_session_t;

typedef void (*vt_session_hook_t)(vt_session_t *s, vt_session_stage_t stage,
                                   void *ud);

vt_session_t *vt_session_new(void);
void          vt_session_free(vt_session_t *s);
int           vt_session_start(vt_session_t *s);
int           vt_session_run(vt_session_t *s);   /* returns when ended */
void          vt_session_end(vt_session_t *s, vt_session_end_t how);

void          vt_session_add_hook(vt_session_t *s, vt_session_hook_t fn, void *ud);

/* Autostart */
int  vt_session_autostart_load(vt_session_t *s);
int  vt_session_autostart_run(vt_session_t *s);

/* Power hooks */
typedef bool (*vt_session_power_cb_t)(vt_session_t *s, vt_session_end_t op, void *ud);
void vt_session_set_power_handler(vt_session_t *s, vt_session_power_cb_t cb, void *ud);

/* Environment setup */
void vt_session_set_env(vt_session_t *s, const char *k, const char *v);
const char *vt_session_get_env(vt_session_t *s, const char *k);

vt_session_stage_t vt_session_stage(const vt_session_t *s);

#ifdef __cplusplus
}
#endif
#endif
