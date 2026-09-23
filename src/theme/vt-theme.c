/*
 * vt-theme.c — Vantage theme engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
    if (out_n) *out_n = n;
    return items;
}
char *vt_theme_dir(void) {
    /* XDG_DATA_HOME/vantage/themes + system path */
    return vt_strprintf("%s/vantage/themes", vt_data_dir_user());
}
char *vt_theme_path_for(const char *name) {
    if (!name) return NULL;
    char *dir = vt_theme_dir();
    char *p = vt_path_join(dir, name);
    vt_free(dir);
    char *r = vt_path_join(p, "theme.json");
    vt_free(p);
    if (vt_path_exists(r)) return r;
    vt_free(r);
    /* try system path */
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
    out->r = r / 255.0f;
    out->g = g / 255.0f;
    out->b = b / 255.0f;
    out->a = a / 255.0f;
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
    /* Generate GTK CSS into ~/.config/gtk-3.0 + gtk-4.0 */
    char *cfg_dir = vt_strprintf("%s/gtk-3.0", vt_config_dir());
    vt_file_mkdir_p(cfg_dir, 0755);
    char *css_path = vt_path_join(cfg_dir, "gtk.css");
    FILE *fp = fopen(css_path, "w");
    if (fp) {
        fprintf(fp,
            "/* Generated by Vantage theme engine. Do not edit. */\n"
            "@define-color accent %s;\n"
            "@define-color bg %s;\n"
            "@define-color fg %s;\n"
            "@define-color surface %s;\n"
            "@define-color border %s;\n"
            "* { font-family: %s; font-size: %dpt; }\n"
            "window { background-color: @bg; color: @fg; }\n"
            "button { background-color: @surface; color: @fg; border-radius: %dpx; border: 1px solid @border; }\n"
            "button:hover { background-color: @accent; color: white; }\n",
            t->accent_hex, t->bg_hex, t->fg_hex, t->surface_hex, t->border_hex,
            t->font_family, t->font_size_pt, t->radius);
        fclose(fp);
    }
    vt_free(css_path);
    /* gtk-4.0 shares the same CSS */
    char *cfg4 = vt_strprintf("%s/gtk-4.0", vt_config_dir());
    vt_file_mkdir_p(cfg4, 0755);
    char *css4 = vt_path_join(cfg4, "gtk.css");
    vt_file_write_all(css4,
        "/* Generated by Vantage */\n", strlen("/* Generated by Vantage */\n"));
    /* write the same content */
    fp = fopen(css4, "w");
    if (fp) {
        fprintf(fp,
            "@define-color accent %s;\n@define-color bg %s;\n",
            t->accent_hex, t->bg_hex);
        fclose(fp);
    }
    vt_free(css4); vt_free(cfg4); vt_free(cfg_dir);
    /* icons */
    setenv("GTK_ICON_THEME", t->icon_theme, 0);
    setenv("GTK_APPLICATION_PREFER_DARK", t->dark ? "1" : "0", 0);
}

void vt_theme_apply_to_qt6(const vt_theme_t *t) {
    if (!t) return;
    /* Generate Qt6 QSS + set QT_QPA_PLATFORMTHEME */
    char *cfg_dir = vt_strprintf("%s/Vantage", vt_config_dir());
    vt_file_mkdir_p(cfg_dir, 0755);
    char *qss_path = vt_path_join(cfg_dir, "vantage.qss");
    FILE *fp = fopen(qss_path, "w");
    if (fp) {
        fprintf(fp,
            "/* Generated by Vantage theme engine */\n"
            "QWidget { background-color: %s; color: %s; font-family: %s; font-size: %dpt; }\n"
            "QPushButton { background-color: %s; color: %s; border: 1px solid %s; border-radius: %dpx; padding: 4px 12px; }\n"
            "QPushButton:hover { background-color: %s; color: white; }\n",
            t->bg_hex, t->fg_hex, t->font_family, t->font_size_pt,
            t->surface_hex, t->fg_hex, t->border_hex, t->radius,
            t->accent_hex);
        fclose(fp);
    }
    vt_free(qss_path); vt_free(cfg_dir);
    /* Tell Qt5/Qt6 apps to use Vantage platform theme */
    setenv("QT_QPA_PLATFORMTHEME", "vantage", 0);
    setenv("QT_QPA_PLATFORM", "xcb;wayland", 0);
    /* Point to the QSS so a future vantage-qt6 plugin picks it up */
    char *qss_env = vt_strprintf("%s/Vantage/vantage.qss", vt_config_dir());
    setenv("VANTAGE_QSS", qss_env, 0);
    vt_free(qss_env);
}
