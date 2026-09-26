/*
 * vt-wl-panel.c — Compositor-side panel for the native Wayland backend
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * A REAL panel drawn by the compositor into the scanout framebuffer —
 * not a client, not XWayland, and not placeholder blocks:
 *
 *   LEFT:   Vantage start button (categorized .desktop application menu
 *           with a Quit Session entry), window list (xdg toplevels)
 *   RIGHT:  workspace buttons, clock (calendar popup), username +
 *           session menu (Lock/Suspend/Switch/Log Out/Reboot/Shutdown)
 *
 * Text is rasterized with FreeType + fontconfig (VT_HAVE_FREETYPE); the
 * glyph cache keeps common codepoints as ARGB bitmaps. Without FreeType
 * the panel still draws its buttons and glyphs, and labels fall back to
 * a compact built-in 5x7 font so nothing is a blank placeholder.
 *
 * Input: the compositor routes pointer presses through
 * vt_wl_panel_pointer() BEFORE any client surface — clicks in the bar or
 * in an open menu never leak to clients.
 */

#define VT_LOG_DOMAIN "wl-panel"
#include <vantage/vt-core.h>
#include "vt-wl-panel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <math.h>
#include <pwd.h>
#include <unistd.h>

#if defined(VT_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <fontconfig/fontconfig.h>
#endif

/* ------------------------------------------------------------ palette */
#define _C_BG        0xff22262b   /* panel bar background            */
#define _C_BG2       0xff1a1a1a   /* desktop background              */
#define _C_ACCENT    0xff4f9adc
#define _C_ACCENT_HI 0xff6faae8
#define _C_FG        0xffeceef0
#define _C_DIM       0xff909399
#define _C_BTN       0xff26282e
#define _C_BTN_HI    0xff393e48
#define _C_WARN      0xffe07a50
#define _C_MENU_BG   0xf01a1c22

/* ------------------------------------------------------------ text */
#if defined(VT_HAVE_FREETYPE)
typedef struct {
    uint32_t cp;
    int w, h, xoff, yoff, adv;
    uint32_t *bgra;          /* ARGB premultiplied-ish (alpha blend here) */
} _glyph_t;

static FT_Library  _ft_lib = NULL;
static FT_Face     _ft_face = NULL;
static vt_vec_t    _glyph_cache;      /* _glyph_t */
static bool        _ft_tried = false;

static bool _ft_init(void) {
    if (_ft_face) return true;
    if (_ft_tried) return false;
    _ft_tried = true;
    if (FT_Init_FreeType(&_ft_lib) != 0) return false;
    /* pick a real sans font via fontconfig */
    const char *path = NULL;
    FcConfig *fc = FcInitLoadConfigAndFonts();
    if (fc) {
        FcPattern *pat = FcNameParse((const FcChar8 *)"sans");
        FcConfigSubstitute(fc, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res = FcResultNoMatch;
        FcPattern *mat = FcFontMatch(fc, pat, &res);
        if (mat) {
            FcChar8 *f = NULL;
            if (FcPatternGetString(mat, FC_FILE, 0, &f) == FcResultMatch)
                path = vt_strdup((const char *)f);
            FcPatternDestroy(mat);
        }
        FcPatternDestroy(pat);
        FcConfigDestroy(fc);
    }
    if (!path) {
        static const char *const tries[] = {
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            NULL,
        };
        for (int i = 0; !path && tries[i]; i++)
            if (access(tries[i], R_OK) == 0) path = tries[i];
    }
    if (!path) {
        vt_logw("wl-panel: no sans font found (fontconfig) — built-in "
                "fallback glyphs");
        return false;
    }
    if (FT_New_Face(_ft_lib, path, 0, &_ft_face) != 0) {
        vt_logw("wl-panel: cannot load font %s", path);
        return false;
    }
    FT_Set_Pixel_Sizes(_ft_face, 0, 13);
    vt_vec_init(&_glyph_cache, sizeof(_glyph_t), 128);
    vt_logi("wl-panel: font %s (freetype)", path);
    return true;
}

static _glyph_t *_glyph_get(uint32_t cp) {
    for (size_t i = 0; i < _glyph_cache.size; i++) {
        _glyph_t *g = vt_vec_at(&_glyph_cache, i);
        if (g->cp == cp) return g;
    }
    if (FT_Load_Char(_ft_face, cp, FT_LOAD_RENDER) != 0) return NULL;
    FT_GlyphSlot sl = _ft_face->glyph;
    _glyph_t g = { .cp = cp, .w = (int)sl->bitmap.width,
                   .h = (int)sl->bitmap.rows, .xoff = sl->bitmap_left,
                   .yoff = -sl->bitmap_top,
                   .adv = (int)(sl->advance.x >> 6), .bgra = NULL };
    if (g.w > 0 && g.h > 0) {
        g.bgra = vt_malloc(sizeof(uint32_t) * (size_t)g.w * (size_t)g.h);
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                unsigned char a = sl->bitmap.buffer[y * g.w + x];
                g.bgra[y * g.w + x] = ((uint32_t)a) << 24;
            }
    }
    vt_vec_push(&_glyph_cache, &g);
    return vt_vec_at(&_glyph_cache, _glyph_cache.size - 1);
}
#endif /* VT_HAVE_FREETYPE */

