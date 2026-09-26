/*
 * vantage-theme.c — Theme CLI
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Lists installed themes, applies a theme, or generates one from an
 * accent color.
 *
 * Usage:
 *   vantage-theme list
 *   vantage-theme apply <name>
 *   vantage-theme generate <accent-hex> [--dark|--light]
 */

#define VT_LOG_DOMAIN "theme"
#include <vantage/vt-core.h>
#include <vantage/vt-theme.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void _usage(const char *prog) {
    fprintf(stderr,
        "Vantage theme tool\n\n"
        "Usage:\n"
        "  %s list\n"
        "  %s apply <name>\n"
        "  %s info <name>\n"
        "  %s generate <accent-hex> [--dark|--light]\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { _usage(argv[0]); return 1; }
    vt_log_set_level(VT_LOG_WARN);

    if (strcmp(argv[1], "list") == 0) {
        size_t n;
        char **items = vt_theme_list_installed(&n);
        printf("Installed themes (%s):\n", vt_theme_dir());
        for (size_t i = 0; i < n; i++) {
            char *p = vt_theme_path_for(items[i]);
            printf("  %-20s  %s\n", items[i], p ? p : "(not found)");
            vt_free(p);
            vt_free(items[i]);
        }
        vt_free(items);
        if (n == 0)
            printf("  (none)\n");
        return 0;
    }
    if (strcmp(argv[1], "apply") == 0 && argc == 3) {
        vt_theme_t *t = vt_theme_load_by_name(argv[2]);
        if (!t) {
            fprintf(stderr, "theme '%s' not found\n", argv[2]);
            return 1;
        }
        vt_theme_apply(t);
        printf("Applied theme: %s\n", argv[2]);
        vt_theme_free(t);
        return 0;
    }
    if (strcmp(argv[1], "info") == 0 && argc == 3) {
        vt_theme_t *t = vt_theme_load_by_name(argv[2]);
        if (!t) { fprintf(stderr, "not found\n"); return 1; }
        printf("name:    %s\n", t->name);
        printf("dark:    %s\n", t->dark ? "true" : "false");
        printf("accent:  %s\n", t->accent_hex);
        printf("bg:      %s\n", t->bg_hex);
        printf("fg:      %s\n", t->fg_hex);
        printf("surface: %s\n", t->surface_hex);
        printf("border:  %s\n", t->border_hex);
        printf("radius:  %d\n", t->radius);
        printf("spacing: %d\n", t->spacing);
        printf("font:    %s %dpt\n", t->font_family, t->font_size_pt);
        printf("icons:   %s\n", t->icon_theme);
        vt_theme_free(t);
        return 0;
    }
    if (strcmp(argv[1], "generate") == 0 && argc >= 3) {
        bool dark = true;
        if (argc == 4 && strcmp(argv[3], "--light") == 0) dark = false;
        vt_palette_t pal;
        vt_theme_palette_from_accent(argv[2], dark, &pal);
        char *bg = vt_theme_rgba_to_hex(dark ? pal.bg_dark : pal.bg_light);
        char *fg = vt_theme_rgba_to_hex(dark ? pal.fg_dark : pal.fg_light);
        char *sf = vt_theme_rgba_to_hex(dark ? pal.surface_dark : pal.surface_light);
        char *br = vt_theme_rgba_to_hex(dark ? pal.border_dark : pal.border_light);
        printf("Generated palette (accent=%s, %s):\n", argv[2], dark?"dark":"light");
        printf("  bg:      %s\n", bg);
        printf("  fg:      %s\n", fg);
        printf("  surface: %s\n", sf);
        printf("  border:  %s\n", br);
        printf("  accent:  %s\n", argv[2]);
        vt_free(bg); vt_free(fg); vt_free(sf); vt_free(br);
        return 0;
    }
    _usage(argv[0]);
    return 1;
}
