/*
 * vantage-settings.c — Settings CLI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A small text-based tool for reading/writing Vantage settings. Designed
 * to be driven by a future Qt6 or GTK GUI; the GUI shells out to this
 * binary, keeping the core C-only.
 *
 * Usage:
 *   vantage-settings list                     # list all settings
 *   vantage-settings get <module> <key>
 *   vantage-settings set <module> <key> <val>
 *   vantage-settings apply [<module>]
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
        "  %s get <section> <key>\n"
        "  %s set <section> <key> <value>\n"
        "  %s apply [<module>]\n"
        "  %s save\n"
        "  %s reset <module>\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { _usage(argv[0]); return 1; }
    vt_log_set_level(VT_LOG_WARN);
    vt_settings_t *s = vt_settings_new();
    vt_config_load(s->cfg, vt_config_default_path());

    if (strcmp(argv[1], "list") == 0) {
        vt_settings_dump_all(s, stdout);
    } else if (strcmp(argv[1], "get") == 0 && argc == 4) {
        const char *v = vt_config_get(s->cfg, argv[2], argv[3], "");
        printf("%s\n", v);
    } else if (strcmp(argv[1], "set") == 0 && argc == 5) {
        vt_config_set(s->cfg, argv[2], argv[3], argv[4]);
        vt_config_save(s->cfg, NULL);
    } else if (strcmp(argv[1], "apply") == 0) {
        if (argc == 3) {
            /* TODO: module name lookup */
            vt_settings_apply_all(s);
        } else vt_settings_apply_all(s);
    } else if (strcmp(argv[1], "save") == 0) {
        vt_settings_save_all(s);
    } else if (strcmp(argv[1], "reset") == 0 && argc == 3) {
        /* TODO: module name lookup */
        vt_settings_reset(s, VT_SETTINGS_APPEARANCE);
    } else {
        _usage(argv[0]);
        return 1;
    }
    vt_settings_free(s);
    return 0;
}