/* blend one ARGB pixel (src alpha) over dst (XRGB) */
static inline uint32_t _blend_px(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0) return dst;
    if (a == 0xff) return 0xff000000u | (src & 0xffffffu);
    uint32_t rb = ((src & 0x00ff00ff) * a + (dst & 0x00ff00ff) * (255 - a))
                  / 255;
    uint32_t gg = ((src & 0x0000ff00) * a + (dst & 0x0000ff00) * (255 - a))
                  / 255;
    return 0xff000000u | (rb & 0x00ff00ff) | (gg & 0x0000ff00);
}

static void _fill_rect(uint32_t *fb, int fbw, int fbh, int x, int y, int w,
                       int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fbw) w = fbw - x;
    if (y + h > fbh) h = fbh - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fb[yy * fbw + xx] = _blend_px(fb[yy * fbw + xx], argb);
}

static void _hline(uint32_t *fb, int fbw, int fbh, int x, int y, int w,
                   uint32_t argb) {
    _fill_rect(fb, fbw, fbh, x, y, w, 1, argb);
}

/* UTF-8 decode helper: returns codepoint, advances *s */
static uint32_t _utf8_next(const char **s) {
    const unsigned char *u = (const unsigned char *)*s;
    uint32_t cp = *u++;
    if (cp >= 0xf0 && (cp & 0xf8) == 0xf0 && *u) {
        cp = ((cp & 0x07) << 18) | ((u[0] & 0x3f) << 12) |
             ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
        u += 3;
    } else if (cp >= 0xe0 && (cp & 0xf0) == 0xe0 && *u) {
        cp = ((cp & 0x0f) << 12) | ((u[0] & 0x3f) << 6) | (u[1] & 0x3f);
        u += 2;
    } else if (cp >= 0xc0 && (cp & 0xe0) == 0xc0 && *u) {
        cp = ((cp & 0x1f) << 6) | (u[0] & 0x3f);
        u += 1;
    }
    *s = (const char *)u;
    return cp;
}

/* Compact built-in 5x7 ASCII font — ALWAYS compiled in: it is the
 * fallback when FreeType has no match, so labels never vanish. */
static const uint8_t _f5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},
    {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14},
    {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},
    {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x05,0x03,0x00,0x00},
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x7F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},
};
/* draw text; returns pixel width consumed */
static int _text_draw(uint32_t *fb, int fbw, int fbh, int x, int y,
                      const char *utf8, uint32_t argb) {
    if (!utf8) return 0;
    int cx = x;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = _utf8_next(&p);
        if (cp == '\n') break;
#if defined(VT_HAVE_FREETYPE)
        if (!_ft_init()) { /* fall through to builtin below */ }
        if (_ft_face) {
            _glyph_t *g = _glyph_get(cp);
            if (g) {
                if (g->bgra)
                    for (int gy = 0; gy < g->h; gy++)
                        for (int gx = 0; gx < g->w; gx++) {
                            uint32_t px = g->bgra[gy * g->w + gx];
                            uint32_t src = (px & 0xff000000u) |
                                           (argb & 0x00ffffffu);
                            int dx = cx + g->xoff + gx, dy = y + 8 + g->yoff + gy;
                            if (dx < 0 || dy < 0 || dx >= fbw || dy >= fbh)
                                continue;
                            fb[dy * fbw + dx] = _blend_px(fb[dy * fbw + dx],
                                                          src);
                        }
                cx += g->adv;
                continue;
            }
            continue;
        }
#endif
        /* built-in 5x7 */
        if (cp >= 32 && cp < 127) {
            const uint8_t *col = _f5x7[cp - 32];
            for (int gy = 0; gy < 7; gy++)
                for (int gx = 0; gx < 5; gx++)
                    if (col[gx] & (1u << (6 - gy))) {
                        int dx = cx + gx, dy = y + 3 + gy;
                        if (dx < 0 || dy < 0 || dx >= fbw || dy >= fbh)
                            continue;
                        fb[dy * fbw + dx] = argb;
                    }
            cx += 6;
        } else {
            cx += 6;
        }
    }
    return cx - x;
}

static int _text_width(const char *utf8) {
    if (!utf8) return 0;
    int w = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = _utf8_next(&p);
#if defined(VT_HAVE_FREETYPE)
        if (_ft_init() && _ft_face) {
            _glyph_t *g = _glyph_get(cp);
            w += g ? g->adv : 6;
            continue;
        }
#endif
        w += 6;
        (void)cp;
    }
    return w;
}

/* rounded rectangle fill (scanline approximation) */
static void _round_rect(uint32_t *fb, int fbw, int fbh, int x, int y, int w,
                        int h, int r, uint32_t argb) {
    if (r <= 0 || 2 * r > w || 2 * r > h) {
        _fill_rect(fb, fbw, fbh, x, y, w, h, argb);
        return;
    }
    _fill_rect(fb, fbw, fbh, x + r, y, w - 2 * r, h, argb);
    _fill_rect(fb, fbw, fbh, x, y + r, w, h - 2 * r, argb);
    for (int i = 0; i < r; i++) {
        int t = (int)((double)r * sqrt(1.0 -
                    ((double)(r - i) * (r - i)) / ((double)r * r)));
        _hline(fb, fbw, fbh, x + r - t, y + r - i, t, argb);
        _hline(fb, fbw, fbh, x + w - r, y + r - i, t, argb);
        _hline(fb, fbw, fbh, x + r - t, y + h - r + i, t, argb);
        _hline(fb, fbw, fbh, x + w - r, y + h - r + i, t, argb);
    }
}

