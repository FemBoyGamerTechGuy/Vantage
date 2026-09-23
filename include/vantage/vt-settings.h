/*
 * vt-settings.h — Vantage settings
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A modular settings system. Each settings module owns a small part of
 * the configuration and provides a small programmatic API. A separate
 * `vantage-settings` binary exposes a simple text/JSON interface so it
 * can be driven by any frontend (e.g. a future Qt6 or GTK GUI).
 */
#ifndef VANTAGE_SETTINGS_H
#define VANTAGE_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_SETTINGS_APPEARANCE = 0,
    VT_SETTINGS_DISPLAYS,
    VT_SETTINGS_KEYBOARD,
    VT_SETTINGS_MOUSE,
    VT_SETTINGS_SHORTCUTS,
    VT_SETTINGS_WINDOWS,
    VT_SETTINGS_WORKSPACES,
    VT_SETTINGS_WALLPAPER,
    VT_SETTINGS_COMPOSITOR,
    VT_SETTINGS_POWER,
    VT_SETTINGS_STARTUP,
    VT_SETTINGS_INPUT,
    VT_SETTINGS_BACKEND,
    VT_SETTINGS_NUM,
} vt_settings_module_t;

typedef struct vt_settings vt_settings_t;

typedef struct vt_settings_ops {
    const char *name;
    int  (*load)(vt_settings_t *s);
    int  (*save)(vt_settings_t *s);
    int  (*apply)(vt_settings_t *s);
    int  (*reset)(vt_settings_t *s);
    void (*dump)(vt_settings_t *s, FILE *fp);
} vt_settings_ops_t;

struct vt_settings {
    vt_config_t       *cfg;
    const vt_settings_ops_t **ops;   /* array of ops, indexed by module */
    void              *state[VT_SETTINGS_NUM];
};

vt_settings_t *vt_settings_new(void);
void           vt_settings_free(vt_settings_t *s);
int            vt_settings_load_all(vt_settings_t *s);
int            vt_settings_save_all(vt_settings_t *s);
int            vt_settings_apply_all(vt_settings_t *s);
int            vt_settings_apply(vt_settings_t *s, vt_settings_module_t m);
int            vt_settings_reset(vt_settings_t *s, vt_settings_module_t m);
void           vt_settings_dump(vt_settings_t *s, vt_settings_module_t m, FILE *fp);
void           vt_settings_dump_all(vt_settings_t *s, FILE *fp);

/* Module ops accessors (per-module.c) */
const vt_settings_ops_t *vt_settings_ops_appearance(void);
const vt_settings_ops_t *vt_settings_ops_displays(void);
const vt_settings_ops_t *vt_settings_ops_keyboard(void);
const vt_settings_ops_t *vt_settings_ops_windows(void);
const vt_settings_ops_t *vt_settings_ops_wallpaper(void);
const vt_settings_ops_t *vt_settings_ops_compositor(void);
const vt_settings_ops_t *vt_settings_ops_power(void);
const vt_settings_ops_t *vt_settings_ops_startup(void);

#ifdef __cplusplus
}
#endif
#endif
