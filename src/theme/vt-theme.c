/*
 * vt-theme.c — Vantage theme engine
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Loads a JSON theme description, applies it to the live compositor,
 * and generates GTK CSS + Qt6 QSS files so user GTK/Qt6 apps pick up
 * the same theme automatically (via XDG_CONFIG_HOME).
 *
 * Uses json-c (optional). If json-c is unavailable, themes are loaded
 * from a simple key=value format and the palette generator falls back
 * to a hardcoded palette.
 */

#define VT_LOG_DOMAIN "theme"
#include <vantage/vt-theme.h>
#include <vantage/vt-paths.h>

#if defined(VT_HAVE_JSONC)
#include <json-c/json.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

vt_theme_t *vt_theme_new(void) {
    vt_theme_t *t = vt_malloc0(sizeof(*t));
    t->dark = true;
    t->accent_hex = vt_strdup("#3b82f6");
    t->bg_hex = vt_strdup("#1e1e2e");
    t->fg_hex = vt_strdup("#cdd6f4");
    t->surface_hex = vt_strdup("#313244");
    t->border_hex = vt_strdup("#45475a");
    t->radius = 8;
    t->spacing = 8;
    t->font_family = vt_strdup("Sans");
    t->font_size_pt = 11;
    t->icon_theme = vt_strdup("Adwaita");
    t->gtk_theme = vt_strdup("Vantage");
    t->qt_theme = vt_strdup("Vantage");
    return t;
}

void vt_theme_free(vt_theme_t *t) {
    if (!t) return;
    vt_free(t->name);
    vt_free(t->accent_hex); vt_free(t->bg_hex); vt_free(t->fg_hex);
    vt_free(t->surface_hex); vt_free(t->border_hex);
    vt_free(t->font_family); vt_free(t->icon_theme);
    vt_free(t->gtk_theme); vt_free(t->qt_theme);
    vt_free(t);
}

vt_theme_t *vt_theme_load(const char *path) {
    if (!path) return NULL;
    size_t len = 0;
    char *buf = vt_file_read_all(path, &len);
    if (!buf) return NULL;
    vt_theme_t *t = vt_theme_new();
    t->name = vt_file_basename(path);

#if defined(VT_HAVE_JSONC)
    json_object *root = json_tokener_parse(buf);
    if (root) {
        json_object *o;
        if (json_object_object_get_ex(root, "dark", &o))
            t->dark = json_object_get_boolean(o);
        if (json_object_object_get_ex(root, "accent", &o)) {
            vt_free(t->accent_hex);
            t->accent_hex = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "bg", &o)) {
            vt_free(t->bg_hex);
            t->bg_hex = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "fg", &o)) {
            vt_free(t->fg_hex);
            t->fg_hex = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "surface", &o)) {
            vt_free(t->surface_hex);
            t->surface_hex = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "border", &o)) {
            vt_free(t->border_hex);
            t->border_hex = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "radius", &o))
            t->radius = json_object_get_int(o);
        if (json_object_object_get_ex(root, "spacing", &o))
            t->spacing = json_object_get_int(o);
        if (json_object_object_get_ex(root, "font_family", &o)) {
            vt_free(t->font_family);
            t->font_family = vt_strdup(json_object_get_string(o));
        }
        if (json_object_object_get_ex(root, "font_size_pt", &o))
            t->font_size_pt = json_object_get_int(o);
        if (json_object_object_get_ex(root, "icon_theme", &o)) {
            vt_free(t->icon_theme);
            t->icon_theme = vt_strdup(json_object_get_string(o));
        }
        json_object_put(root);
    } else