/* ------------------------------------------------------- .desktop apps */
typedef struct {
    char *name;
    char *exec;
    char *category;
} _app_t;

static const struct { const char *key; const char *label; } _wl_cats[] = {
    { "Utility", "Accessories" }, { "Development", "Development" },
    { "Education", "Education" }, { "Science", "Education" },
    { "Game", "Games" }, { "Graphics", "Graphics" },
    { "Network", "Internet" }, { "AudioVideo", "Multimedia" },
    { "Audio", "Multimedia" }, { "Video", "Multimedia" },
    { "Office", "Office" }, { "System", "System" },
    { "Settings", "System" }, { "Terminal", "Utilities" },
};
static const char *const _wl_cat_order[] = {
    "Accessories", "Development", "Education", "Games", "Graphics",
    "Internet", "Multimedia", "Office", "System", "Utilities", "Other",
};
#define _WL_N_CATS ((int)(sizeof(_wl_cat_order) / sizeof(_wl_cat_order[0])))

static const char *_wl_category_of(const char *cats) {
    if (!cats || !*cats) return "Other";
    for (size_t i = 0; i < sizeof(_wl_cats) / sizeof(_wl_cats[0]); i++)
        if (strstr(cats, _wl_cats[i].key)) return _wl_cats[i].label;
    return "Other";
}

static void _wl_apps_load(vt_vec_t *apps) {
    static const char *const dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
    };
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (!vt_strendswith(de->d_name, ".desktop")) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], de->d_name);
            size_t len = 0;
            char *content = vt_file_read_all(path, &len);
            if (!content) continue;
            char *name = NULL, *exec = NULL, *cats = NULL, *nod = NULL,
                 *onlyin = NULL;
            char *save = NULL;
            for (char *line = strtok_r(content, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                if (name && exec) break;
                if (!name && vt_strstartswith(line, "Name="))
                    name = vt_strdup(line + 5);
                else if (!exec && vt_strstartswith(line, "Exec=")) {
                    exec = vt_strdup(line + 5);
                    char *pc = strstr(exec, "%");
                    if (pc) *pc = 0;
                    vt_strtrim(exec);
                }
                else if (!cats && vt_strstartswith(line, "Categories="))
                    cats = vt_strdup(line + 11);
                else if (vt_strstartswith(line, "NoDisplay=true"))
                    nod = vt_strdup("1");
                else if (vt_strstartswith(line, "OnlyShowIn="))
                    onlyin = vt_strdup(line + 12);
            }
            vt_free(content);
            if (nod || !name || !exec ||
                (onlyin && !strstr(onlyin, "Vantage") &&
                 !strstr(onlyin, "GNOME") && !strstr(onlyin, "XFCE"))) {
                vt_free(name); vt_free(exec); vt_free(cats);
                vt_free(nod); vt_free(onlyin);
                continue;
            }
            _app_t a = { .name = name, .exec = exec,
                         .category = vt_strdup(_wl_category_of(cats)) };
            vt_vec_push(apps, &a);
            vt_free(cats);
            vt_free(nod);
            vt_free(onlyin);
        }
        closedir(dir);
    }
    /* sort by name */
    for (size_t i = 0; i + 1 < apps->size; i++)
        for (size_t j = i + 1; j < apps->size; j++) {
            _app_t *a = vt_vec_at(apps, i), *b = vt_vec_at(apps, j);
            if (strcasecmp(a->name, b->name) > 0) {
                _app_t t = *a; *a = *b; *b = t;
            }
        }
    vt_logi("wl-panel: %zu applications indexed", apps->size);
}

static int _wl_cat_count(const vt_vec_t *apps, const char *cat) {
    int n = 0;
    for (size_t i = 0; i < apps->size; i++) {
        _app_t *a = vt_vec_at((vt_vec_t *)apps, i);
        if (vt_streq(a->category, cat)) n++;
    }
    return n;
}

/* ------------------------------------------------------------ windows */
typedef struct {
    uint64_t id;
    char *title;
    bool focused;
} _wlwin_t;

/* ------------------------------------------------------------ panel */
typedef enum {
    _WL_MENU_NONE = 0,
    _WL_MENU_APPS,
    _WL_MENU_SESSION,
    _WL_MENU_CAL,
} _menu_kind_t;

struct vt_wl_panel {
    int  w;                /* screen width  */
    int  bar_h;            /* bar height    */
    vt_vec_t apps;         /* _app_t        */
    vt_vec_t wins;         /* _wlwin_t      */
    int  ws_count, ws_cur;
    char username[48];
    _menu_kind_t menu;
    int  menu_sel;         /* hovered row */
    int  app_cat;          /* selected category */
    int  cal_year, cal_mon;
    char *status;          /* session-action feedback line */
    vt_wl_panel_cbs_t cb;
    void *cb_ud;
    bool apps_loaded;
};

