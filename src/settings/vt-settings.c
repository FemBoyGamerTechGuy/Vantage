/*
 * vt-settings.c — Modular settings system
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The settings system is a collection of small "ops" structs, one per
 * settings module (appearance, displays, keyboard, etc.). Each ops
 * struct exposes load/save/apply/reset/dump callbacks that operate on
 * the underlying vt_config_t.
 *
 * This file owns the registry of ops and the global settings_t.
 */

#define VT_LOG_DOMAIN "settings"
#include <vantage/vt-settings.h>
#include <string.h>
#include <stdlib.h>

vt_settings_t *vt_settings_new(void) {
    vt_settings_t *s = vt_malloc0(sizeof(*s));
    s->cfg = vt_config_new_defaults();
    const vt_settings_ops_t **arr =
        vt_malloc0(sizeof(vt_settings_ops_t *) * VT_SETTINGS_NUM);
    arr[VT_SETTINGS_APPEARANCE] = vt_settings_ops_appearance();
    arr[VT_SETTINGS_DISPLAYS]   = vt_settings_ops_displays();
    arr[VT_SETTINGS_KEYBOARD]   = vt_settings_ops_keyboard();
    arr[VT_SETTINGS_WINDOWS]    = vt_settings_ops_windows();
    arr[VT_SETTINGS_WALLPAPER]  = vt_settings_ops_wallpaper();
    arr[VT_SETTINGS_COMPOSITOR] = vt_settings_ops_compositor();
    arr[VT_SETTINGS_POWER]     = vt_settings_ops_power();
    arr[VT_SETTINGS_STARTUP]   = vt_settings_ops_startup();
    /* unimplemented modules get a NULL ops (treated as no-op) */
    s->ops = arr;
    return s;
}
void vt_settings_free(vt_settings_t *s) {
    if (!s) return;
    if (s->cfg) vt_config_free(s->cfg);
    vt_free(s->ops);
    vt_free(s);
}
int vt_settings_load_all(vt_settings_t *s) {
    int rc = VT_OK;
    for (int i = 0; i < VT_SETTINGS_NUM; i++) {
        if (s->ops && s->ops[i] && s->ops[i]->load)
            rc |= s->ops[i]->load(s);
    }
    return rc;
}
int vt_settings_save_all(vt_settings_t *s) {
    int rc = VT_OK;
    for (int i = 0; i < VT_SETTINGS_NUM; i++) {
        if (s->ops && s->ops[i] && s->ops[i]->save)
            rc |= s->ops[i]->save(s);
    }
    return rc;
}
int vt_settings_apply_all(vt_settings_t *s) {
    int rc = VT_OK;
    for (int i = 0; i < VT_SETTINGS_NUM; i++) {
        if (s->ops && s->ops[i] && s->ops[i]->apply)
            rc |= s->ops[i]->apply(s);
    }
    return rc;
}
int vt_settings_apply(vt_settings_t *s, vt_settings_module_t m) {
    if (!s || m < 0 || m >= VT_SETTINGS_NUM) return VT_ERR_INVAL;
    return (s->ops && s->ops[m] && s->ops[m]->apply) ? s->ops[m]->apply(s) : VT_OK;
}
int vt_settings_reset(vt_settings_t *s, vt_settings_module_t m) {
    if (!s || m < 0 || m >= VT_SETTINGS_NUM) return VT_ERR_INVAL;
    return (s->ops && s->ops[m] && s->ops[m]->reset) ? s->ops[m]->reset(s) : VT_OK;
}
void vt_settings_dump(vt_settings_t *s, vt_settings_module_t m, FILE *fp) {
    if (!s || !fp) return;
    if (s->ops && s->ops[m] && s->ops[m]->dump) s->ops[m]->dump(s, fp);
}
void vt_settings_dump_all(vt_settings_t *s, FILE *fp) {
    if (!s || !fp) return;
    for (int i = 0; i < VT_SETTINGS_NUM; i++) {
        if (s->ops && s->ops[i] && s->ops[i]->dump) {
            fprintf(fp, "=== %s ===\n", s->ops[i]->name);
            s->ops[i]->dump(s, fp);
            fprintf(fp, "\n");
        }
    }
}