#endif
    {
        /* simple key=value fallback */
        char *p = buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                char *k = vt_strtrim(p);
                char *v = vt_strtrim(eq + 1);
                if (vt_streq(k, "dark")) vt_parse_bool(v, &t->dark);
                else if (vt_streq(k, "accent")) { vt_free(t->accent_hex); t->accent_hex = vt_strdup(v); }
                else if (vt_streq(k, "bg")) { vt_free(t->bg_hex); t->bg_hex = vt_strdup(v); }
                else if (vt_streq(k, "fg")) { vt_free(t->fg_hex); t->fg_hex = vt_strdup(v); }
                else if (vt_streq(k, "surface")) { vt_free(t->surface_hex); t->surface_hex = vt_strdup(v); }
                else if (vt_streq(k, "border")) { vt_free(t->border_hex); t->border_hex = vt_strdup(v); }
                else if (vt_streq(k, "radius")) t->radius = atoi(v);
                else if (vt_streq(k, "spacing")) t->spacing = atoi(v);
                else if (vt_streq(k, "font_family")) { vt_free(t->font_family); t->font_family = vt_strdup(v); }
                else if (vt_streq(k, "font_size_pt")) t->font_size_pt = atoi(v);
                else if (vt_streq(k, "icon_theme")) { vt_free(t->icon_theme); t->icon_theme = vt_strdup(v); }
            }
            if (!nl) break;
            p = nl + 1;
        }
    }
    vt_free(buf);
    return t;
}

vt_theme_t *vt_theme_load_by_name(const char *name) {
    if (!name) return NULL;
    char *p = vt_theme_path_for(name);
    if (!p) return NULL;
    vt_theme_t *t = vt_theme_load(p);
    vt_free(p);
    return t;
}

int vt_theme_save(const vt_theme_t *t, const char *path) {
    if (!t || !path) return VT_ERR_INVAL;
    FILE *fp = fopen(path, "w");
    if (!fp) return VT_ERR_IO;
    fprintf(fp,
        "{\n"
        "  \"dark\": %s,\n"
        "  \"accent\": \"%s\",\n"
        "  \"bg\": \"%s\",\n"
        "  \"fg\": \"%s\",\n"
        "  \"surface\": \"%s\",\n"
        "  \"border\": \"%s\",\n"
        "  \"radius\": %d,\n"
        "  \"spacing\": %d,\n"
        "  \"font_family\": \"%s\",\n"
        "  \"font_size_pt\": %d,\n"
        "  \"icon_theme\": \"%s\"\n"
        "}\n",
        t->dark ? "true" : "false",
        t->accent_hex, t->bg_hex, t->fg_hex, t->surface_hex, t->border_hex,
        t->radius, t->spacing, t->font_family, t->font_size_pt, t->icon_theme);
    fclose(fp);
    return VT_OK;
}

char **vt_theme_list_installed(size_t *out_n) {
    char *dir = vt_theme_dir();
    size_t n = 0;
    char **items = vt_file_list_dir(dir, &n);
    vt_free(dir);
    /* also list the bundled themes (resource discovery: development
     * tree, XDG data dirs, or compiled install prefix) */
    const char *res = vt_paths_resource_dir();
    if (res) {
        char *bdir = vt_path_join(res, "themes");
        size_t bn = 0;
        char **bitems = vt_file_list_dir(bdir, &bn);
        for (size_t i = 0; i < bn; i++) {
            bool dup = false;
            for (size_t k = 0; k < n; k++)
                if (vt_streq(items[k], bitems[i])) { dup = true; break; }
            if (!dup) {
                char **grown = vt_realloc(items, (n + 1) * sizeof(char *));
                if (!grown) { vt_free(bitems[i]); continue; }
                items = grown;
                items[n++] = bitems[i];
            } else {
                vt_free(bitems[i]);
            }
        }
        vt_free(bitems);
        vt_free(bdir);
    }
    if (out_n) *out_n = n;
    return items;
}
char *vt_theme_dir(void) {
    /* XDG_DATA_HOME/vantage/themes + system path */
    return vt_strprintf("%s/vantage/themes", vt_data_dir_user());
}
char *vt_theme_path_for(const char *name) {
    if (!name) return NULL;
    /* 1. user themes (saved via vantage-theme / settings) */
    char *dir = vt_theme_dir();
    char *p = vt_path_join(dir, name);
    vt_free(dir);
    char *r = vt_path_join(p, "theme.json");
    vt_free(p);
    if (vt_path_exists(r)) return r;
    vt_free(r);
    /* 2. bundled themes via resource discovery (dev tree, XDG, or
     * compiled install prefix — see vt-paths.c) */
    char *rel = vt_strprintf("themes/%s/theme.json", name);
    char *found = vt_paths_resource_find(rel);
    vt_free(rel);
    if (found) return found;
    /* 3. legacy fallback: compiled data dir */
    return vt_strprintf(VT_DATADIR "/themes/%s/theme.json", name);
}