vt_wl_panel_t *vt_wl_panel_create(int width, int bar_height) {
    vt_wl_panel_t *p = vt_malloc0(sizeof(*p));
    p->w = width;
    p->bar_h = bar_height > 24 ? bar_height : 32;
    vt_vec_init(&p->apps, sizeof(_app_t), 32);
    vt_vec_init(&p->wins, sizeof(_wlwin_t), 8);
    p->ws_count = 4;
    p->ws_cur = 0;
    p->menu = _WL_MENU_NONE;
    p->menu_sel = -1;
    p->app_cat = 0;
    const char *n = getenv("USER");
    if (!n || !*n) {
        struct passwd *pw = getpwuid(getuid());
        n = pw ? pw->pw_name : "user";
    }
    snprintf(p->username, sizeof(p->username), "%s", n);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    p->cal_year = tm.tm_year + 1900;
    p->cal_mon = tm.tm_mon;
    return p;
}

void vt_wl_panel_destroy(vt_wl_panel_t *p) {
    if (!p) return;
    for (size_t i = 0; i < p->apps.size; i++) {
        _app_t *a = vt_vec_at(&p->apps, i);
        vt_free(a->name); vt_free(a->exec); vt_free(a->category);
    }
    vt_vec_fini(&p->apps);
    for (size_t i = 0; i < p->wins.size; i++) {
        _wlwin_t *w = vt_vec_at(&p->wins, i);
        vt_free(w->title);
    }
    vt_vec_fini(&p->wins);
    vt_free(p->status);
    vt_free(p);
}

void vt_wl_panel_resize(vt_wl_panel_t *p, int width) {
    if (p) p->w = width;
}

int vt_wl_panel_height(const vt_wl_panel_t *p) {
    return p ? p->bar_h : 32;
}

void vt_wl_panel_set_callbacks(vt_wl_panel_t *p,
                               const vt_wl_panel_cbs_t *cbs, void *ud) {
    if (!p) return;
    if (cbs) p->cb = *cbs;
    p->cb_ud = ud;
}

void vt_wl_panel_set_workspaces(vt_wl_panel_t *p, int count, int cur) {
    if (!p) return;
    if (count > 0 && count <= 16) p->ws_count = count;
    if (cur >= 0 && cur < p->ws_count) p->ws_cur = cur;
}

void vt_wl_panel_set_windows(vt_wl_panel_t *p, const vt_wl_panel_win_t *wins,
                             size_t n) {
    if (!p) return;
    for (size_t i = 0; i < p->wins.size; i++) {
        _wlwin_t *w = vt_vec_at(&p->wins, i);
        vt_free(w->title);
    }
    vt_vec_clear(&p->wins);
    for (size_t i = 0; i < n && i < 16; i++) {
        _wlwin_t w = { .id = wins[i].id,
                       .title = vt_strdup(wins[i].title ? wins[i].title : ""),
                       .focused = wins[i].focused };
        vt_vec_push(&p->wins, &w);
    }
}

/* ------------------------------------------------------------- layout */
typedef struct { int x, w; const char *tip; } _seg_t;

static _seg_t _seg_start(const vt_wl_panel_t *p) {
    return (_seg_t){ .x = 8, .w = 92 };
}
/* task buttons occupy the middle; right side laid out right-to-left */
static _seg_t _seg_user(const vt_wl_panel_t *p) {
    int w = _text_width(p->username) + 34;
    return (_seg_t){ .x = p->w - w - 8, .w = w };
}
static _seg_t _seg_clock(const vt_wl_panel_t *p) {
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%a %d %b %H:%M", &tm);
    return (_seg_t){ .x = 0, .w = _text_width(buf) + 14 };
}

/* right edge coordinates: user | clock | ws */
static int _rx_clock(const vt_wl_panel_t *p) { return _seg_user(p).x - 8; }
static int _rx_ws(const vt_wl_panel_t *p)    { return _rx_clock(p) - _seg_clock(p).w - 8; }

/* ------------------------------------------------------------- actions */
static void _panel_action(vt_wl_panel_t *p, int act);

static void _panel_spawn(const char *exec) {
    if (!exec || !*exec) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", exec, (char *)NULL);
        _exit(127);
    }
    vt_logi("wl-panel: launched '%s' (pid %d)", exec, (int)pid);
}

/* ------------------------------------------------------------- paint */
static void _paint_apps_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                             int fbh);
static void _paint_session_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                                int fbh);
static void _paint_calendar(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                            int fbh);

