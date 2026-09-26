/*
 * vt-session.h — Vantage session manager
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
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
struct vt_session;  /* private: hooks, autostarts, env, managed children */

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
bool vt_session_is_running(const vt_session_t *s);

/* ------------------------------------------------------------- supervisor */
/* Spawn a named managed child. `critical` children (the WM) restart with
 * exponential backoff and take the session down after too many failures;
 * non-critical children (panel, desktop) restart with fixed delay. */
int  vt_session_spawn_managed(vt_session_t *s, const char *name,
                              const char *cmd, bool critical);
/* Reap dead children and apply restart policy. Call from the main loop. */
void vt_session_supervise(vt_session_t *s);
/* Send SIGTERM to all managed children (graceful stop). */
void vt_session_term_children(vt_session_t *s);
/* Number of live managed children. */
size_t vt_session_children_alive(const vt_session_t *s);
/* The end action requested (LOGOUT etc.) — valid during SHUTDOWN. */
int  vt_session_stop_children(vt_session_t *s, int sig);

#ifdef __cplusplus
}
#endif
#endif