bool vt_theme_hex_to_rgba(const char *hex, vt_color_t *out) {
    if (!hex || !out) return false;
    if (hex[0] == '#') hex++;
    unsigned int r, g, b, a = 255;
    if (strlen(hex) == 8) {
        if (sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a) < 4) return false;
    } else if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) < 3) {
        return false;
    }
    out->r = (float)r / 255.0f;
    out->g = (float)g / 255.0f;
    out->b = (float)b / 255.0f;
    out->a = (float)a / 255.0f;
    return true;
}
char *vt_theme_rgba_to_hex(vt_color_t c) {
    uint8_t r = (uint8_t)(c.r * 255), g = (uint8_t)(c.g * 255),
            b = (uint8_t)(c.b * 255), a = (uint8_t)(c.a * 255);
    return vt_strprintf("#%02x%02x%02x%02x", r, g, b, a);
}

/* HSL-based palette generator */
static void _rgb_to_hsl(float r, float g, float b, float *h, float *s, float *l) {
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (mx + mn) / 2.0f;
    if (mx == mn) { *h = *s = 0; return; }
    float d = mx - mn;
    *s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h /= 6;
}
static void _hsl_to_rgb(float h, float s, float l, float *r, float *g, float *b) {
    if (s == 0) { *r = *g = *b = l; return; }
    float q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    float t[3] = { h + 1.0f/3, h, h - 1.0f/3 };
    for (int i = 0; i < 3; i++) {
        if (t[i] < 0) t[i] += 1;
        if (t[i] > 1) t[i] -= 1;
        if (t[i] < 1.0f/6) t[i] = p + (q - p) * 6 * t[i];
        else if (t[i] < 0.5f) t[i] = q;
        else if (t[i] < 2.0f/3) t[i] = p + (q - p) * (2.0f/3 - t[i]) * 6;
        else t[i] = p;
    }
    *r = t[0]; *g = t[1]; *b = t[2];
}

void vt_theme_palette_from_accent(const char *accent_hex, bool dark,
                                     vt_palette_t *out) {
    if (!out) return;
    vt_color_t acc; if (!vt_theme_hex_to_rgba(accent_hex, &acc)) acc = (vt_color_t){0.2f, 0.5f, 0.95f, 1};
    out->accent = acc;
    out->accent_fg = (vt_color_t){1,1,1,1};

    float h, s, l;
    _rgb_to_hsl(acc.r, acc.g, acc.b, &h, &s, &l);
    float bg_l = dark ? 0.10f : 0.96f;
    float fg_l = dark ? 0.88f : 0.10f;
    float sf_l = dark ? 0.18f : 0.94f;
    float br_l = dark ? 0.26f : 0.82f;

    _hsl_to_rgb(h, s * 0.15f, bg_l, &out->bg_dark.r, &out->bg_dark.g, &out->bg_dark.b);
    out->bg_dark.a = 1;
    _hsl_to_rgb(h, s * 0.15f, fg_l, &out->fg_dark.r, &out->fg_dark.g, &out->fg_dark.b);
    out->fg_dark.a = 1;
    _hsl_to_rgb(h, s * 0.20f, sf_l, &out->surface_dark.r, &out->surface_dark.g, &out->surface_dark.b);
    out->surface_dark.a = 1;
    _hsl_to_rgb(h, s * 0.25f, br_l, &out->border_dark.r, &out->border_dark.g, &out->border_dark.b);
    out->border_dark.a = 1;

    /* light variants */
    _hsl_to_rgb(h, s * 0.15f, 0.96f, &out->bg_light.r, &out->bg_light.g, &out->bg_light.b);
    out->bg_light.a = 1;
    _hsl_to_rgb(h, s * 0.15f, 0.10f, &out->fg_light.r, &out->fg_light.g, &out->fg_light.b);
    out->fg_light.a = 1;
    _hsl_to_rgb(h, s * 0.20f, 0.94f, &out->surface_light.r, &out->surface_light.g, &out->surface_light.b);
    out->surface_light.a = 1;
    _hsl_to_rgb(h, s * 0.25f, 0.82f, &out->border_light.r, &out->border_light.g, &out->border_light.b);
    out->border_light.a = 1;

    out->radius = 8;
    out->spacing = 8;
    snprintf(out->font_family, sizeof(out->font_family), "Sans");
    out->font_size_pt = 11;
}

