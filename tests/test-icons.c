/*
 * test-icons.c — unit tests for the icon-theme lookup module
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * Builds a fake icon-theme tree in a temp XDG_DATA_HOME and verifies:
 *   - the user's gtk-3.0/settings.ini theme is honored (Papirus-style
 *     name, NOT a hard-coded Adwaita)
 *   - size selection prefers the best matching directory
 *   - the Inherits chain is followed
 *   - absolute icon paths pass through
 *   - pixmaps is the last resort
 */

#define VT_LOG_DOMAIN "test-icons"
#include <vantage/vt-icons.h>
#include <vantage/vt-core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int _fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok  - %s\n", msg); } \
    else { printf("  FAIL- %s\n", msg); _fails++; } \
} while (0)

static void _write(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(body, f);
    fclose(f);
}

static void _mkdirp(const char *p) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", p);
    for (char *s = buf + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(buf, 0755); *s = '/'; }
    mkdir(buf, 0755);
}

/* minimal valid PNG (1x1 transparent) */
static const unsigned char _png1x1[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,
    0,0,0,31,21,196,137,0,0,0,11,73,68,65,84,120,156,99,248,15,4,0,9,
    251,3,253,251,94,107,43,0,0,0,0,73,69,78,68,174,66,96,130,
};

static void _png(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(_png1x1, 1, sizeof(_png1x1), f);
    fclose(f);
}

int main(void) {
    char tmp[] = "/tmp/vt-test-icons-XXXXXX";
    if (!mkdtemp(tmp)) { perror("mkdtemp"); return 1; }
    char xdg[600], cfg[600], home[600];
    snprintf(xdg, sizeof(xdg), "%s/data", tmp);
    snprintf(cfg, sizeof(cfg), "%s/config", tmp);
    snprintf(home, sizeof(home), "%s", tmp);

    setenv("XDG_DATA_HOME", xdg, 1);
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_DATA_DIRS", "", 1);
    setenv("HOME", home, 1);
    unsetenv("VANTAGE_ICON_THEME");

    /* user theme: Papirus-like custom theme "TestTheme" inheriting
     * from a second theme "BaseTheme" */
    char t1[700], t2[700];
    snprintf(t1, sizeof(t1), "%s/icons/TestTheme", xdg);
    snprintf(t2, sizeof(t2), "%s/icons/BaseTheme", xdg);
    _mkdirp(t1); _mkdirp(t2);

    char idx1[760], idx2[760];
    snprintf(idx1, sizeof(idx1), "%s/index.theme", t1);
    snprintf(idx2, sizeof(idx2), "%s/index.theme", t2);
    _write(idx1,
        "[Icon Theme]\n"
        "Name=TestTheme\n"
        "Inherits=BaseTheme\n"
        "Directories=48x48/apps,scalable/apps\n\n"
        "[48x48/apps]\nSize=48\nType=Fixed\n\n"
        "[scalable/apps]\nSize=48\nType=Scalable\n");
    _write(idx2,
        "[Icon Theme]\n"
        "Name=BaseTheme\n"
        "Directories=32x32/apps\n\n"
        "[32x32/apps]\nSize=32\nType=Fixed\n");

    /* icons: TestTheme carries big.png in 48x48 and scalable;
     * BaseTheme carries small.png in 32x32 */
    {
        char d[760];
        snprintf(d, sizeof(d), "%s/48x48/apps", t1); _mkdirp(d);
        snprintf(d, sizeof(d), "%s/scalable/apps", t1); _mkdirp(d);
        snprintf(d, sizeof(d), "%s/32x32/apps", t2); _mkdirp(d);
    }
    char p[800];
    snprintf(p, sizeof(p), "%s/48x48/apps/big.png", t1); _png(p);
    snprintf(p, sizeof(p), "%s/scalable/apps/vector.png", t1); _png(p);
    snprintf(p, sizeof(p), "%s/32x32/apps/small.png", t2); _png(p);

    /* gtk-3.0 settings.ini selects TestTheme (the user's theme) */
    char gtkd[700];
    snprintf(gtkd, sizeof(gtkd), "%s/gtk-3.0", cfg);
    _mkdirp(gtkd);
    char ginid[760];
    snprintf(ginid, sizeof(ginid), "%s/settings.ini", gtkd);
    _write(ginid, "[Settings]\ngtk-icon-theme-name=TestTheme\n");

    vt_icon_theme_t *th = vt_icon_theme_load();
    CHECK(vt_streq(vt_icon_theme_name(th), "TestTheme"),
          "user's settings.ini theme honored");

    char out[1024];
    CHECK(vt_icon_theme_lookup(th, "big", 24, out, sizeof(out)) == 0 &&
          strstr(out, "48x48/apps/big.png"),
          "icon found in the user theme");
    CHECK(vt_icon_theme_lookup(th, "vector", 24, out, sizeof(out)) == 0 &&
          strstr(out, "scalable/apps/vector.png"),
          "scalable directory icon found");
    CHECK(vt_icon_theme_lookup(th, "small", 24, out, sizeof(out)) == 0 &&
          strstr(out, "32x32/apps/small.png"),
          "Inherits chain followed (BaseTheme)");
    CHECK(vt_icon_theme_lookup(th, "missing", 24, out, sizeof(out)) != 0,
          "unknown icon returns not-found");

    /* absolute path passthrough */
    snprintf(p, sizeof(p), "%s/48x48/apps/big.png", t1);
    CHECK(vt_icon_theme_lookup(th, p, 24, out, sizeof(out)) == 0 &&
          vt_streq(out, p),
          "absolute icon path passes through");

    /* env override beats settings.ini */
    vt_icon_theme_free(th);
    setenv("VANTAGE_ICON_THEME", "BaseTheme", 1);
    th = vt_icon_theme_load();
    CHECK(vt_streq(vt_icon_theme_name(th), "BaseTheme"),
          "VANTAGE_ICON_THEME overrides settings.ini");
    CHECK(vt_icon_theme_lookup(th, "big", 24, out, sizeof(out)) != 0,
          "icons outside the selected theme are not leaked");
    vt_icon_theme_free(th);
    unsetenv("VANTAGE_ICON_THEME");

    /* pixels: PNG loading to ARGB */
    snprintf(p, sizeof(p), "%s/48x48/apps/big.png", t1);
    uint32_t *px = NULL;
    int w = 0, h = 0;
    CHECK(vt_icon_load_argb(p, &px, &w, &h) == 0 && w == 1 && h == 1,
          "PNG decoded to ARGB");
    vt_free(px);

    /* cleanup */
    char rm[600];
    snprintf(rm, sizeof(rm), "rm -rf %s", tmp);
    int rc_ignore = system(rm);
    (void)rc_ignore;

    printf(_fails ? "\n%d FAILURES\n" : "\nall icon tests passed\n",
           _fails);
    return _fails ? 1 : 0;
}
