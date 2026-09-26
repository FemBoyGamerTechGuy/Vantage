/*
 * vt-icons.c — system icon-theme lookup (icon-theme spec subset)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * Lives in the wallpaper/image library because it reuses the same
 * image decoders (built-in PNG + optional gdk-pixbuf for svg/xpm).
 */

#define VT_LOG_DOMAIN "icons"
#include <vantage/vt-icons.h>
#include <vantage/vt-core.h>
#include <vantage/vt-paths.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>

#if defined(VT_HAVE_GDKPIXBUF)
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

/* reuse the wallpaper engine's direct PNG decoder (no gdk-pixbuf
 * needed for the most common icon format) */
bool vt_image_probe_png(const char *path);
uint8_t *vt_image_load_png(const char *path, int *out_w, int *out_h);

/* -------------------------------------------------------------- theme */
#define _MAX_THEME_CHAIN 8
#define _MAX_SUBDIRS     64

struct vt_icon_theme {
    char      *theme_name;          /* the user's theme */
    char      *chain[_MAX_THEME_CHAIN]; /* theme + inherited names */
    int        n_chain;
    char      *bases[16];           /* icon base directories */
    int        n_bases;
};

static char *_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* read "key=value" from an INI-style [Icon Theme] section; returns a
 * malloc'd value or NULL */
static char *_ini_value(const char *path, const char *section,
                        const char *key) {
    size_t len = 0;
    char *content = vt_file_read_all(path, &len);
    if (!content) return NULL;
    char *result = NULL;
    bool in_group = false;
    char *save = NULL;
    char secbuf[128], keybuf[128];
    snprintf(secbuf, sizeof(secbuf), "[%s]", section);
    snprintf(keybuf, sizeof(keybuf), "%s=", key);
    for (char *line = strtok_r(content, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *l = _trim(line);
        if (*l == '[') {
            in_group = strncmp(l, secbuf, strlen(secbuf)) == 0;
            continue;
        }
        if (!in_group) continue;
        if (vt_strstartswith(l, keybuf)) {
            result = vt_strdup(_trim(l + strlen(keybuf)));
            break;
        }
    }
    vt_free(content);
    return result;
}

static bool _have_index(const char *base, const char *theme) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s/index.theme", base, theme);
    FILE *f = fopen(p, "r");
    if (f) { fclose(f); return true; }
    return false;
}

static void _add_base(vt_icon_theme_t *t, const char *dir) {
    if (!dir || !*dir || t->n_bases >= 16) return;
    for (int i = 0; i < t->n_bases; i++)
        if (vt_streq(t->bases[i], dir)) return;
    t->bases[t->n_bases++] = vt_strdup(dir);
}

static void _collect_bases(vt_icon_theme_t *t) {
    const char *xh = getenv("XDG_DATA_HOME");
    char *user;
    if (xh && *xh) user = vt_strprintf("%s/icons", xh);
    else user = vt_strprintf("%s/.icons", vt_home_dir());
    _add_base(t, user);
    vt_free(user);
    char *xdgh = vt_strprintf("%s/.local/share/icons", vt_home_dir());
    _add_base(t, xdgh);
    vt_free(xdgh);

    const char *xd = getenv("XDG_DATA_DIRS");
    if (xd && *xd) {
        char *copy = vt_strdup(xd);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ":", &save); tok;
             tok = strtok_r(NULL, ":", &save))
            if (*tok) {
                char *b = vt_strprintf("%s/icons", tok);
                _add_base(t, b);
                vt_free(b);
            }
        vt_free(copy);
    } else {
        _add_base(t, "/usr/local/share/icons");
        _add_base(t, "/usr/share/icons");
    }
}

/* theme name from gtk settings.ini (the desktop's real setting) */
static char *_theme_from_gtk(const char *rc) {
    return _ini_value(rc, "Settings", "gtk-icon-theme-name");
}

static char *_pick_theme_name(void) {
    const char *env = getenv("VANTAGE_ICON_THEME");
    if (env && *env) return vt_strdup(env);

    char p[512];
    const char *xch = getenv("XDG_CONFIG_HOME");
    if (xch && *xch) snprintf(p, sizeof(p), "%s/gtk-3.0/settings.ini", xch);
    else snprintf(p, sizeof(p), "%s/.config/gtk-3.0/settings.ini",
                  vt_home_dir());
    char *t = _theme_from_gtk(p);
    if (t) return t;
    if (xch && *xch) snprintf(p, sizeof(p), "%s/gtk-4.0/settings.ini", xch);
    else snprintf(p, sizeof(p), "%s/.config/gtk-4.0/settings.ini",
                  vt_home_dir());
    t = _theme_from_gtk(p);
    if (t) return t;
    return vt_strdup("hicolor");
}