void vt_theme_apply(vt_theme_t *t) {
    if (!t) return;
    vt_theme_apply_to_gtk(t);
    vt_theme_apply_to_qt6(t);
    /* Also export env so apps know */
    setenv("GTK_THEME", t->gtk_theme, 0);
    setenv("QT_QPA_PLATFORM_THEME", "vantage", 0);
}

void vt_theme_apply_to_gtk(const vt_theme_t *t) {
    if (!t) return;
    /* --- GTK3 CSS (uses @define-color references) ------------------ */
    vt_strbuilder_t g3;
    vt_strbuilder_init(&g3, 8192);
    vt_strbuilder_appendf(&g3,
        "/* Generated by the Vantage theme engine — do not edit.\n"
        " * Regenerate with: vantage-theme apply */\n"
        "@define-color accent_color %s;\n"
        "@define-color accent_bg_color %s;\n"
        "@define-color accent_fg_color #ffffff;\n"
        "@define-color window_bg_color %s;\n"
        "@define-color window_fg_color %s;\n"
        "@define-color view_bg_color %s;\n"
        "@define-color view_fg_color %s;\n"
        "@define-color headerbar_bg_color %s;\n"
        "@define-color headerbar_fg_color %s;\n"
        "@define-color sidebar_bg_color %s;\n"
        "@define-color sidebar_fg_color %s;\n"
        "@define-color card_bg_color %s;\n"
        "@define-color dialog_bg_color %s;\n"
        "@define-color dialog_fg_color %s;\n"
        "@define-color popover_bg_color %s;\n"
        "@define-color popover_fg_color %s;\n"
        "@define-color borders %s;\n"
        "@define-color destructive #e05a50;\n"
        "@define-color success #3ec46d;\n"
        "@define-color warning #e5a50a;\n\n"
        "* { font-family: '%s'; font-size: %dpt; }\n"
        "window, .background { background-color: @window_bg_color; "
        "color: @window_fg_color; }\n"
        "dialog, messagedialog { background-color: @dialog_bg_color; "
        "color: @dialog_fg_color; }\n"
        "headerbar { background-color: @headerbar_bg_color; "
        "color: @headerbar_fg_color; border-bottom: 1px solid @borders; "
        "padding: 4px 8px; }\n"
        "headerbar button { margin: 2px; }\n"
        ".sidebar, sidebar { background-color: @sidebar_bg_color; "
        "color: @sidebar_fg_color; }\n"
        "card { background-color: @card_bg_color; "
        "border: 1px solid @borders; border-radius: %dpx; }\n\n"
        "treeview.view, textview, text, entry, spinbutton { "
        "background-color: @view_bg_color; color: @view_fg_color; }\n"
        "entry, spinbutton { border: 1px solid @borders; "
        "border-radius: %dpx; padding: 4px 8px; "
        "transition: border-color 120ms ease; }\n"
        "entry:focus, spinbutton:focus { border-color: @accent_bg_color; }\n"
        "entry selection { background-color: @accent_bg_color; "
        "color: @accent_fg_color; }\n\n"
        "button { background-color: @card_bg_color; color: @window_fg_color; "
        "border: 1px solid @borders; border-radius: %dpx; "
        "padding: 5px 14px; font-weight: normal; "
        "transition: background-color 120ms ease; }\n"
        "button:hover { background-color: @accent_bg_color; "
        "color: @accent_fg_color; }\n"
        "button:active, button:checked { background-color: @accent_bg_color; "
        "color: @accent_fg_color; }\n"
        "button:disabled { color: alpha(@window_fg_color, 0.5); "
        "background-color: alpha(@card_bg_color, 0.5); }\n"
        "button.suggested-action { background-color: @accent_bg_color; "
        "color: @accent_fg_color; }\n"
        "button.destructive-action { background-color: @destructive; "
        "color: #ffffff; }\n\n"
        "checkbutton check, radiobutton radio { "
        "border: 1px solid @borders; border-radius: 4px; "
        "min-width: 14px; min-height: 14px; }\n"
        "checkbutton:checked { "
        "background-color: @accent_bg_color; color: @accent_fg_color; }\n\n"
        "menubar, .menubar { background-color: @headerbar_bg_color; "
        "color: @headerbar_fg_color; }\n"
        "menu, .menu { background-color: @popover_bg_color; "
        "color: @popover_fg_color; border: 1px solid @borders; "
        "border-radius: %dpx; padding: 4px; }\n"
        "menuitem:hover { background-color: @accent_bg_color; "
        "color: @accent_fg_color; border-radius: %dpx; }\n\n"
        "scrollbar { background-color: transparent; }\n"
        "scrollbar slider { background-color: @borders; border-radius: 5px; "
        "min-width: 8px; min-height: 24px; }\n"
        "scrollbar slider:hover { background-color: @accent_bg_color; }\n\n"
        "tooltip { background-color: @popover_bg_color; "
        "color: @popover_fg_color; border: 1px solid @borders; }\n"
        "progressbar trough { background-color: @card_bg_color; "
        "border-radius: %dpx; }\n"
        "progressbar progress { background-color: @accent_bg_color; "
        "border-radius: %dpx; }\n"
        "switch { background-color: @card_bg_color; "
        "border-radius: 14px; }\n"
        "switch:checked { background-color: @accent_bg_color; }\n"
        "scale trough { background-color: @borders; }\n"
        "scale highlight { background-color: @accent_bg_color; }\n"
        "notebook header { background-color: @headerbar_bg_color; }\n"
        "notebook tab:checked { background-color: @accent_bg_color; "
        "color: @accent_fg_color; }\n",
        t->accent_hex, t->accent_hex, t->bg_hex, t->fg_hex,
        t->bg_hex, t->fg_hex, t->surface_hex, t->fg_hex,
        t->surface_hex, t->fg_hex, t->surface_hex,
        t->bg_hex, t->fg_hex, t->surface_hex, t->fg_hex,
        t->border_hex,
        t->font_family, t->font_size_pt, t->radius, t->radius, t->radius,
        t->radius, t->radius, t->radius, t->radius);
    char *css3 = vt_strbuilder_finish(&g3, NULL);

    /* --- GTK4 CSS: @define-color is not supported; emit literal
     * colors by replacing the references. */
    static const struct { const char *name; char val[16]; } tokens[] = {
        { "@window_bg_color", {0} }, { "@window_fg_color", {0} },
        { "@view_bg_color", {0} }, { "@view_fg_color", {0} },
        { "@headerbar_bg_color", {0} }, { "@headerbar_fg_color", {0} },
        { "@sidebar_bg_color", {0} }, { "@sidebar_fg_color", {0} },
        { "@card_bg_color", {0} }, { "@dialog_bg_color", {0} },
        { "@dialog_fg_color", {0} }, { "@popover_bg_color", {0} },
        { "@popover_fg_color", {0} }, { "@borders", {0} },
        { "@accent_bg_color", {0} }, { "@accent_fg_color", {0} },
        { "@accent_color", {0} }, { "@destructive", {0} },
    };
    char vals[VT_ARRAY_SIZE(tokens)][16];
    snprintf(vals[0], 16, "%s", t->bg_hex);
    snprintf(vals[1], 16, "%s", t->fg_hex);
    snprintf(vals[2], 16, "%s", t->bg_hex);
    snprintf(vals[3], 16, "%s", t->fg_hex);
    snprintf(vals[4], 16, "%s", t->surface_hex);
    snprintf(vals[5], 16, "%s", t->fg_hex);
    snprintf(vals[6], 16, "%s", t->surface_hex);
    snprintf(vals[7], 16, "%s", t->fg_hex);
    snprintf(vals[8], 16, "%s", t->surface_hex);
    snprintf(vals[9], 16, "%s", t->bg_hex);
    snprintf(vals[10], 16, "%s", t->fg_hex);
    snprintf(vals[11], 16, "%s", t->surface_hex);
    snprintf(vals[12], 16, "%s", t->fg_hex);
    snprintf(vals[13], 16, "%s", t->border_hex);
    snprintf(vals[14], 16, "%s", t->accent_hex);
    snprintf(vals[15], 16, "%s", "#ffffff");
    snprintf(vals[16], 16, "%s", t->accent_hex);
    snprintf(vals[17], 16, "%s", "#e05a50");

    char *css4 = vt_strdup(css3);
    /* strip the @define-color block: GTK4 wants literal colors */
    char *first_rule = strstr(css4, "* { font-family");
    if (first_rule) {
        char *kept = vt_strdup(first_rule);
        vt_free(css4);
        css4 = kept;
    }
    for (size_t i = 0; i < VT_ARRAY_SIZE(tokens); i++) {
        char *replaced = vt_strreplace(css4, tokens[i].name, vals[i]);
        vt_free(css4);
        css4 = replaced;
    }

    const char *css_dirs[] = { "gtk-3.0", "gtk-4.0" };
    char *css_bodies[] = { css3, css4 };
    for (int i = 0; i < 2; i++) {
        char *cfg_dir = vt_strprintf("%s/%s", vt_config_dir(), css_dirs[i]);
        vt_file_mkdir_p(cfg_dir, 0755);
        char *css_path = vt_path_join(cfg_dir, "gtk.css");
        vt_file_write_all(css_path, css_bodies[i], strlen(css_bodies[i]));
        vt_free(css_path);
        char *ini_path = vt_path_join(cfg_dir, "settings.ini");
        FILE *fp = fopen(ini_path, "w");
        if (fp) {
            fprintf(fp,
                "[Settings]\n"
                "gtk-theme-name=%s\n"
                "gtk-icon-theme-name=%s\n"
                "gtk-font-name=%s %d\n"
                "gtk-application-prefer-dark-theme=%d\n"
                "gtk-enable-animations=%d\n"
                "gtk-xft-antialias=1\n"
                "gtk-xft-hinting=1\n",
                t->gtk_theme, t->icon_theme, t->font_family,
                t->font_size_pt, t->dark ? 1 : 0, 1);
            fclose(fp);
        }
        vt_free(ini_path);
        vt_free(cfg_dir);
    }
    vt_free(css3);
    vt_free(css4);
    setenv("GTK_ICON_THEME", t->icon_theme, 0);
    setenv("GTK_APPLICATION_PREFER_DARK", t->dark ? "1" : "0", 0);
}

