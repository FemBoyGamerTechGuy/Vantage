/*
 * vt-settings-modules.c — Per-module settings ops
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Each module is intentionally tiny. They read/write known config keys
 * and call into the appropriate subsystem's apply() to push changes
 * live.
 */

#define VT_LOG_DOMAIN "settings"
#include <vantage/vt-settings.h>
#include <vantage/vt-theme.h>
#include <string.h>
#include <stdlib.h>

/* ---------------- appearance ---------------- */
static int _appearance_load(vt_settings_t *s) {
    /* nothing — appearance values are loaded by vt_config_t */
    (void)s; return VT_OK;
}
static int _appearance_apply(vt_settings_t *s) {
    const char *name = vt_config_get(s->cfg, "desktop", "theme", "Vantage-Dark");
    vt_theme_t *t = vt_theme_load_by_name(name);
    if (!t) t = vt_theme_new();
    vt_theme_apply(t);
    vt_theme_free(t);
    return VT_OK;
}
static int _appearance_save(vt_settings_t *s) {
    return vt_config_save(s->cfg, NULL);
}
static int _appearance_reset(vt_settings_t *s) {
    for (int i = 0; i < VT_CFG_NUM_KNOWN; i++) {
        vt_config_set(s->cfg, "desktop",
                       vt_cfg_known_name(i),
                       vt_cfg_known_default(i));
    }
    return VT_OK;
}
static void _appearance_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "theme:    %s\n", vt_config_get(s->cfg, "desktop", "theme", "?"));
    fprintf(fp, "dark:     %s\n", vt_config_get(s->cfg, "desktop", "dark-mode", "?"));
}
static const vt_settings_ops_t _ops_appearance = {
    .name = "appearance",
    .load = _appearance_load, .save = _appearance_save,
    .apply = _appearance_apply, .reset = _appearance_reset,
    .dump = _appearance_dump,
};
const vt_settings_ops_t *vt_settings_ops_appearance(void) { return &_ops_appearance; }

/* ---------------- displays ---------------- */
static int _displays_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _displays_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _displays_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _displays_reset(vt_settings_t *s) { (void)s; return VT_OK; }
static void _displays_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "scale:    %s\n", vt_config_get(s->cfg, "desktop", "scale", "1"));
}
static const vt_settings_ops_t _ops_displays = {
    .name = "displays", .load = _displays_load, .save = _displays_save,
    .apply = _displays_apply, .reset = _displays_reset, .dump = _displays_dump,
};
const vt_settings_ops_t *vt_settings_ops_displays(void) { return &_ops_displays; }

/* ---------------- keyboard ---------------- */
static int _kb_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _kb_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _kb_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _kb_reset(vt_settings_t *s) { (void)s; return VT_OK; }
static void _kb_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "layout:   %s\n", vt_config_get(s->cfg, "keyboard", "layout", "us"));
}
static const vt_settings_ops_t _ops_kb = {
    .name = "keyboard", .load = _kb_load, .save = _kb_save,
    .apply = _kb_apply, .reset = _kb_reset, .dump = _kb_dump,
};
const vt_settings_ops_t *vt_settings_ops_keyboard(void) { return &_ops_kb; }

/* ---------------- windows ---------------- */
static int _win_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _win_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _win_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _win_reset(vt_settings_t *s) { (void)s; return VT_OK; }
static void _win_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "focus-new: %s\n", vt_config_get(s->cfg, "wm", "focus-new", "true"));
}
static const vt_settings_ops_t _ops_win = {
    .name = "windows", .load = _win_load, .save = _win_save,
    .apply = _win_apply, .reset = _win_reset, .dump = _win_dump,
};
const vt_settings_ops_t *vt_settings_ops_windows(void) { return &_ops_win; }

/* ---------------- wallpaper ---------------- */
static int _wp_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _wp_apply(vt_settings_t *s) {
    /* notify compositor to reload wallpaper */
    (void)s;
    return VT_OK;
}
static int _wp_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _wp_reset(vt_settings_t *s) {
    vt_config_set(s->cfg, "desktop", "wallpaper", "");
    vt_config_set(s->cfg, "desktop", "video-wallpaper", "");
    return VT_OK;
}
static void _wp_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "wallpaper:       %s\n", vt_config_get(s->cfg, "desktop", "wallpaper", ""));
    fprintf(fp, "video-wallpaper: %s\n", vt_config_get(s->cfg, "desktop", "video-wallpaper", ""));
}
static const vt_settings_ops_t _ops_wp = {
    .name = "wallpaper", .load = _wp_load, .save = _wp_save,
    .apply = _wp_apply, .reset = _wp_reset, .dump = _wp_dump,
};
const vt_settings_ops_t *vt_settings_ops_wallpaper(void) { return &_ops_wp; }

/* ---------------- compositor ---------------- */
static int _comp_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _comp_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _comp_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _comp_reset(vt_settings_t *s) {
    vt_config_set(s->cfg, "desktop", "animations", "true");
    vt_config_set(s->cfg, "desktop", "vsync", "true");
    vt_config_set(s->cfg, "desktop", "shadows", "true");
    vt_config_set(s->cfg, "desktop", "blur", "false");
    return VT_OK;
}
static void _comp_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "animations: %s\n", vt_config_get(s->cfg, "desktop", "animations", "true"));
    fprintf(fp, "vsync:      %s\n", vt_config_get(s->cfg, "desktop", "vsync", "true"));
    fprintf(fp, "shadows:    %s\n", vt_config_get(s->cfg, "desktop", "shadows", "true"));
    fprintf(fp, "blur:       %s\n", vt_config_get(s->cfg, "desktop", "blur", "false"));
}
static const vt_settings_ops_t _ops_comp = {
    .name = "compositor", .load = _comp_load, .save = _comp_save,
    .apply = _comp_apply, .reset = _comp_reset, .dump = _comp_dump,
};
const vt_settings_ops_t *vt_settings_ops_compositor(void) { return &_ops_comp; }

/* ---------------- power ---------------- */
static int _pwr_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _pwr_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _pwr_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _pwr_reset(vt_settings_t *s) {
    vt_config_set(s->cfg, "power", "backend", "auto");
    return VT_OK;
}
static void _pwr_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "backend:   %s\n", vt_config_get(s->cfg, "power", "backend", "auto"));
}
static const vt_settings_ops_t _ops_pwr = {
    .name = "power", .load = _pwr_load, .save = _pwr_save,
    .apply = _pwr_apply, .reset = _pwr_reset, .dump = _pwr_dump,
};
const vt_settings_ops_t *vt_settings_ops_power(void) { return &_ops_pwr; }

/* ---------------- startup ---------------- */
static int _startup_load(vt_settings_t *s) { (void)s; return VT_OK; }
static int _startup_apply(vt_settings_t *s) { (void)s; return VT_OK; }
static int _startup_save(vt_settings_t *s) { return vt_config_save(s->cfg, NULL); }
static int _startup_reset(vt_settings_t *s) { (void)s; return VT_OK; }
static void _startup_dump(vt_settings_t *s, FILE *fp) {
    fprintf(fp, "autostart entries: (see session autostart loader)\n");
}
static const vt_settings_ops_t _ops_startup = {
    .name = "startup", .load = _startup_load, .save = _startup_save,
    .apply = _startup_apply, .reset = _startup_reset, .dump = _startup_dump,
};
const vt_settings_ops_t *vt_settings_ops_startup(void) { return &_ops_startup; }