void vt_wl_panel_paint(vt_wl_panel_t *p, uint32_t *fb, int fbw, int fbh) {
    if (!p) return;
    int h = p->bar_h;
    _fill_rect(fb, fbw, fbh, 0, 0, fbw, h, 0xff23262b);
    _hline(fb, fbw, fbh, 0, h - 1, fbw, 0xff393e48);

    /* --- start button --- */
    _seg_t s = _seg_start(p);
    _round_rect(fb, fbw, fbh, s.x, 5, 84, h - 10, 8, _C_ACCENT);
    uint32_t fg = 0xffffffff;
    for (int i = 0; i < 6; i++) {
        _fill_rect(fb, fbw, fbh, s.x + 10 + i, 10 + i, 2, 2, fg);
        _fill_rect(fb, fbw, fbh, s.x + 26 - i, 10 + i, 2, 2, fg);
    }
    _fill_rect(fb, fbw, fbh, s.x + 17, 20, 2, 2, fg);
    _text_draw(fb, fbw, fbh, s.x + 36, h / 2 - 7, "Vantage", 0xffffffff);

    /* --- task buttons (xdg toplevels) --- */
    int tx = s.x + s.w + 10;
    int tw_end = _rx_ws(p) - 10;
    for (size_t i = 0; i < p->wins.size; i++) {
        _wlwin_t *w = vt_vec_at(&p->wins, i);
        int bw = 140;
        if (tx + bw > tw_end) break;
        uint32_t bg = w->focused ? _C_BTN_HI : _C_BTN;
        _round_rect(fb, fbw, fbh, tx, 4, bw - 6, h - 8, 6, bg);
        if (w->focused)
            _hline(fb, fbw, fbh, tx + 4, 4, bw - 14, _C_ACCENT_HI);
        const char *label = w->title ? w->title : "";
        int lw = _text_width(label);
        if (lw > bw - 26) {
            char trunc[64];
            snprintf(trunc, sizeof(trunc), "%s", label);
            while (strlen(trunc) > 4 && _text_width(trunc) > bw - 32)
                trunc[strlen(trunc) - 1] = 0;
            _text_draw(fb, fbw, fbh, tx + 8, h / 2 - 7, trunc,
                       w->focused ? _C_FG : _C_DIM);
        } else {
            _text_draw(fb, fbw, fbh, tx + 8, h / 2 - 7, label,
                       w->focused ? _C_FG : _C_DIM);
        }
        tx += bw;
    }

    /* --- workspace buttons --- */
    int wx = _rx_ws(p);
    for (int i = 0; i < p->ws_count; i++) {
        int x = wx + i * 26;
        bool active = (i == p->ws_cur);
        _round_rect(fb, fbw, fbh, x, 5, 22, h - 10, 7,
                    active ? _C_ACCENT_HI : _C_BTN);
        if (active) {
            _hline(fb, fbw, fbh, x + 3, 6, 16, 0xffffffff);
            _hline(fb, fbw, fbh, x + 3, h - 7, 16, 0xffffffff);
        }
        char n[12];
        snprintf(n, sizeof(n), "%d", i + 1);
        int nw = _text_width(n);
        _text_draw(fb, fbw, fbh, x + (22 - nw) / 2, h / 2 - 7, n,
                   active ? 0xffffffff : _C_DIM);
    }

    /* --- clock --- */
    _seg_t c = _seg_clock(p);
    int cx = _rx_clock(p) - c.w;
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%a %d %b %H:%M", &tm);
    _text_draw(fb, fbw, fbh, cx + 6, h / 2 - 7, buf, _C_FG);

    /* --- username + caret --- */
    _seg_t u = _seg_user(p);
    _round_rect(fb, fbw, fbh, u.x + 6, 4, h - 8, h - 8, (h - 8) / 2,
                _C_ACCENT);
    _fill_rect(fb, fbw, fbh, u.x + 6 + (h - 8) / 2 - 2, 10, 4, 3, fg);
    _fill_rect(fb, fbw, fbh, u.x + 6 + (h - 8) / 2 - 4, 15, 8, 3, fg);
    _text_draw(fb, fbw, fbh, u.x + h + 2, h / 2 - 7, p->username, _C_FG);
    for (int i = 0; i < 3; i++)
        _fill_rect(fb, fbw, fbh, u.x + u.w - 16 + i * 4, h / 2 - 1 + i, 2, 2,
                   _C_DIM);

    /* --- open menus on top --- */
    switch (p->menu) {
    case _WL_MENU_APPS:    _paint_apps_menu(p, fb, fbw, fbh); break;
    case _WL_MENU_SESSION: _paint_session_menu(p, fb, fbw, fbh); break;
    case _WL_MENU_CAL:     _paint_calendar(p, fb, fbw, fbh); break;
    default: break;
    }
}

#define _WL_MENU_ROW 26

static void _paint_apps_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                             int fbh) {
    if (!p->apps_loaded) {
        _wl_apps_load(&p->apps);
        p->apps_loaded = true;
    }
    const int cat_w = 130, rows = 14;
    const int w = 500, h = rows * _WL_MENU_ROW + 8;
    int x = 8, y = p->bar_h + 4;
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);
    _fill_rect(fb, fbw, fbh, x + cat_w, y, 1, h, 0xff393e48);

    for (int i = 0; i < _WL_N_CATS; i++) {
        const char *cat = _wl_cat_order[i];
        int n = _wl_cat_count(&p->apps, cat);
        if (n == 0 && i != _WL_N_CATS - 1) continue;
        int ry = y + 4 + i * _WL_MENU_ROW;
        if (ry + _WL_MENU_ROW > y + h) break;
        bool active = (i == p->app_cat);
        if (active)
            _fill_rect(fb, fbw, fbh, x + 2, ry, cat_w - 4, _WL_MENU_ROW - 2,
                       _C_ACCENT);
        _text_draw(fb, fbw, fbh, x + 10, ry + 6, cat,
                   active ? 0xffffffff : _C_FG);
        char badge[8];
        snprintf(badge, sizeof(badge), "%d", n);
        _text_draw(fb, fbw, fbh, x + cat_w - 10 - _text_width(badge),
                   ry + 6, badge, _C_DIM);
    }

    const char *cat = _wl_cat_order[p->app_cat];
    int row = 0;
    for (size_t i = 0; i < p->apps.size && row < rows; i++) {
        _app_t *a = vt_vec_at(&p->apps, i);
        if (!vt_streq(a->category, cat)) continue;
        int ry = y + 4 + row * _WL_MENU_ROW;
        bool sel = (row == p->menu_sel);
        if (sel)
            _fill_rect(fb, fbw, fbh, x + cat_w + 2, ry, w - cat_w - 4,
                       _WL_MENU_ROW - 2, _C_BTN_HI);
        _text_draw(fb, fbw, fbh, x + cat_w + 10, ry + 6, a->name, _C_FG);
        row++;
    }
    if (row == 0)
        _text_draw(fb, fbw, fbh, x + cat_w + 10, y + 10,
                   "(no applications)", _C_DIM);
    /* Quit Session row pinned at the bottom of the category pane */
    int qy = y + h - _WL_MENU_ROW - 4;
    _fill_rect(fb, fbw, fbh, x + 2, qy, cat_w - 4, _WL_MENU_ROW - 2,
               _C_BTN);
    _text_draw(fb, fbw, fbh, x + 10, qy + 6, "Quit Session", _C_WARN);
}

