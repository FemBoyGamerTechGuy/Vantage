/*
 * vantage-settings.c — Settings CLI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A small text-based tool for reading/writing Vantage settings, applying
 * them live, and regenerating theme files. Designed to be driven by any
 * frontend (a future Qt6/GTK GUI can shell out to this binary, keeping
 * the core C-only).
 *
 * Usage:
 *   vantage-settings list                    dump every module
 *   vantage-settings modules                 list module names
 *   vantage-settings get <section> <key>
 *   vantage-settings set <section> <key> <value>   (writes + saves)
 *   vantage-settings apply [<module>]        push changes live
 *   vantage-settings reset <module>          restore defaults
 */

#define VT_LOG_DOMAIN "settings"
#include <vantage/vt-core.h>
#include <vantage/vt-settings.h>
#include <vantage/vt-config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void _usage(const char *prog) {
    fprintf(stderr,
        "Vantage settings tool\n\n"
        "Usage:\n"
        "  %s list\n"
        "  %s modules\n"
        "  %s get <section> <key>\n"
        "  %s set <section> <key> <value>\n"
        "  %s apply [<module>]\n"
        "  %s reset <module>\n",
        prog, prog, prog, prog, prog, prog);
}

static const char *const _mod_names[] = {
    [VT_SETTINGS_APPEARANCE] = "appearance",
    [VT_SETTINGS_DISPLAYS]   = "displays",
    [VT_SETTINGS_KEYBOARD]   = "keyboard",
    [VT_SETTINGS_MOUSE]      = "mouse",
    [VT_SETTINGS_SHORTCUTS]  = "shortcuts",
    [VT_SETTINGS_WINDOWS]    = "windows",
    [VT_SETTINGS_WORKSPACES] = "workspaces",
    [VT_SETTINGS_WALLPAPER]  = "wallpaper",
    [VT_SETTINGS_COMPOSITOR] = "compositor",
    [VT_SETTINGS_POWER]      = "power",
    [VT_SETTINGS_STARTUP]    = "startup",
    [VT_SETTINGS_INPUT]      = "input",
    [VT_SETTINGS_BACKEND]    = "backend",
};

static int _module_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < (int)(sizeof(_mod_names) / sizeof(_mod_names[0])); i++)
        if (_mod_names[i] && vt_strcaseeq(_mod_names[i], name)) return i;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { _usage(argv[0]); return 1; }
    vt_log_set_level(VT_LOG_WARN);
    vt_settings_t *s = vt_settings_new();
    vt_config_load(s->cfg, vt_config_default_path());

    if (strcmp(argv[1], "list") == 0) {
        vt_settings_dump_all(s, stdout);
    } else if (strcmp(argv[1], "modules") == 0) {
        for (int i = 0; i < (int)(sizeof(_mod_names) / sizeof(_mod_names[0])); i++)
            if (_mod_names[i]) printf("%s\n", _mod_names[i]);
    } else if (strcmp(argv[1], "get") == 0 && argc == 4) {
        const char *v = vt_config_get(s->cfg, argv[2], argv[3], "");
        printf("%s\n", v);
    } else if (strcmp(argv[1], "set") == 0 && argc == 5) {
        vt_config_set(s->cfg, argv[2], argv[3], argv[4]);
        if (vt_config_save(s->cfg, NULL))
            fprintf(stderr, "settings: could not save config\n");
        else
            printf("set %s.%s = %s\n", argv[2], argv[3], argv[4]);
    } else if (strcmp(argv[1], "apply") == 0) {
        if (argc == 3) {
            int m = _module_from_name(argv[2]);
            if (m < 0) {
                fprintf(stderr, "settings: unknown module '%s'\n", argv[2]);
                vt_settings_free(s);
                return 1;
            }
            vt_settings_apply(s, (vt_settings_module_t)m);
            printf("applied %s\n", argv[2]);
        } else {
            vt_settings_apply_all(s);
            printf("applied all\n");
        }
    } else if (strcmp(argv[1], "reset") == 0 && argc == 3) {
        int m = _module_from_name(argv[2]);
        if (m < 0) {
            fprintf(stderr, "settings: unknown module '%s'\n", argv[2]);
            vt_settings_free(s);
            return 1;
        }
        vt_settings_reset(s, (vt_settings_module_t)m);
        vt_config_save(s->cfg, NULL);
        printf("reset %s\n", argv[2]);
    } else {
        _usage(argv[0]);
        vt_settings_free(s);
        return 1;
    }
    vt_settings_free(s);
    return 0;
}
