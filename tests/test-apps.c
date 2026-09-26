/*
 * test-apps.c — unit tests for the shared .desktop discovery module
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * Covers the exact regressions seen on real hardware:
 *   - Categories= arriving AFTER Name=/Exec= in the file (the old
 *     parsers stopped reading at Name+Exec and dumped every app into
 *     "Other", which is why the Wayland menu showed almost no
 *     categories)
 *   - locale-aware Name selection (Name[ru] must not leak into an
 *     English session; a Russian session must see the Russian name)
 *   - NoDisplay / Hidden / NotShowIn filtering
 *   - Exec field-code stripping
 *   - Terminal=true wrapping
 *   - category normalization incl. the Accessories/Utilities split
 */

#define VT_LOG_DOMAIN "test-apps"
#include <vantage/vt-apps.h>
#include <vantage/vt-core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int _fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  - %s\n", msg); } \
    else { printf("  FAIL- %s\n", msg); _fails++; } \
} while (0)

static void _write(const char *dir, const char *name, const char *body) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror("write"); exit(1); }
    fputs(body, f);
    fclose(f);
}

int main(void) {
    char tmpdir[] = "/tmp/vt-test-apps-XXXXXX";
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }
    setenv("XDG_DATA_HOME", tmpdir, 1);      /* keep icons/apps isolated */

    /* Categories= AFTER Name/Exec — the real-world ordering */
    _write(tmpdir, "z-net.desktop",
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Web Browser\n"
        "Exec=webbrowser %u\n"
        "Icon=webbrowser\n"
        "Terminal=false\n"
        "Categories=Network;\n");
    /* localized names: Name[ru] BEFORE Name= (order must not matter) */
    _write(tmpdir, "y-ru.desktop",
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name[ru]=Текстовый редактор\n"
        "Name[ru_RU]=Русский редактор\n"
        "Name=Text Editor\n"
        "Exec=editor %F\n"
        "Categories=Utility;\n");
    /* terminal utility → Utilities */
    _write(tmpdir, "x-term.desktop",
        "[Desktop Entry]\n"
        "Name=System Monitor\n"
        "Exec=htop\n"
        "Terminal=true\n"
        "Categories=System;Utility;\n");
    /* plain utility → Accessories */
    _write(tmpdir, "w-calc.desktop",
        "[Desktop Entry]\n"
        "Name=Calculator\n"
        "Exec=calc\n"
        "Categories=Utility;GNOME;\n");
    /* NoDisplay must be hidden */
    _write(tmpdir, "v-hidden.desktop",
        "[Desktop Entry]\n"
        "Name=Secret\n"
        "Exec=secret\n"
        "NoDisplay=true\n"
        "Categories=Utility;\n");
    /* NotShowIn=Vantage must be hidden */
    _write(tmpdir, "u-notfor.desktop",
        "[Desktop Entry]\n"
        "Name=Not For Vantage\n"
        "Exec=nfv\n"
        "NotShowIn=Vantage;\n");
    /* second group content must be ignored */
    _write(tmpdir, "t-groups.desktop",
        "[Desktop Entry]\n"
        "Name=Grouped\n"
        "Exec=grouped\n"
        "Categories=Development;\n"
        "\n"
        "[Desktop Action whatever]\n"
        "Name=Action Name\n"
        "Exec=action\n"
        "Categories=Game;\n");
    /* mixed categories: Network beats Utility */
    _write(tmpdir, "s-mixed.desktop",
        "[Desktop Entry]\n"
        "Name=Chat Client\n"
        "Exec=chat\n"
        "Categories=Utility;Network;\n");

    const char *dirs[1] = { tmpdir };
    vt_apps_t *apps = vt_apps_load_dirs(dirs, 1);

    CHECK(vt_apps_n(apps) == 6, "6 of 8 entries visible (NoDisplay + "
          "NotShowIn filtered)");

    /* --- the critical regression: Categories= after Exec= --- */
    const vt_app_t *net = vt_apps_find_id(apps, "z-net.desktop");
    CHECK(net != NULL, "z-net.desktop parsed");
    if (net) {
        CHECK(vt_streq(net->category, "Internet"),
              "Categories= after Exec= still maps to Internet");
        CHECK(vt_streq(net->exec, "webbrowser"),
              "Exec field codes (%u) stripped");
        CHECK(vt_streq(net->icon, "webbrowser"), "Icon captured");
    }

    /* --- locale-aware names --- */
    const vt_app_t *ru = vt_apps_find_id(apps, "y-ru.desktop");
    CHECK(ru != NULL, "y-ru.desktop parsed");
    unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); setenv("LANG", "C", 1);
    vt_apps_t *a_en = vt_apps_load_dirs(dirs, 1);
    const vt_app_t *ru_en = vt_apps_find_id(a_en, "y-ru.desktop");
    CHECK(ru_en && vt_streq(ru_en->name, "Text Editor"),
          "C locale shows Name= (localized line order irrelevant)");
    vt_apps_free(a_en);

    setenv("LC_ALL", "ru_RU.UTF-8", 1);
    vt_apps_t *a_ru = vt_apps_load_dirs(dirs, 1);
    const vt_app_t *ru_ru = vt_apps_find_id(a_ru, "y-ru.desktop");
    CHECK(ru_ru && vt_streq(ru_ru->name, "Русский редактор"),
          "ru_RU locale picks Name[ru_RU]");
    vt_apps_free(a_ru);
    setenv("LC_ALL", "ru.UTF-8", 1);
    vt_apps_t *a_ru2 = vt_apps_load_dirs(dirs, 1);
    const vt_app_t *ru_ru2 = vt_apps_find_id(a_ru2, "y-ru.desktop");
    CHECK(ru_ru2 && vt_streq(ru_ru2->name, "Текстовый редактор"),
          "ru locale (no region) picks Name[ru]");
    vt_apps_free(a_ru2);
    unsetenv("LC_ALL");

    /* --- category normalization --- */
    const vt_app_t *term = vt_apps_find_id(apps, "x-term.desktop");
    CHECK(term && vt_streq(term->category, "System"),
          "System beats Utility in mixed categories");
    CHECK(term && term->terminal, "Terminal=true captured");
    setenv("VANTAGE_TERMINAL", "sh", 1);
    char *cmd = term ? vt_apps_launch_cmd(term) : NULL;
    CHECK(cmd && vt_streq(cmd, "sh -e htop"),
          "Terminal=true wrapped with a real emulator");
    unsetenv("VANTAGE_TERMINAL");
    vt_free(cmd);

    const vt_app_t *calc = vt_apps_find_id(apps, "w-calc.desktop");
    CHECK(calc && vt_streq(calc->category, "Accessories"),
          "plain Utility → Accessories");
    const vt_app_t *mixed = vt_apps_find_id(apps, "s-mixed.desktop");
    CHECK(mixed && vt_streq(mixed->category, "Internet"),
          "Network beats Utility (specific category priority)");

    /* --- group isolation --- */
    const vt_app_t *grp = vt_apps_find_id(apps, "t-groups.desktop");
    CHECK(grp && vt_streq(grp->category, "Development"),
          "[Desktop Action] group values ignored");

    /* --- the fixed category table --- */
    CHECK(vt_apps_category_count() == 11, "11 display categories");
    CHECK(vt_streq(vt_apps_category_label(0), "Accessories") &&
          vt_streq(vt_apps_category_label(5), "Internet") &&
          vt_streq(vt_apps_category_label(10), "Other"),
          "category table order");
    CHECK(vt_apps_in_category(apps, vt_apps_category_index("Internet")) == 2,
          "category row counting works");

    /* --- sorted by name --- */
    bool sorted = true;
    for (size_t i = 0; i + 1 < vt_apps_n(apps); i++) {
        const vt_app_t *x = vt_apps_at(apps, i);
        const vt_app_t *y = vt_apps_at(apps, i + 1);
        if (strcasecmp(x->name, y->name) > 0) sorted = false;
    }
    CHECK(sorted, "applications sorted by name");

    vt_apps_free(apps);

    /* cleanup */
    char rm[600];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmpdir);
    int rc_ignore = system(rm);
    (void)rc_ignore;

    printf(_fails ? "\n%d FAILURES\n" : "\nall app-db tests passed\n",
           _fails);
    return _fails ? 1 : 0;
}