enum {
    _WL_UA_LOCK = 0, _WL_UA_SUSPEND, _WL_UA_SWITCH, _WL_UA_LOGOUT,
    _WL_UA_REBOOT, _WL_UA_SHUTDOWN, _WL_UA_EXIT, _WL_UA_COUNT
};
static const char *const _wl_user_actions[_WL_UA_COUNT] = {
    "Lock Screen", "Suspend", "Switch User", "Log Out", "Reboot",
    "Shutdown", "Exit Session",
};

static void _paint_session_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                                int fbh) {
    const int w = 200;
    int h = _WL_UA_COUNT * _WL_MENU_ROW + 8 + (p->status ? _WL_MENU_ROW : 0);
    _seg_t u = _seg_user(p);
    int x = u.x + u.w - w;
    if (x < 4) x = 4;
    int y = p->bar_h + 4;
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);
    for (int i = 0; i < _WL_UA_COUNT; i++) {
        int ry = y + 4 + i * _WL_MENU_ROW;
        bool sel = (i == p->menu_sel);
        if (sel)
            _fill_rect(fb, fbw, fbh, x + 2, ry, w - 4, _WL_MENU_ROW - 2,
                       _C_ACCENT);
        uint32_t col = (i >= _WL_UA_REBOOT && !sel) ? _C_WARN : _C_FG;
        _text_draw(fb, fbw, fbh, x + 10, ry + 6, _wl_user_actions[i], col);
    }
    if (p->status)
        _text_draw(fb, fbw, fbh, x + 10, y + 4 + _WL_UA_COUNT * _WL_MENU_ROW + 6,
                   p->status, _C_DIM);
}

static void _paint_calendar(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                            int fbh) {
    const int cell = 28, w = 7 * cell + 16, h = 6 * cell + 60;
    _seg_t c = _seg_clock(p);
    int x = _rx_clock(p) - c.w;
    if (x < 4) x = 4;
    int y = p->bar_h + 4;
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);
    static const char *const mon[] = { "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    char head[64];
    snprintf(head, sizeof(head), "%s %d", mon[p->cal_mon], p->cal_year);
    _text_draw(fb, fbw, fbh, x + 44, y + 6, head, _C_FG);
    _text_draw(fb, fbw, fbh, x + 12, y + 6, "<", _C_ACCENT_HI);
    _text_draw(fb, fbw, fbh, x + w - 18, y + 6, ">", _C_ACCENT_HI);
    static const char *const wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa",
                                      "Su" };
    for (int i = 0; i < 7; i++)
        _text_draw(fb, fbw, fbh, x + 8 + i * cell + 6, y + 30, wd[i],
                   _C_DIM);
    struct tm first = { .tm_year = p->cal_year - 1900,
                        .tm_mon = p->cal_mon, .tm_mday = 1 };
    mktime(&first);
    int lead = (first.tm_wday + 6) % 7;
    int days = 31 - (((p->cal_mon == 1 ? 2 : p->cal_mon) + 2) % 7 + 2) % 7
               - ((p->cal_mon == 1 && (p->cal_year % 4 == 0 &&
                 (p->cal_year % 100 != 0 || p->cal_year % 400 == 0))) ? 0 : 0);
    /* robust month length via mktime probing */
    days = 31;
    while (days > 28) {
        struct tm probe = { .tm_year = p->cal_year - 1900,
                            .tm_mon = p->cal_mon, .tm_mday = days };
        time_t t = mktime(&probe);
        if (t == (time_t)-1 || probe.tm_mon != p->cal_mon) days--;
        else break;
    }
    time_t now = time(NULL);
    struct tm tmn;
    localtime_r(&now, &tmn);
    for (int d = 0; d < days; d++) {
        int col = (lead + d) % 7, row = (lead + d) / 7;
        int cx = x + 8 + col * cell, cy = y + 56 + row * cell;
        if (cy + cell > y + h) break;
        bool today = (p->cal_year == tmn.tm_year + 1900 &&
                      p->cal_mon == tmn.tm_mon && d + 1 == tmn.tm_mday);
        if (today)
            _round_rect(fb, fbw, fbh, cx, cy, cell - 4, cell - 4, 6,
                        _C_ACCENT);
        char ds[8];
        snprintf(ds, sizeof(ds), "%d", d + 1);
        _text_draw(fb, fbw, fbh, cx + (cell - 4 - _text_width(ds)) / 2,
                   cy + cell / 2 - 8, ds, today ? 0xffffffff : _C_FG);
    }
}

