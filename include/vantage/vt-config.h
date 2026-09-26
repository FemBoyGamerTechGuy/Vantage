/*
 * vt-config.h — Vantage configuration system
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Vantage uses a simple INI-like keyfile format (sections, keys, values).
 * Configuration files live under ~/.config/vantage/X.conf and can be
 * edited by hand. Live reload is supported via vt_config_watch().
 */
#ifndef VANTAGE_CONFIG_H
#define VANTAGE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vt_config   vt_config_t;
typedef struct vt_cfg_key   vt_cfg_key_t;
typedef struct vt_cfg_sec   vt_cfg_sec_t;

struct vt_cfg_key {
    char *key;
    char *val;
};

struct vt_cfg_sec {
    char *name;
    vt_vec_t keys;        /* vt_cfg_key_t */
};

struct vt_config {
    char     *path;
    vt_vec_t  sections;  /* vt_cfg_sec_t */
    bool      dirty;
    uint64_t  mtime;
    void     *priv;      /* watch state */
};

vt_config_t *vt_config_new(void);
vt_config_t *vt_config_new_from_file(const char *path);
void         vt_config_free(vt_config_t *c);
void         vt_config_clear(vt_config_t *c);

int          vt_config_load(vt_config_t *c, const char *path);
int          vt_config_save(vt_config_t *c, const char *path);
bool         vt_config_reload(vt_config_t *c);

/* Read helpers — section may be NULL for "no section". */
const char *vt_config_get(vt_config_t *c, const char *sec, const char *key,
                          const char *def);
long        vt_config_get_int(vt_config_t *c, const char *sec, const char *key,
                              long def);
bool        vt_config_get_bool(vt_config_t *c, const char *sec, const char *key,
                               bool def);
double      vt_config_get_double(vt_config_t *c, const char *sec,
                                  const char *key, double def);

/* Write helpers */
bool         vt_config_set(vt_config_t *c, const char *sec, const char *key,
                           const char *val);
bool         vt_config_set_int(vt_config_t *c, const char *sec, const char *key,
                               long val);
bool         vt_config_set_bool(vt_config_t *c, const char *sec, const char *key,
                                bool val);

/* Defaults */
vt_config_t *vt_config_new_defaults(void);

/* Standard locations */
const char *vt_config_default_path(void);
const char *vt_config_user_dir(void);
const char *vt_config_sysconf_dir(void);

/* Snapshot of well-known config keys used across the DE. */
typedef enum {
    VT_CFG_BACKEND,
    VT_CFG_RENDERER,
    VT_CFG_THEME,
    VT_CFG_DARK_MODE,
    VT_CFG_WALLPAPER,
    VT_CFG_VIDEO_WALLPAPER,
    VT_CFG_VIDEO_VOLUME,
    VT_CFG_COMPOSITOR,
    VT_CFG_ANIMATIONS,
    VT_CFG_VSYNC,
    VT_CFG_SHADOWS,
    VT_CFG_BLUR,
    VT_CFG_SCALE,
    VT_CFG_WORKSPACE_COUNT,
    VT_CFG_PANEL_HEIGHT,
    VT_CFG_PANEL_POSITION,
    VT_CFG_DBUS,
    VT_CFG_LOG_LEVEL,
    VT_CFG_NUM_KNOWN,
} vt_cfg_known_t;

const char *vt_cfg_known_name(vt_cfg_known_t k);
const char *vt_cfg_known_default(vt_cfg_known_t k);

/* Watch for live edits (uses inotify on Linux, falls back to polling). */
typedef void (*vt_config_cb_t)(vt_config_t *c, void *ud);
int  vt_config_watch(vt_config_t *c, vt_config_cb_t cb, void *ud);
void vt_config_unwatch(vt_config_t *c);

#ifdef __cplusplus
}
#endif
#endif