void vt_theme_apply_to_qt6(const vt_theme_t *t) {
    if (!t) return;
    /* --- Qt5/Qt6 stylesheet + qt6ct-compatible conf --------------- */
    char *cfg_dir = vt_strprintf("%s/Vantage", vt_config_dir());
    vt_file_mkdir_p(cfg_dir, 0755);
    char *qss_path = vt_path_join(cfg_dir, "vantage.qss");
    FILE *fp = fopen(qss_path, "w");
    if (fp) {
        fprintf(fp,
            "/* Generated by the Vantage theme engine — do not edit.\n"
            " * Regenerate with: vantage-theme apply\n"
            " * Load with: QT_QPA_PLATFORMTHEME=vantage or qt6ct with the "
            "Vantage stylesheet */\n\n"
            "QWidget {{ background-color: %s; color: %s; "
            "font-family: '%s'; font-size: %dpt; }}\n\n"
            "/* containers */\n"
            "QMainWindow, QDialog {{ background-color: %s; }}\n"
            "QMenuBar {{ background-color: %s; color: %s; }}\n"
            "QMenuBar::item:selected {{ background-color: %s; }}\n"
            "QMenu {{ background-color: %s; color: %s; border: 1px solid %s; "
            "border-radius: %dpx; padding: 4px; }}\n"
            "QMenu::item {{ padding: 5px 24px; border-radius: %dpx; }}\n"
            "QMenu::item:selected {{ background-color: %s; color: #ffffff; }}\n\n"
            "/* controls */\n"
            "QPushButton {{ background-color: %s; color: %s; "
            "border: 1px solid %s; border-radius: %dpx; padding: 5px 14px; }}\n"
            "QPushButton:hover {{ background-color: %s; color: #ffffff; "
            "border-color: %s; }}\n"
            "QPushButton:pressed {{ background-color: %s; }}\n"
            "QPushButton:disabled {{ color: %s; background-color: %s; }}\n\n"
            "QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox {{ "
            "background-color: %s; color: %s; border: 1px solid %s; "
            "border-radius: %dpx; padding: 4px 8px; "
            "selection-background-color: %s; }}\n"
            "QLineEdit:focus, QComboBox:focus {{ border-color: %s; }}\n\n"
            "QComboBox QAbstractItemView {{ background-color: %s; "
            "color: %s; selection-background-color: %s; "
            "selection-color: #ffffff; border: 1px solid %s; }}\n\n"
            "QCheckBox, QRadioButton {{ spacing: 8px; }}\n"
            "QCheckBox::indicator, QRadioButton::indicator {{ width: 16px; "
            "height: 16px; border: 1px solid %s; border-radius: %dpx; "
            "background-color: %s; }}\n"
            "QCheckBox::indicator:checked {{ background-color: %s; "
            "border-color: %s; }}\n\n"
            "QSlider::groove:horizontal {{ height: 4px; "
            "background-color: %s; border-radius: 2px; }}\n"
            "QSlider::handle:horizontal {{ width: 14px; height: 14px; "
            "margin: -6px 0; border-radius: 7px; background-color: %s; }}\n\n"
            "QScrollBar:vertical {{ background-color: transparent; width: 10px; "
            "margin: 0; }}\n"
            "QScrollBar::handle:vertical {{ background-color: %s; "
            "border-radius: 5px; min-height: 24px; }}\n"
            "QScrollBar::handle:vertical:hover {{ background-color: %s; }}\n"
            "QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}\n\n"
            "QToolTip {{ background-color: %s; color: %s; border: 1px solid %s; "
            "padding: 4px; }}\n"
            "QProgressBar {{ background-color: %s; border-radius: %dpx; "
            "border: 1px solid %s; text-align: center; color: %s; }}\n"
            "QProgressBar::chunk {{ background-color: %s; "
            "border-radius: %dpx; }}\n\n"
            "QTreeView, QTableView, QListView {{ background-color: %s; "
            "alternate-background-color: %s; border: 1px solid %s; }}\n"
            "QHeaderView::section {{ background-color: %s; color: %s; "
            "padding: 4px 8px; border: 0 0 1px 0 solid %s; }}\n"
            "QTabWidget::pane {{ border: 1px solid %s; border-radius: %dpx; }}\n"
            "QTabBar::tab {{ background-color: %s; color: %s; padding: 6px 14px; "
            "border-top-left-radius: %dpx; border-top-right-radius: %dpx; }}\n"
            "QTabBar::tab:selected {{ background-color: %s; "
            "color: #ffffff; }}\n",
            t->bg_hex, t->fg_hex, t->font_family, t->font_size_pt,
            t->bg_hex,
            t->surface_hex, t->fg_hex, t->accent_hex,
            t->surface_hex, t->fg_hex, t->border_hex, t->radius, t->radius,
            t->accent_hex,
            t->surface_hex, t->fg_hex, t->border_hex, t->radius,
            t->accent_hex, t->accent_hex, t->bg_hex,
            t->fg_hex, t->bg_hex,
            t->bg_hex, t->fg_hex, t->border_hex, t->radius, t->accent_hex,
            t->accent_hex,
            t->surface_hex, t->fg_hex, t->accent_hex, t->border_hex,
            t->border_hex, t->radius == 9 ? 9 : 8, t->bg_hex,
            t->accent_hex, t->accent_hex,
            t->border_hex, t->accent_hex,
            t->border_hex, t->accent_hex,
            t->surface_hex, t->fg_hex, t->border_hex,
            t->surface_hex, t->radius, t->border_hex, t->fg_hex,
            t->accent_hex, t->radius,
            t->bg_hex, t->surface_hex, t->border_hex,
            t->surface_hex, t->fg_hex, t->border_hex,
            t->border_hex, t->radius,
            t->surface_hex, t->fg_hex, t->radius, t->radius,
            t->accent_hex);
        fclose(fp);
    }
    vt_free(qss_path);
    /* qt6ct-style conf so the qt6ct platform theme can consume it */
    char *conf = vt_path_join(cfg_dir, "qt6ct.conf");
    fp = fopen(conf, "w");
    if (fp) {
        fprintf(fp,
            "[Appearance]\n"
            "color_scheme_path=%s/Vantage/vantage.colors\n"
            "custom_stylesheet=true\n"
            "stylesheet=%s/Vantage/vantage.qss\n"
            "icon_theme=%s\n"
            "[Fonts]\n"
            "fixed=@\"%s,10,-1,5,50,0,0,0,0,0\"\n"
            "general=@\"%s,%d,-1,5,50,0,0,0,0,0\"\n",
            vt_config_dir(), vt_config_dir(), t->icon_theme,
            t->font_family, t->font_family, t->font_size_pt);
        fclose(fp);
    }
    vt_free(conf);
    /* KColorScheme-compatible color file */
    char *colors = vt_path_join(cfg_dir, "vantage.colors");
    fp = fopen(colors, "w");
    if (fp) {
        fprintf(fp,
            "[ColorScheme]\n"
            "Name=Vantage\n\n"
            "[Colors:Window]\n"
            "Background=%s\n"
            "Foreground=%s\n"
            "ForegroundActive=%s\n"
            "ForegroundInactive=%s\n\n"
            "[Colors:Selection]\n"
            "Background=%s\n"
            "Foreground=#ffffff\n\n"
            "[Colors:Button]\n"
            "Background=%s\n"
            "Foreground=%s\n",
            t->bg_hex, t->fg_hex, t->accent_hex, t->border_hex,
            t->accent_hex, t->surface_hex, t->fg_hex);
        fclose(fp);
    }
    vt_free(colors);
    vt_free(cfg_dir);
    setenv("QT_QPA_PLATFORMTHEME", "vantage", 0);
    setenv("QT_QPA_PLATFORM", "xcb;wayland", 0);
    char *qss_env = vt_strprintf("%s/Vantage/vantage.qss", vt_config_dir());
    setenv("VANTAGE_QSS", qss_env, 0);
    vt_free(qss_env);
}