/* ------------------------------------------------------------- input */
bool vt_wl_panel_contains(const vt_wl_panel_t *p, int x, int y) {
    if (!p) return false;
    if (y >= 0 && y < p->bar_h) return true;
    if (p->menu == _WL_MENU_NONE) return false;
    /* open menus also swallow clicks inside their rect */
    switch (p->menu) {
    case _WL_MENU_APPS: return x >= 8 && x < 8 + 500 &&
        y >= p->bar_h + 4 && y < p->bar_h + 4 + 14 * _WL_MENU_ROW + 8;
    case _WL_MENU_SESSION: {
        _seg_t u = _seg_user(p);
        int w = 200;
        int h = _WL_UA_COUNT * _WL_MENU_ROW + 8 +
                (p->status ? _WL_MENU_ROW : 0);
        int mx = u.x + u.w - w;
        if (mx < 4) mx = 4;
        return x >= mx && x < mx + w && y >= p->bar_h + 4 &&
               y < p->bar_h + 4 + h;
    }
    case _WL_MENU_CAL: {
        _seg_t c = _seg_clock(p);
        int cell = 28, w = 7 * cell + 16, h = 6 * cell + 60;
        int mx = _rx_clock(p) - c.w;
        if (mx < 4) mx = 4;
        return x >= mx && x < mx + w && y >= p->bar_h + 4 &&
               y < p->bar_h + 4 + h;
    }
    default: return false;
    }
}

static void _panel_action(vt_wl_panel_t *p, int act) {
    char *status = NULL;
    switch (act) {
    case _WL_UA_LOCK:
        if (system("loginctl lock-session 2>/dev/null") == 0)
            status = vt_strdup("lock requested");
        else
            status = vt_strdup("lock unavailable (no logind/elogind)");
        break;
    case _WL_UA_SUSPEND:
        if (system("systemctl suspend 2>/dev/null") != 0 &&
            system("loginctl suspend 2>/dev/null") != 0)
            status = vt_strdup("suspend failed");
        else
            status = vt_strdup("suspend requested");
        break;
    case _WL_UA_SWITCH:
        status = vt_strdup("switch: log in on another VT first "
                           "(Ctrl+Alt+F3)");
        break;
    case _WL_UA_LOGOUT:
        if (p->cb.logout) p->cb.logout(p->cb_ud);
        status = vt_strdup("logging out");
        break;
    case _WL_UA_REBOOT:
        if (system("systemctl reboot 2>/dev/null") != 0 &&
            system("loginctl reboot 2>/dev/null") != 0)
            status = vt_strdup("reboot not permitted");
        else
            status = vt_strdup("reboot requested");
        break;
    case _WL_UA_SHUTDOWN:
        if (system("systemctl poweroff 2>/dev/null") != 0 &&
            system("loginctl poweroff 2>/dev/null") != 0)
            status = vt_strdup("shutdown not permitted");
        else
            status = vt_strdup("shutdown requested");
        break;
    case _WL_UA_EXIT:
        if (p->cb.logout) p->cb.logout(p->cb_ud);
        status = vt_strdup("exiting the session");
        break;
    default:
        break;
    }
    vt_free(p->status);
    p->status = status;
    vt_logi("wl-panel: session action '%s' — %s",
            _wl_user_actions[act], status ? status : "?");
}

