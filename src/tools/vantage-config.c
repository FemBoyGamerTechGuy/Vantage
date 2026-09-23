/*
 * vantage-config.c — Configuration CLI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define VT_LOG_DOMAIN "config"
#include <vantage/vt-core.h>
#include <vantage/vt-config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void _usage(const char *prog) {
    fprintf(stderr,
        "Vantage config tool\n\n"
        "Usage:\n"
        "  %s path\n"
        "  %s init\n"
        "  %s list\n"
        "  %s get <section> <key>\n"
        "  %s set <section> <key> <value>\n"
        "  %s get-defaults\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { _usage(argv[0]); return 1; }
    vt_log_set_level(VT_LOG_WARN);

    if (strcmp(argv[1], "path") == 0) {
        printf("%s\n", vt_config_default_path());
        return 0;
    }
    if (strcmp(argv[1], "get-defaults") == 0) {
        vt_config_t *d = vt_config_new_defaults();
        for (int i = 0; i < VT_CFG_NUM_KNOWN; i++) {
            printf("%-22s = %s\n",
                   vt_cfg_known_name(i),
                   vt_cfg_known_default(i));
        }
        vt_config_free(d);
        return 0;
    }
    if (strcmp(argv[1], "init") == 0) {
        /* write defaults to the default config path */
        vt_config_t *d = vt_config_new_defaults();
        char *p = vt_path_expand(vt_config_default_path());
        /* mkdir -p */
        char *parent = vt_file_dirname(p);
        vt_file_mkdir_p(parent, 0755);
        vt_free(parent);
        vt_config_save(d, p);
        printf("Wrote %s\n", p);
        vt_free(p);
        vt_config_free(d);
        return 0;
    }
    /* All other commands need a loaded config */
    vt_config_t *cfg = vt_config_new_defaults();
    if (vt_config_load(cfg, vt_config_default_path()) != VT_OK)
        vt_logw("config: %s not found — using defaults", vt_config_default_path());

    if (strcmp(argv[1], "list") == 0) {
        /* Print all key/value pairs */
        for (size_t i = 0; i < cfg->sections.size; i++) {
            vt_cfg_sec_t *sec = vt_vec_at(&cfg->sections, i);
            if (sec->name && *sec->name) printf("[%s]\n", sec->name);
            for (size_t j = 0; j < sec->keys.size; j++) {
                vt_cfg_key_t *k = vt_vec_at(&sec->keys, j);
                printf("%s = %s\n", k->key, k->val);
            }
            printf("\n");
        }
    } else if (strcmp(argv[1], "get") == 0 && argc == 4) {
        const char *v = vt_config_get(cfg, argv[2], argv[3], "");
        printf("%s\n", v);
    } else if (strcmp(argv[1], "set") == 0 && argc == 5) {
        vt_config_set(cfg, argv[2], argv[3], argv[4]);
        vt_config_save(cfg, NULL);
    } else {
        _usage(argv[0]);
        return 1;
    }
    vt_config_free(cfg);
    return 0;
}