vt_icon_theme_t *vt_icon_theme_load(void) {
    vt_icon_theme_t *t = vt_malloc0(sizeof(*t));
    _collect_bases(t);
    t->theme_name = _pick_theme_name();
    /* strip quotes gtk sometimes writes */
    if (*t->theme_name == '"' || *t->theme_name == '\'') {
        size_t l = strlen(t->theme_name);
        if (l >= 2 && t->theme_name[l - 1] == *t->theme_name) {
            memmove(t->theme_name, t->theme_name + 1, l - 2);
            t->theme_name[l - 2] = 0;
        }
    }
    /* build the inheritance chain: user theme → Inherits → … → hicolor */
    t->chain[t->n_chain++] = vt_strdup(t->theme_name);
    for (int i = 0; i < t->n_chain && i < _MAX_THEME_CHAIN; i++) {
        for (int b = 0; b < t->n_bases; b++) {
            char idx[512];
            snprintf(idx, sizeof(idx), "%s/%s/index.theme", t->bases[b],
                     t->chain[i]);
            if (!_have_index(t->bases[b], t->chain[i])) continue;
            char *inh = _ini_value(idx, "Icon Theme", "Inherits");
            if (!inh) break;
            char *save = NULL;
            for (char *tok = strtok_r(inh, ",", &save); tok;
                 tok = strtok_r(NULL, ",", &save)) {
                tok = _trim(tok);
                if (!*tok || t->n_chain >= _MAX_THEME_CHAIN) continue;
                bool dup = false;
                for (int c = 0; c < t->n_chain; c++)
                    if (vt_streq(t->chain[c], tok)) dup = true;
                if (!dup) t->chain[t->n_chain++] = vt_strdup(tok);
            }
            vt_free(inh);
            break;
        }
    }
    /* hicolor is the guaranteed final fallback */
    bool have_hicolor = false;
    for (int c = 0; c < t->n_chain; c++)
        if (vt_streq(t->chain[c], "hicolor")) have_hicolor = true;
    if (!have_hicolor && t->n_chain < _MAX_THEME_CHAIN)
        t->chain[t->n_chain++] = vt_strdup("hicolor");

    {
        char chainbuf[256] = "";
        size_t off = 0;
        for (int c = 0; c < t->n_chain; c++)
            off += (size_t)snprintf(chainbuf + off,
                                    sizeof(chainbuf) - off, "%s%s",
                                    c ? " > " : "", t->chain[c]);
        vt_logi("icons: theme '%s' (chain %s), %d base dirs",
                t->theme_name, chainbuf, t->n_bases);
    }
    return t;
}

void vt_icon_theme_free(vt_icon_theme_t *t) {
    if (!t) return;
    for (int i = 0; i < t->n_chain; i++) vt_free(t->chain[i]);
    for (int i = 0; i < t->n_bases; i++) vt_free(t->bases[i]);
    vt_free(t->theme_name);
    vt_free(t);
}

const char *vt_icon_theme_name(const vt_icon_theme_t *t) {
    return t && t->theme_name ? t->theme_name : "hicolor";
}

/* -------------------------------------------------------------- lookup */

static const char *const _exts[] = { ".png", ".svg", ".xpm", NULL };

static int _dir_declared_size(const char *sub) {
    /* "48x48/apps" → 48; "scalable/apps" → 0 (vector) */
    int v = 0;
    for (const char *p = sub; *p && *p != 'x' && *p != '/' && *p != '@'; p++)
        if (isdigit((unsigned char)*p)) v = v * 10 + (*p - '0');
        else return 0;
    return v;
}

static bool _find_file(const char *dirpath, const char *name,
                       char *out, size_t out_n) {
    for (int e = 0; _exts[e]; e++) {
        char p[768];
        snprintf(p, sizeof(p), "%s/%s%s", dirpath, name, _exts[e]);
        if (access(p, R_OK) == 0) {
            snprintf(out, out_n, "%s", p);
            return true;
        }
    }
    return false;
}