bool vt_wl_panel_pointer(vt_wl_panel_t *p, int x, int y, int kind,
                         int button) {
    if (!p) return false;
    /* kind: 0=motion 1=press 2=release */
    if (kind == 0 && p->menu == _WL_MENU_NONE) return false;

    /* press outside the panel/menus closes any open menu */
    if (kind == 1 && !vt_wl_panel_contains(p, x, y)) {
        p->menu = _WL_MENU_NONE;
        p->menu_sel = -1;
        return false;
    }

    if (y < p->bar_h) {
        /* ---- bar hits ---- */
        _seg_t s = _seg_start(p);
        if (x >= s.x && x < s.x + s.w) {
            if (kind == 1) {
                p->menu = (p->menu == _WL_MENU_APPS) ? _WL_MENU_NONE
                                                      : _WL_MENU_APPS;
                p->menu_sel = -1;
            }
            return true;
        }
        _seg_t u = _seg_user(p);
        if (x >= u.x && x < u.x + u.w) {
            if (kind == 1) {
                p->menu = (p->menu == _WL_MENU_SESSION) ? _WL_MENU_NONE
                                                        : _WL_MENU_SESSION;
                p->menu_sel = -1;
            }
            return true;
        }
        _seg_t c = _seg_clock(p);
        int cx = _rx_clock(p) - c.w;
        if (x >= cx && x < cx + c.w) {
            if (kind == 1) {
                if (p->menu == _WL_MENU_CAL) {
                    p->menu = _WL_MENU_NONE;
                } else {
                    time_t now = time(NULL);
                    struct tm tm;
                    localtime_r(&now, &tm);
                    p->cal_year = tm.tm_year + 1900;
                    p->cal_mon = tm.tm_mon;
                    p->menu = _WL_MENU_CAL;
                }
                p->menu_sel = -1;
            }
            return true;
        }
        int wx = _rx_ws(p);
        if (x >= wx && x < wx + p->ws_count * 26) {
            int idx = (x - wx) / 26;
            if (kind == 1 && idx >= 0 && idx < p->ws_count) {
                p->ws_cur = idx;
                if (p->cb.switch_ws) p->cb.switch_ws(idx, p->cb_ud);
            }
            return true;
        }
        int tx = s.x + s.w + 10;
        for (size_t i = 0; i < p->wins.size; i++) {
            _wlwin_t *w = vt_vec_at(&p->wins, i);
            if (x >= tx && x < tx + 134) {
                if (kind == 1 && button == 1) {
                    if (p->cb.focus_window)
                        p->cb.focus_window(w->id, p->cb_ud);
                } else if (kind == 1 && button == 3) {
                    if (p->cb.close_window) p->cb.close_window(w->id, p->cb_ud);
                }
                return true;
            }
            tx += 140;
        }
        return true;    /* empty bar space: swallow */
    }

    /* ---- open-menu hits ---- */
    switch (p->menu) {
    case _WL_MENU_APPS: {
        const int cat_w = 130, rows = 14;
        const int mh = rows * _WL_MENU_ROW + 8;
        int mx = 8, my = p->bar_h + 4;
        int lx = x - mx, ly = y - my;
        if (kind == 0) {
            if (lx < cat_w) {
                int ci = (ly - 4) / _WL_MENU_ROW;
                if (ci >= 0 && ci < _WL_N_CATS &&
                    (_wl_cat_count(&p->apps, _wl_cat_order[ci]) > 0 ||
                     ci == _WL_N_CATS - 1))
                    p->app_cat = ci;
                p->menu_sel = -1;
            } else {
                int ri = (ly - 4) / _WL_MENU_ROW;
                p->menu_sel = (ri >= 0 && ri < rows) ? ri : -1;
            }
            return true;
        }
        if (kind == 2 || kind == 1) {
            if (ly >= mh - _WL_MENU_ROW - 4 && lx < cat_w) {
                p->menu = _WL_MENU_SESSION;
                p->menu_sel = -1;
                return true;
            }
            if (lx < cat_w) {
                int ci = (ly - 4) / _WL_MENU_ROW;
                if (ci >= 0 && ci < _WL_N_CATS &&
                    (_wl_cat_count(&p->apps, _wl_cat_order[ci]) > 0 ||
                     ci == _WL_N_CATS - 1))
                    p->app_cat = ci;
                return true;
            }
            int ri = (ly - 4) / _WL_MENU_ROW;
            const char *cat = _wl_cat_order[p->app_cat];
            int row = 0;
            for (size_t i = 0; i < p->apps.size && row <= ri; i++) {
                _app_t *a = vt_vec_at(&p->apps, i);
                if (!vt_streq(a->category, cat)) continue;
                if (row == ri) {
                    _panel_spawn(a->exec);
                    p->menu = _WL_MENU_NONE;
                    return true;
                }
                row++;
            }
            return true;
        }
        return true;
    }
    case _WL_MENU_SESSION: {
        _seg_t u = _seg_user(p);
        const int mw = 200;
        int mx = u.x + u.w - mw;
        if (mx < 4) mx = 4;
        int my = p->bar_h + 4;
        int ly = y - my;
        if (kind == 0) {
            int ri = (ly - 4) / _WL_MENU_ROW;
            p->menu_sel = (ri >= 0 && ri < _WL_UA_COUNT) ? ri : -1;
            return true;
        }
        if (kind == 2 || kind == 1) {
            int ri = (ly - 4) / _WL_MENU_ROW;
            if (ri >= 0 && ri < _WL_UA_COUNT) {
                _panel_action(p, ri);
                if (ri == _WL_UA_LOGOUT || ri == _WL_UA_EXIT) {
                    p->menu = _WL_MENU_NONE;
                }
            } else {
                p->menu = _WL_MENU_NONE;
            }
            return true;
        }
        return true;
    }
    case _WL_MENU_CAL: {
        _seg_t c = _seg_clock(p);
        const int cell = 28, mw = 7 * cell + 16;
        int mx = _rx_clock(p) - c.w;
        if (mx < 4) mx = 4;
        int my = p->bar_h + 4;
        int lx = x - mx, ly = y - my;
        if ((kind == 2 || kind == 1) && ly < 36) {
            if (lx < 32) {
                if (--p->cal_mon < 0) { p->cal_mon = 11; p->cal_year--; }
            } else if (lx > mw - 32) {
                if (++p->cal_mon > 11) { p->cal_mon = 0; p->cal_year++; }
            } else {
                p->menu = _WL_MENU_NONE;
            }
            return true;
        }
        if (kind == 2 || kind == 1) p->menu = _WL_MENU_NONE;
        return true;
    }
    default:
        return false;
    }
}

bool vt_wl_panel_key(vt_wl_panel_t *p, const char *combo) {
    if (!p || !combo) return false;
    if (p->menu != _WL_MENU_NONE && vt_streq(combo, "Escape")) {
        p->menu = _WL_MENU_NONE;
        return true;
    }
    return false;
}