static bool _lookup_in_theme(const vt_icon_theme_t *t, int base_i,
                             const char *theme, const char *name,
                             int size, char *out, size_t out_n) {
    char tdir[512];
    snprintf(tdir, sizeof(tdir), "%s/%s", t->bases[base_i], theme);
    DIR *d = opendir(tdir);
    if (!d) return false;
    bool found = false;
    char best_path[768];
    int best_size = 0;
    bool best_scalable = false;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        int dsz = _dir_declared_size(de->d_name);
        bool scalable = strncmp(de->d_name, "scalable", 8) == 0;
        char lvl1[600];
        snprintf(lvl1, sizeof(lvl1), "%s/%s", tdir, de->d_name);
        /* icon spec Directories are two-level ("48x48/apps"); search
         * the subdirectories of each size directory */
        DIR *sub = opendir(lvl1);
        if (!sub) continue;
        struct dirent *se;
        while ((se = readdir(sub))) {
            if (se->d_name[0] == '.') continue;
            char lvl2[768];
            snprintf(lvl2, sizeof(lvl2), "%s/%s", lvl1, se->d_name);
            char cand[900];
            if (!_find_file(lvl2, name, cand, sizeof(cand))) continue;
            if (!found) {
                snprintf(best_path, sizeof(best_path), "%s", cand);
                best_size = dsz;
                best_scalable = scalable;
                found = true;
                continue;
            }
            /* prefer the largest declared size <= request; if every
             * match is larger, prefer the smallest of those; scalable
             * beats everything (it rasterizes to any size) */
            bool better;
            if (scalable && !best_scalable) better = true;
            else if (scalable == best_scalable) {
                if (best_size == 0 && dsz > 0) better = true;
                else if (dsz > 0 && dsz <= size &&
                         (best_size > size || dsz > best_size))
                    better = true;
                else if (dsz > 0 && best_size > size && dsz < best_size)
                    better = true;
                else better = false;
            } else better = false;
            if (better) {
                snprintf(best_path, sizeof(best_path), "%s", cand);
                best_size = dsz;
                best_scalable = scalable;
            }
        }
        closedir(sub);
    }
    closedir(d);
    if (found) {
        snprintf(out, out_n, "%s", best_path);
        return true;
    }
    return false;
}

int vt_icon_theme_lookup(const vt_icon_theme_t *t, const char *icon,
                         int size, char *out, size_t out_n) {
    if (!t || !icon || !*icon || !out || !out_n) return -1;
    if (icon[0] == '/') {                       /* absolute path */
        if (access(icon, R_OK) == 0) {
            snprintf(out, out_n, "%s", icon);
            return 0;
        }
        return -1;
    }
    if (size <= 0) size = 24;

    for (int c = 0; c < t->n_chain; c++)
        for (int b = 0; b < t->n_bases; b++)
            if (_lookup_in_theme(t, b, t->chain[c], icon, size, out,
                                 out_n))
                return 0;

    /* classic pixmaps fallback */
    char p[768];
    snprintf(p, sizeof(p), "/usr/share/pixmaps/%s.png", icon);
    if (access(p, R_OK) == 0) {
        snprintf(out, out_n, "%s", p);
        return 0;
    }
    snprintf(p, sizeof(p), "/usr/share/pixmaps/%s.xpm", icon);
    if (access(p, R_OK) == 0) {
        snprintf(out, out_n, "%s", p);
        return 0;
    }
    return -1;
}

/* -------------------------------------------------------------- pixels */
int vt_icon_load_argb(const char *path, uint32_t **out_pixels,
                      int *out_w, int *out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;
    *out_pixels = NULL;
    *out_w = *out_h = 0;

    int w = 0, h = 0;
    if (vt_image_probe_png(path)) {
        uint8_t *rgba = vt_image_load_png(path, &w, &h);
        if (!rgba) return -1;
        uint32_t *px = vt_malloc(sizeof(uint32_t) * (size_t)w * h);
        for (int i = 0; i < w * h; i++)
            px[i] = 0xff000000u | ((uint32_t)rgba[i * 4] << 16) |
                    ((uint32_t)rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
        vt_free(rgba);
        *out_pixels = px;
        *out_w = w;
        *out_h = h;
        return 0;
    }
#if defined(VT_HAVE_GDKPIXBUF)
    {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
        if (!pb) return -1;
        w = gdk_pixbuf_get_width(pb);
        h = gdk_pixbuf_get_height(pb);
        int n = gdk_pixbuf_get_n_channels(pb);
        int rs = gdk_pixbuf_get_rowstride(pb);
        const uint8_t *data = gdk_pixbuf_get_pixels(pb);
        uint32_t *px = vt_malloc(sizeof(uint32_t) * (size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *p = data + (size_t)y * rs + (size_t)x * n;
                uint32_t a = n >= 4 ? p[3] : 0xff;
                px[y * w + x] = ((uint32_t)a << 24) |
                                ((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8) | p[2];
            }
        g_object_unref(pb);
        *out_pixels = px;
        *out_w = w;
        *out_h = h;
        return 0;
    }
#else
    return -1;
#endif
}
