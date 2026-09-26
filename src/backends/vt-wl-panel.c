/*
 * vt-wl-panel.c — Compositor-side panel for the native Wayland backend
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 *
 * A REAL panel drawn by the compositor into the scanout framebuffer —
 * not a client, not XWayland, and not placeholder blocks:
 *
 *   LEFT:   Programs button (categorized .desktop application menu with
 *           a search bar, scrolling, icons and Quit Session), window
 *           list (xdg toplevels)
 *   RIGHT:  workspace buttons, network indicator, volume (ALSA),
 *           clock (calendar popup), username + session menu
 *           (Lock/Suspend/Switch/Log Out/Reboot/Shutdown)
 *
 * Applications, categories and icons come from the shared vt-apps /
 * vt-icons modules — the same database the X11 panel uses.
 *
 * Text is rasterized with FreeType + fontconfig (VT_HAVE_FREETYPE);
 * the glyph cache keeps codepoints as ARGB bitmaps. Without FreeType
 * the panel still draws its buttons and labels with a compact built-in
 * 5x7 font so nothing is a blank placeholder.
 *
 * Input: the compositor routes pointer presses through
 * vt_wl_panel_pointer() BEFORE any client surface — clicks in the bar
 * or in an open menu never leak to clients.
 */

#define VT_LOG_DOMAIN "wl-panel"
#include <vantage/vt-core.h>
#include <vantage/vt-apps.h>
#include <vantage/vt-icons.h>
#include <vantage/vt-integrations.h>
#include "vt-wl-panel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <math.h>
#include <pwd.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#if defined(VT_HAVE_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <fontconfig/fontconfig.h>
#endif

/* ------------------------------------------------------------ palette */
#define _C_BG        0xff22262b   /* panel bar background            */
#define _C_ACCENT    0xff4f9adc
#define _C_ACCENT_HI 0xff6faae8
#define _C_FG        0xffeceef0
#define _C_DIM       0xff909399
#define _C_BTN       0xff26282e
#define _C_BTN_HI    0xff393e48
#define _C_WARN      0xffe07a50
#define _C_MENU_BG   0xf01a1c22

/* ------------------------------------------------------------ geometry */
#define _BAR_H_DEF   34
#define _ROW         26

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

/* blend an ARGB source pixmap (own stride) over the framebuffer */
static void _blend_argb(uint32_t *fb, int fbw, int fbh, int x, int y,
                        const uint32_t *px, int pw, int ph) {
    for (int sy = 0; sy < ph; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= fbh) continue;
        for (int sx = 0; sx < pw; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= fbw) continue;
            fb[dy * fbw + dx] = _blend_px(fb[dy * fbw + dx],
                                          px[sy * pw + sx]);
        }
    }
}

/* nearest-neighbor ARGB rescale */
static uint32_t *_argb_scale(const uint32_t *src, int sw, int sh,
                             int dw, int dh) {
    uint32_t *out = vt_malloc(sizeof(uint32_t) * (size_t)dw * dh);
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            out[y * dw + x] = src[sy * sw + sx];
        }
    }
    return out;
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

/* ------------------------------------------------------------ text */
#if defined(VT_HAVE_FREETYPE)
typedef struct {
    uint32_t cp;
    int w, h, xoff, yoff, adv;
    uint32_t *bgra;          /* white glyph, alpha in the high byte */
} _glyph_t;

static FT_Library  _ft_lib = NULL;
static FT_Face     _ft_face = NULL;
static FT_Face     _ft_face_fb = NULL;    /* fallback face for missing
                                             glyphs (e.g. Cyrillic) */
static vt_vec_t    _glyph_cache;
static bool        _ft_tried = false;

static bool _ft_open_face(FT_Library lib, const char *pattern,
                          FT_Face *face) {
    const char *path = NULL;
    FcConfig *fc = FcInitLoadConfigAndFonts();
    if (fc) {
        FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
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
    if (!path) return false;
    bool ok = FT_New_Face(lib, path, 0, face) == 0;
    if (ok) FT_Set_Pixel_Sizes(*face, 0, 13);
    vt_free((void *)path);
    return ok;
}

static bool _ft_init(void) {
    if (_ft_face) return true;
    if (_ft_tried) return false;
    _ft_tried = true;
    if (FT_Init_FreeType(&_ft_lib) != 0) return false;
    /* primary: the user's default sans */
    if (!_ft_open_face(_ft_lib, "sans", &_ft_face)) {
        static const char *const tries[] = {
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            NULL,
        };
        for (int i = 0; tries[i]; i++)
            if (access(tries[i], R_OK) == 0 &&
                FT_New_Face(_ft_lib, tries[i], 0, &_ft_face) == 0) {
                FT_Set_Pixel_Sizes(_ft_face, 0, 13);
                break;
            }
    }
    /* fallback face: a font that covers Cyrillic/Greek (for when the
     * primary face lacks the glyph). DejaVu/Noto cover it; if the
     * primary already IS one of those, this just fails silently. */
    _ft_open_face(_ft_lib, "sans:lang=ru", &_ft_face_fb);
    if (!_ft_face) {
        vt_logw("wl-panel: no sans font found (fontconfig) — built-in "
                "fallback glyphs");
        return false;
    }
    vt_vec_init(&_glyph_cache, sizeof(_glyph_t), 128);
    vt_logi("wl-panel: freetype ready%s", _ft_face_fb ? " + fallback" : "");
    return true;
}

static _glyph_t *_glyph_get(uint32_t cp) {
    for (size_t i = 0; i < _glyph_cache.size; i++) {
        _glyph_t *g = vt_vec_at(&_glyph_cache, i);
        if (g->cp == cp) return g;
    }
    FT_Face face = _ft_face;
    if (face && FT_Get_Char_Index(face, cp) == 0 && _ft_face_fb &&
        FT_Get_Char_Index(_ft_face_fb, cp) != 0)
        face = _ft_face_fb;               /* per-glyph fallback */
    if (!face || FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) return NULL;
    FT_GlyphSlot sl = face->glyph;
    _glyph_t g = { .cp = cp, .w = (int)sl->bitmap.width,
                   .h = (int)sl->bitmap.rows, .xoff = sl->bitmap_left,
                   .yoff = -sl->bitmap_top,
                   .adv = (int)(sl->advance.x >> 6), .bgra = NULL };
    if (g.w > 0 && g.h > 0) {
        g.bgra = vt_malloc(sizeof(uint32_t) * (size_t)g.w * (size_t)g.h);
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                unsigned char a = sl->bitmap.buffer[y * sl->bitmap.pitch + x];
                g.bgra[y * g.w + x] = ((uint32_t)a) << 24;
            }
    }
    vt_vec_push(&_glyph_cache, &g);
    return vt_vec_at(&_glyph_cache, _glyph_cache.size - 1);
}
#endif /* VT_HAVE_FREETYPE */

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

static int _text_draw(uint32_t *fb, int fbw, int fbh, int x, int y,
                      const char *utf8, uint32_t argb) {
    if (!utf8) return 0;
    int cx = x;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = _utf8_next(&p);
        if (cp == '\n') break;
#if defined(VT_HAVE_FREETYPE)
        if (_ft_init() && _ft_face) {
            _glyph_t *g = _glyph_get(cp);
            if (g) {
                if (g->bgra)
                    for (int gy = 0; gy < g->h; gy++)
                        for (int gx = 0; gx < g->w; gx++) {
                            uint32_t px = g->bgra[gy * g->w + gx];
                            uint32_t src = (px & 0xff000000u) |
                                           (argb & 0x00ffffffu);
                            int dx = cx + g->xoff + gx,
                                dy = y + 9 + g->yoff + gy;
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
                        int dx = cx + gx, dy = y + 4 + gy;
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

/* ------------------------------------------------------------ icons */
typedef struct {
    char *key;                 /* icon name */
    uint32_t *px;              /* 18x18 ARGB */
    bool tried;
} _icon_t;

static vt_icon_theme_t *_icons = NULL;
static vt_vec_t _icon_cache;

static uint32_t *_icon_get(const char *name) {
    if (!name || !*name) return NULL;
#if defined(VT_HAVE_GDKPIXBUF) || defined(VT_HAVE_PNG)
    if (!_icons) {
        _icons = vt_icon_theme_load();
        vt_vec_init(&_icon_cache, sizeof(_icon_t), 32);
    }
    for (size_t i = 0; i < _icon_cache.size; i++) {
        _icon_t *e = vt_vec_at(&_icon_cache, i);
        if (vt_streq(e->key, name)) return e->px;
    }
    _icon_t e = { .key = vt_strdup(name), .px = NULL, .tried = true };
    char path[1024];
    if (vt_icon_theme_lookup(_icons, name, 24, path, sizeof(path)) == 0) {
        uint32_t *full = NULL;
        int w = 0, h = 0;
        if (vt_icon_load_argb(path, &full, &w, &h) == 0 && w > 0 && h > 0) {
            if (w == 18 && h == 18) e.px = full;
            else {
                e.px = _argb_scale(full, w, h, 18, 18);
                vt_free(full);
            }
        }
    }
    vt_vec_push(&_icon_cache, &e);
    return e.px;
#else
    (void)_icons; (void)_icon_cache;
    return NULL;
#endif
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
    _WL_MENU_VOL,
} _menu_kind_t;

enum {
    _WL_UA_LOCK = 0, _WL_UA_SUSPEND, _WL_UA_SWITCH, _WL_UA_LOGOUT,
    _WL_UA_REBOOT, _WL_UA_SHUTDOWN, _WL_UA_EXIT, _WL_UA_COUNT
};
static const char *const _wl_user_actions[_WL_UA_COUNT] = {
    "Lock Screen", "Suspend", "Switch User", "Log Out", "Reboot",
    "Shutdown", "Exit Session",
};

struct vt_wl_panel {
    int  w;                /* screen width  */
    int  bar_h;            /* bar height    */
    vt_apps_t *apps;       /* shared .desktop database */
    bool apps_loaded;
    int  cat_vis[16];      /* visible category indices (compact rows) */
    int  n_cat_vis;
    int  cat_rows;         /* rows the category pane can show */
    vt_vec_t wins;         /* _wlwin_t      */
    int  ws_count, ws_cur;
    char username[48];
    _menu_kind_t menu;
    int  menu_sel;         /* hovered row */
    int  app_cat;          /* selected category (table index) */
    int  app_scroll;       /* first visible app row */
    int  app_rows;         /* visible app rows */
    char search[64];       /* search buffer */
    bool search_active;
    int  cal_year, cal_mon;
    char *status;          /* session-action feedback line */
    int  vol, vol_muted;
    bool vol_avail;
    time_t vol_sync;
    int  vol_drag;         /* volume popup dragging */
    bool net_up;
    char net_name[24];
    int  net_wifi;         /* -1 wired, 0..100 wireless */
    time_t net_sync;
    vt_wl_panel_cbs_t cb;
    void *cb_ud;
};

static void _vol_sync(vt_wl_panel_t *p) {
    time_t now = time(NULL);
    if (p->vol_sync && now - p->vol_sync < 2) return;
    p->vol_sync = now;
    vt_audio_t *a = vt_audio_new();
    if (!a) { p->vol_avail = false; return; }
    int pct = 0;
    if (vt_audio_init(a) == VT_OK && vt_audio_get_volume(a, &pct) == VT_OK) {
        p->vol = pct;
        p->vol_muted = vt_audio_get_mute(a);
        p->vol_avail = true;
    } else {
        p->vol_avail = false;
    }
    vt_audio_free(a);
}

static void _vol_set(vt_wl_panel_t *p, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    p->vol = pct;
    p->vol_sync = time(NULL);     /* the value is fresh — hold it */
    vt_audio_t *a = vt_audio_new();
    if (!a) return;
    if (vt_audio_init(a) == VT_OK)
        vt_audio_set_volume(a, pct);
    vt_audio_free(a);
}

static void _vol_toggle(vt_wl_panel_t *p) {
    p->vol_muted = !p->vol_muted;
    vt_audio_t *a = vt_audio_new();
    if (!a) return;
    if (vt_audio_init(a) == VT_OK)
        vt_audio_set_mute(a, p->vol_muted);
    vt_audio_free(a);
}

static void _net_sync(vt_wl_panel_t *p) {
    time_t now = time(NULL);
    if (p->net_sync && now - p->net_sync < 2) return;
    p->net_sync = now;
    p->net_up = false;
    p->net_wifi = -1;
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (vt_streq(de->d_name, "lo")) continue;
        char path[256];
        size_t len = 0;
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate",
                 de->d_name);
        char *state = vt_file_read_all(path, &len);
        if (state && vt_strstartswith(state, "up")) {
            p->net_up = true;
            snprintf(p->net_name, sizeof(p->net_name), "%s", de->d_name);
            /* wireless link quality */
            snprintf(path, sizeof(path), "/proc/net/wireless");
            char *w = vt_file_read_all(path, &len);
            if (w) {
                char *save = NULL;
                for (char *line = strtok_r(w, "\n", &save); line;
                     line = strtok_r(NULL, "\n", &save)) {
                    if (strstr(line, de->d_name)) {
                        char tmp[256];
                        snprintf(tmp, sizeof(tmp), "%s", line);
                        char *sp = NULL;
                        char *tok = strtok_r(tmp, ": ", &sp);
                        (void)tok;
                        tok = strtok_r(NULL, " ", &sp);
                        (void)tok;
                        tok = strtok_r(NULL, " ", &sp);
                        if (tok) {
                            int link = atoi(tok);
                            p->net_wifi = link > 0 ? (link * 100) / 70 : 0;
                        }
                        break;
                    }
                }
                vt_free(w);
            }
        }
        vt_free(state);
        if (p->net_up) break;
    }
    closedir(d);
}

vt_wl_panel_t *vt_wl_panel_create(int width, int bar_height) {
    vt_wl_panel_t *p = vt_malloc0(sizeof(*p));
    p->w = width;
    p->bar_h = bar_height >= 28 ? bar_height : _BAR_H_DEF;
    vt_vec_init(&p->wins, sizeof(_wlwin_t), 8);
    p->ws_count = 4;
    p->ws_cur = 0;
    p->menu = _WL_MENU_NONE;
    p->menu_sel = -1;
    p->app_cat = -1;
    p->app_scroll = 0;
    p->app_rows = 12;
    p->net_wifi = -1;
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
    vt_apps_free(p->apps);
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
    return p ? p->bar_h : _BAR_H_DEF;
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
/* LEFT:  [ Programs (110) ] [ window buttons … ]
 * RIGHT: [ ws ] [ net ] [ vol ] [ clock ] [ username ] */

static int _seg_start_x(void)   { return 8; }
static int _seg_start_w(void)   { return 110; }

static int _rx_user(const vt_wl_panel_t *p) {
    return p->w - _text_width(p->username) - 42;
}
static int _rx_clock(const vt_wl_panel_t *p) {
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%a %d %b %H:%M", &tm);
    return _rx_user(p) - 8 - _text_width(buf) - 16;
}
#define _VOL_W 60
static int _rx_vol(const vt_wl_panel_t *p)   { return _rx_clock(p) - 8 - _VOL_W; }
#define _NET_W 64
static int _rx_net(const vt_wl_panel_t *p)   { return _rx_vol(p) - 8 - _NET_W; }
static int _rx_ws(const vt_wl_panel_t *p)    { return _rx_net(p) - 8 - p->ws_count * 26; }

/* ------------------------------------------------------------- apps db */
static void _apps_load(vt_wl_panel_t *p) {
    if (p->apps_loaded) return;
    p->apps_loaded = true;
    p->apps = vt_apps_load();
    /* compact visible-category list (no gaps for empty categories) */
    p->n_cat_vis = 0;
    for (int i = 0; i < vt_apps_category_count() && p->n_cat_vis < 15; i++) {
        if (vt_apps_in_category(p->apps, i) > 0)
            p->cat_vis[p->n_cat_vis++] = i;
    }
    /* default category: first visible */
    p->app_cat = p->n_cat_vis > 0 ? p->cat_vis[0] : -1;
}

/* apps matching the active category AND the search filter */
static const vt_app_t *_app_row(const vt_wl_panel_t *p, size_t row) {
    if (!p->apps || p->app_cat < 0) return NULL;
    size_t r = 0;
    for (size_t i = 0; i < vt_apps_n(p->apps); i++) {
        const vt_app_t *a = vt_apps_at(p->apps, i);
        if (a->category != vt_apps_category_label(p->app_cat)) continue;
        if (p->search_active && p->search[0] &&
            !strcasestr(a->name, p->search) &&
            !(a->keywords && strcasestr(a->keywords, p->search)))
            continue;
        if (r == row) return a;
        r++;
    }
    return NULL;
}

static size_t _app_row_count(const vt_wl_panel_t *p) {
    if (!p->apps || p->app_cat < 0) return 0;
    size_t r = 0;
    for (size_t i = 0; i < vt_apps_n(p->apps); i++) {
        const vt_app_t *a = vt_apps_at(p->apps, i);
        if (a->category != vt_apps_category_label(p->app_cat)) continue;
        if (p->search_active && p->search[0] &&
            !strcasestr(a->name, p->search) &&
            !(a->keywords && strcasestr(a->keywords, p->search)))
            continue;
        r++;
    }
    return r;
}

/* ------------------------------------------------------------- actions */
static void _panel_spawn(const vt_app_t *app) {
    if (!app) return;
    char *cmd = vt_apps_launch_cmd(app);
    if (!cmd || !*cmd) { vt_free(cmd); return; }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        /* children inherit WAYLAND_DISPLAY/XDG_RUNTIME_DIR — the
         * compositor exported them when it created the socket */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    vt_logi("wl-panel: launched '%s' (pid %d) [%s]",
            cmd, (int)pid, app->terminal ? "terminal" : "direct");
    vt_free(cmd);
}

static void _panel_action(vt_wl_panel_t *p, int act) {
    char *status = NULL;
    switch (act) {
    case _WL_UA_LOCK:
        /* non-blocking: the compositor must never stall on system() */
        vt_proc_spawn_detached("loginctl lock-session 2>/dev/null");
        status = vt_strdup("lock requested");
        break;
    case _WL_UA_SUSPEND:
        vt_proc_spawn_detached(
            "loginctl suspend 2>/dev/null || systemctl suspend 2>/dev/null");
        status = vt_strdup("suspend requested");
        break;
    case _WL_UA_SWITCH:
        status = vt_strdup("switch: log in on another VT first "
                           "(Ctrl+Alt+F3)");
        break;
    case _WL_UA_LOGOUT:
        if (p->cb.logout) p->cb.logout("", p->cb_ud);
        status = vt_strdup("logging out");
        break;
    case _WL_UA_REBOOT:
        if (p->cb.logout) p->cb.logout("reboot", p->cb_ud);
        status = vt_strdup("reboot requested");
        break;
    case _WL_UA_SHUTDOWN:
        if (p->cb.logout) p->cb.logout("shutdown", p->cb_ud);
        status = vt_strdup("shutdown requested");
        break;
    case _WL_UA_EXIT:
        if (p->cb.logout) p->cb.logout("exit", p->cb_ud);
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

/* ------------------------------------------------------------- paint */
#define _MENU_X        8
#define _MENU_CAT_W    150
#define _MENU_W        560
#define _MENU_SEARCH_H 34
#define _CAL_CELL      30
#define _VOL_POP_W     44
#define _VOL_POP_H     170

static int _apps_menu_h(const vt_wl_panel_t *p) {
    return _MENU_SEARCH_H + p->app_rows * _ROW + 8;
}

/* Single source of truth for EVERY popup menu's rectangle. The
 * painter and the click hit-test MUST agree — when they drifted
 * apart (the session menu was drawn right-aligned but hit-tested at
 * _rx_user()-168, which slides with the USERNAME LENGTH), clicks fell
 * outside the hit region and the menu actions became unclickable on
 * machines with long login names. */
static void _menu_rect(const vt_wl_panel_t *p, int menu, int *x, int *y,
                       int *w, int *h) {
    *y = p->bar_h + 4;
    switch (menu) {
    case _WL_MENU_APPS:
        *x = _MENU_X; *w = _MENU_W;
        *h = _apps_menu_h(p);
        break;
    case _WL_MENU_SESSION:
        *w = 210;
        *h = _WL_UA_COUNT * _ROW + 8 + (p->status ? _ROW : 0);
        /* right-aligned to the panel's right margin — NEVER anchored
         * to _rx_user(): that slides with the username length */
        *x = p->w - 8 - *w;
        if (*x < 4) *x = 4;
        break;
    case _WL_MENU_CAL:
        *w = 7 * _CAL_CELL + 16;
        *h = 6 * _CAL_CELL + 64;
        *x = _rx_clock(p) - 40;
        if (*x < 4) *x = 4;
        if (*x + *w > p->w - 4) *x = p->w - 4 - *w;
        break;
    case _WL_MENU_VOL:
        *w = _VOL_POP_W; *h = _VOL_POP_H;
        *x = _rx_vol(p) + _VOL_W / 2 - *w / 2;
        break;
    default:
        *x = 0; *y = 0; *w = 0; *h = 0;
        break;
    }
}

static void _paint_apps_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                             int fbh) {
    _apps_load(p);
    int x, y, w, h;
    _menu_rect(p, _WL_MENU_APPS, &x, &y, &w, &h);
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);

    /* --- search bar --- */
    _round_rect(fb, fbw, fbh, x + 6, y + 5, w - 12, _MENU_SEARCH_H - 12, 6,
                0xff2a2e35);
    _text_draw(fb, fbw, fbh, x + 14, y + 11, "Search:", _C_DIM);
    char shown[96];
    snprintf(shown, sizeof(shown), "%s", p->search);
    _text_draw(fb, fbw, fbh, x + 14 + 52, y + 11, shown, _C_FG);
    if (p->search_active) {
        int cw = _text_width(shown);
        _fill_rect(fb, fbw, fbh, x + 14 + 52 + cw + 2, y + 12, 6, 14,
                   _C_ACCENT_HI);
    }

    int pane_y = y + _MENU_SEARCH_H;
    _fill_rect(fb, fbw, fbh, x + _MENU_CAT_W, pane_y, 1, h - _MENU_SEARCH_H,
               0xff393e48);

    /* --- category pane: COMPACT rows (no gaps) --- */
    p->cat_rows = (h - _MENU_SEARCH_H - _ROW - 8) / _ROW;
    for (int r = 0; r < p->n_cat_vis; r++) {
        int ci = p->cat_vis[r];
        if (r >= p->cat_rows) break;
        int ry = pane_y + 4 + r * _ROW;
        bool active = (ci == p->app_cat);
        if (active)
            _fill_rect(fb, fbw, fbh, x + 2, ry, _MENU_CAT_W - 4, _ROW - 2,
                       _C_ACCENT);
        _text_draw(fb, fbw, fbh, x + 10, ry + 6,
                   vt_apps_category_label(ci),
                   active ? 0xffffffff : _C_FG);
        char badge[8];
        snprintf(badge, sizeof(badge), "%d",
                 vt_apps_in_category(p->apps, ci));
        _text_draw(fb, fbw, fbh,
                   x + _MENU_CAT_W - 10 - _text_width(badge), ry + 6,
                   badge, _C_DIM);
    }

    /* --- Quit Session pinned at the bottom of the category pane --- */
    int qy = y + h - _ROW - 4;
    _fill_rect(fb, fbw, fbh, x + 2, qy, _MENU_CAT_W - 4, _ROW - 2, _C_BTN);
    _text_draw(fb, fbw, fbh, x + 10, qy + 6, "Quit Session", _C_WARN);

    /* --- applications pane (scrollable, with icons) --- */
    size_t total = _app_row_count(p);
    for (int r = 0; r < p->app_rows; r++) {
        const vt_app_t *a = _app_row(p, (size_t)(p->app_scroll + r));
        if (!a) break;
        int ry = pane_y + 4 + r * _ROW;
        bool sel = (r == p->menu_sel);
        if (sel)
            _fill_rect(fb, fbw, fbh, x + _MENU_CAT_W + 2, ry,
                       w - _MENU_CAT_W - 4, _ROW - 2, _C_BTN_HI);
        uint32_t *icon = _icon_get(a->icon);
        if (icon)
            _blend_argb(fb, fbw, fbh, x + _MENU_CAT_W + 8, ry + 3,
                        icon, 18, 18);
        else {
            /* honest fallback: a dim generic app glyph */
            _round_rect(fb, fbw, fbh, x + _MENU_CAT_W + 8, ry + 3, 18, 18,
                        4, _C_BTN_HI);
            _fill_rect(fb, fbw, fbh, x + _MENU_CAT_W + 14, ry + 9, 6, 2,
                       _C_DIM);
            _fill_rect(fb, fbw, fbh, x + _MENU_CAT_W + 14, ry + 13, 6, 2,
                       _C_DIM);
        }
        _text_draw(fb, fbw, fbh, x + _MENU_CAT_W + 34, ry + 6, a->name,
                   sel ? 0xffffffff : _C_FG);
    }
    if (total == 0)
        _text_draw(fb, fbw, fbh, x + _MENU_CAT_W + 34, pane_y + 10,
                   "(no applications)", _C_DIM);

    /* --- scrollbar when the list overflows --- */
    if (total > (size_t)p->app_rows) {
        int sb_h = (int)((double)p->app_rows / (double)total *
                         (p->app_rows * _ROW - 8));
        if (sb_h < 14) sb_h = 14;
        int sb_track = p->app_rows * _ROW - 8;
        int sb_y = pane_y + 4 + (int)((double)p->app_scroll / (double)total *
                                      (double)(sb_track - sb_h));
        _fill_rect(fb, fbw, fbh, x + w - 8, pane_y + 4, 4, sb_track,
                   0xff2a2e35);
        _fill_rect(fb, fbw, fbh, x + w - 8, sb_y, 4, sb_h, _C_ACCENT);
    }
}

static void _paint_session_menu(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                                int fbh) {
    int x, y, w, h;
    _menu_rect(p, _WL_MENU_SESSION, &x, &y, &w, &h);
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);
    for (int i = 0; i < _WL_UA_COUNT; i++) {
        int ry = y + 4 + i * _ROW;
        bool sel = (i == p->menu_sel);
        if (sel)
            _fill_rect(fb, fbw, fbh, x + 2, ry, w - 4, _ROW - 2, _C_ACCENT);
        uint32_t col = (i >= _WL_UA_REBOOT && !sel) ? _C_WARN : _C_FG;
        _text_draw(fb, fbw, fbh, x + 10, ry + 6, _wl_user_actions[i], col);
    }
    if (p->status)
        _text_draw(fb, fbw, fbh, x + 10, y + 4 + _WL_UA_COUNT * _ROW + 6,
                   p->status, _C_DIM);
}

static void _paint_calendar(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                            int fbh) {
    int x, y, w, h;
    _menu_rect(p, _WL_MENU_CAL, &x, &y, &w, &h);
    _fill_rect(fb, fbw, fbh, x, y, w, h, _C_MENU_BG);
    static const char *const mon[] = { "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    char head[64];
    snprintf(head, sizeof(head), "%s %d", mon[p->cal_mon], p->cal_year);
    int hw = _text_width(head);
    _text_draw(fb, fbw, fbh, x + (w - hw) / 2, y + 6, head, _C_FG);
    _text_draw(fb, fbw, fbh, x + 12, y + 6, "<", _C_ACCENT_HI);
    _text_draw(fb, fbw, fbh, x + w - 18, y + 6, ">", _C_ACCENT_HI);
    static const char *const wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa",
                                      "Su" };
    for (int i = 0; i < 7; i++)
        _text_draw(fb, fbw, fbh, x + 8 + i * _CAL_CELL + 8, y + 32, wd[i],
                   _C_DIM);
    struct tm first = { .tm_year = p->cal_year - 1900,
                        .tm_mon = p->cal_mon, .tm_mday = 1 };
    mktime(&first);
    int lead = (first.tm_wday + 6) % 7;          /* Monday-first offset */
    /* robust month length via mktime probing */
    int days = 31;
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
        int cx = x + 8 + col * _CAL_CELL, cy = y + 58 + row * _CAL_CELL;
        if (cy + _CAL_CELL > y + h) break;
        bool today = (p->cal_year == tmn.tm_year + 1900 &&
                      p->cal_mon == tmn.tm_mon && d + 1 == tmn.tm_mday);
        if (today)
            _round_rect(fb, fbw, fbh, cx, cy, _CAL_CELL - 4, _CAL_CELL - 4,
                        7, _C_ACCENT);
        char ds[8];
        snprintf(ds, sizeof(ds), "%d", d + 1);
        /* measured-width centering inside the day cell */
        int dw = _text_width(ds);
        _text_draw(fb, fbw, fbh, cx + (_CAL_CELL - 4 - dw) / 2,
                   cy + _CAL_CELL / 2 - 9, ds,
                   today ? 0xffffffff : _C_FG);
    }
}

static void _paint_volume(vt_wl_panel_t *p, uint32_t *fb, int fbw,
                          int fbh) {
    _vol_sync(p);
    int x, y, w, h;
    _menu_rect(p, _WL_MENU_VOL, &x, &y, &w, &h);
    _fill_rect(fb, fbw, fbh, x, y, _VOL_POP_W, _VOL_POP_H, _C_MENU_BG);
    if (!p->vol_avail) {
        _text_draw(fb, fbw, fbh, x + 6, y + 40, "no", _C_DIM);
        _text_draw(fb, fbw, fbh, x + 6, y + 66, "mixer", _C_DIM);
        return;
    }
    /* vertical slider track */
    int track_y = y + 30, track_h = _VOL_POP_H - 66;
    _fill_rect(fb, fbw, fbh, x + _VOL_POP_W / 2 - 2, track_y, 4, track_h,
               0xff2a2e35);
    int fill_h = (track_h * p->vol) / 100;
    _fill_rect(fb, fbw, fbh, x + _VOL_POP_W / 2 - 2,
               track_y + track_h - fill_h, 4, fill_h, _C_ACCENT);
    /* knob */
    int knob_y = track_y + track_h - fill_h - 7;
    _round_rect(fb, fbw, fbh, x + _VOL_POP_W / 2 - 8, knob_y, 16, 14, 5,
                p->vol_muted ? _C_DIM : _C_ACCENT_HI);
    char v[8];
    snprintf(v, sizeof(v), "%d", p->vol);
    int vw = _text_width(v);
    _text_draw(fb, fbw, fbh, x + (_VOL_POP_W - vw) / 2, y + 8, v, _C_FG);
    snprintf(v, sizeof(v), "%s", p->vol_muted ? "MUTE" : "ALSA");
    vw = _text_width(v);
    _text_draw(fb, fbw, fbh, x + (_VOL_POP_W - vw) / 2,
               y + _VOL_POP_H - 26, v, _C_DIM);
}

void vt_wl_panel_paint(vt_wl_panel_t *p, uint32_t *fb, int fbw, int fbh) {
    if (!p) return;
    int h = p->bar_h;
    _fill_rect(fb, fbw, fbh, 0, 0, fbw, h, 0xff23262b);
    _hline(fb, fbw, fbh, 0, h - 1, fbw, 0xff393e48);

    /* --- Programs button --- */
    int sx = _seg_start_x(), sw = _seg_start_w();
    _round_rect(fb, fbw, fbh, sx, 5, sw, h - 10, 8, _C_ACCENT);
    uint32_t fg = 0xffffffff;
    /* grid glyph */
    for (int gy = 0; gy < 3; gy++)
        for (int gx = 0; gx < 3; gx++)
            _fill_rect(fb, fbw, fbh, sx + 12 + gx * 6, 12 + gy * 6, 3, 3,
                       fg);
    _text_draw(fb, fbw, fbh, sx + 40, h / 2 - 7, "Programs", 0xffffffff);

    /* --- task buttons (xdg toplevels) --- */
    int tx = sx + sw + 10;
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

    /* --- network indicator --- */
    _net_sync(p);
    {
        int nx = _rx_net(p);
        char buf[32];
        if (!p->net_up) snprintf(buf, sizeof(buf), "net off");
        else if (p->net_wifi >= 0)
            snprintf(buf, sizeof(buf), "wifi %d%%", p->net_wifi);
        else snprintf(buf, sizeof(buf), "net %s", p->net_name);
        uint32_t c = p->net_up ? _C_FG : _C_DIM;
        int bw2 = _text_width(buf);
        _text_draw(fb, fbw, fbh, nx + (_NET_W - bw2) / 2, h / 2 - 7, buf, c);
    }

    /* --- volume --- */
    _vol_sync(p);
    {
        int vx = _rx_vol(p);
        char buf[32];
        if (!p->vol_avail) snprintf(buf, sizeof(buf), "vol --");
        else snprintf(buf, sizeof(buf), "%s %d%%",
                      p->vol_muted ? "M" : "V", p->vol);
        uint32_t c = p->vol_avail ? (p->vol_muted ? _C_WARN : _C_FG)
                                  : _C_DIM;
        int bw2 = _text_width(buf);
        _text_draw(fb, fbw, fbh, vx + (_VOL_W - bw2) / 2, h / 2 - 7, buf, c);
    }

    /* --- clock --- */
    {
        char buf[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%a %d %b %H:%M", &tm);
        _text_draw(fb, fbw, fbh, _rx_clock(p) + 6, h / 2 - 7, buf, _C_FG);
    }

    /* --- username + caret --- */
    {
        int ux = _rx_user(p);
        _round_rect(fb, fbw, fbh, ux, 4, 28, h - 8, (h - 8) / 2, _C_ACCENT);
        _fill_rect(fb, fbw, fbh, ux + 12, 10, 4, 3, fg);
        _fill_rect(fb, fbw, fbh, ux + 10, 15, 8, 3, fg);
        _text_draw(fb, fbw, fbh, ux + 34, h / 2 - 7, p->username, _C_FG);
        for (int i = 0; i < 3; i++)
            _fill_rect(fb, fbw, fbh, ux + 34 + _text_width(p->username)
                       + 4 + i * 4, h / 2 - 1 + i, 2, 2, _C_DIM);
    }

    /* --- open menus on top --- */
    switch (p->menu) {
    case _WL_MENU_APPS:    _paint_apps_menu(p, fb, fbw, fbh); break;
    case _WL_MENU_SESSION: _paint_session_menu(p, fb, fbw, fbh); break;
    case _WL_MENU_CAL:     _paint_calendar(p, fb, fbw, fbh); break;
    case _WL_MENU_VOL:     _paint_volume(p, fb, fbw, fbh); break;
    default: break;
    }
}

/* ------------------------------------------------------------- input */
bool vt_wl_panel_contains(const vt_wl_panel_t *p, int x, int y) {
    if (!p) return false;
    if (y >= 0 && y < p->bar_h) return true;
    if (p->menu == _WL_MENU_NONE) return false;
    int mx, my, mw, mh;
    _menu_rect(p, p->menu, &mx, &my, &mw, &mh);
    return x >= mx && x < mx + mw && y >= my && y < my + mh;
}

bool vt_wl_panel_pointer(vt_wl_panel_t *p, int x, int y, int kind,
                         int button) {
    if (!p) return false;
    vt_logd("wl-panel: pointer kind=%d b=%d at %d,%d menu=%d", kind,
            button, x, y, (int)p->menu);
    /* kind: 0=motion 1=press 2=release */
    if (kind == 0 && p->menu == _WL_MENU_NONE) return false;

    /* press outside the panel/menus closes any open menu */
    if (kind == 1 && !vt_wl_panel_contains(p, x, y)) {
        p->menu = _WL_MENU_NONE;
        p->menu_sel = -1;
        p->vol_drag = 0;
        return false;
    }

    if (y < p->bar_h) {
        /* ---- bar hits ---- */
        int sx = _seg_start_x(), sw = _seg_start_w();
        if (x >= sx && x < sx + sw) {
            if (kind == 1) {
                if (p->menu == _WL_MENU_APPS) {
                    p->menu = _WL_MENU_NONE;
                } else {
                    p->menu = _WL_MENU_APPS;
                    p->search[0] = 0;
                    p->search_active = true;
                    p->app_scroll = 0;
                    p->menu_sel = -1;
                    _apps_load(p);
                }
            }
            return true;
        }
        int ux = _rx_user(p);
        if (x >= ux && x < p->w - 8) {
            if (kind == 1) {
                p->menu = (p->menu == _WL_MENU_SESSION) ? _WL_MENU_NONE
                                                        : _WL_MENU_SESSION;
                p->menu_sel = -1;
            }
            return true;
        }
        int kx = _rx_clock(p);
        if (x >= kx && x < _rx_vol(p) - 8) {
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
        int vx = _rx_vol(p);
        if (x >= vx && x < vx + _VOL_W) {
            if (kind == 1 && button == 2) {
                /* middle-click on the applet toggles mute (same
                 * convention as the X11 volume applet) */
                _vol_toggle(p);
                return true;
            }
            if (kind == 1) {
                if (p->menu == _WL_MENU_VOL) {
                    p->menu = _WL_MENU_NONE;
                } else {
                    p->menu = _WL_MENU_VOL;
                    p->vol_drag = 0;
                }
            }
            return true;
        }
        int nx = _rx_net(p);
        if (x >= nx && x < nx + _NET_W) return true;  /* indicator only */
        int wx = _rx_ws(p);
        if (x >= wx && x < wx + p->ws_count * 26) {
            int idx = (x - wx) / 26;
            if (kind == 1 && idx >= 0 && idx < p->ws_count) {
                p->ws_cur = idx;
                if (p->cb.switch_ws) p->cb.switch_ws(idx, p->cb_ud);
            }
            return true;
        }
        int tx = sx + sw + 10;
        for (size_t i = 0; i < p->wins.size; i++) {
            _wlwin_t *w = vt_vec_at(&p->wins, i);
            if (x >= tx && x < tx + 134) {
                if (kind == 1 && button == 1) {
                    if (p->cb.focus_window)
                        p->cb.focus_window(w->id, p->cb_ud);
                } else if (kind == 1 && button == 3) {
                    if (p->cb.close_window)
                        p->cb.close_window(w->id, p->cb_ud);
                }
                return true;
            }
            tx += 140;
        }
        return true;    /* empty bar space: swallow */
    }

    /* ---- open-menu hits ---- */
    int my = p->bar_h + 4;
    switch (p->menu) {
    case _WL_MENU_APPS: {
        int lx = x - _MENU_X, ly = y - my;
        int pane_y = _MENU_SEARCH_H;
        if (kind == 0) {
            if (ly < pane_y) return true;     /* search bar hover */
            if (lx < _MENU_CAT_W) {
                int r = (ly - pane_y - 4) / _ROW;
                if (r >= 0 && r < p->n_cat_vis && r < p->cat_rows)
                    p->app_cat = p->cat_vis[r];
                p->menu_sel = -1;
            } else {
                int r = (ly - pane_y - 4) / _ROW;
                p->menu_sel = (r >= 0 && r < p->app_rows) ? r : -1;
            }
            return true;
        }
        /* click */
        if (ly < pane_y) {                     /* search bar focus */
            p->search_active = true;
            return true;
        }
        if (ly >= _apps_menu_h(p) - _ROW - 4 && lx < _MENU_CAT_W) {
            /* Quit Session → session actions menu */
            p->menu = _WL_MENU_SESSION;
            p->menu_sel = -1;
            return true;
        }
        if (lx < _MENU_CAT_W) {
            int r = (ly - pane_y - 4) / _ROW;
            vt_logd("wl-panel: cat click r=%d nvis=%d catrows=%d cur=%d->%s",
                    r, p->n_cat_vis, p->cat_rows, p->app_cat,
                    r >= 0 && r < p->n_cat_vis ?
                    vt_apps_category_label(p->cat_vis[r]) : "?");
            if (r >= 0 && r < p->n_cat_vis && r < p->cat_rows) {
                p->app_cat = p->cat_vis[r];
                p->app_scroll = 0;
            }
            return true;
        }
        int r = (ly - pane_y - 4) / _ROW;
        const vt_app_t *a = _app_row(p, (size_t)(p->app_scroll + r));
        vt_logd("wl-panel: app click row=%d -> %s", r,
                a ? a->name : "(none)");
        if (a) {
            _panel_spawn(a);
            p->menu = _WL_MENU_NONE;
        }
        return true;
    }
    case _WL_MENU_SESSION: {
        /* rows resolve from y alone; x-bounds were already enforced by
         * vt_wl_panel_contains() via _menu_rect (single geometry source) */
        int ly = y - my;
        if (kind == 0) {
            int ri = (ly - 4) / _ROW;
            p->menu_sel = (ri >= 0 && ri < _WL_UA_COUNT) ? ri : -1;
            return true;
        }
        int ri = (ly - 4) / _ROW;
        if (ri >= 0 && ri < _WL_UA_COUNT) {
            _panel_action(p, ri);
            if (ri == _WL_UA_LOGOUT || ri == _WL_UA_REBOOT ||
                ri == _WL_UA_SHUTDOWN || ri == _WL_UA_EXIT)
                p->menu = _WL_MENU_NONE;
        } else {
            p->menu = _WL_MENU_NONE;
        }
        return true;
    }
    case _WL_MENU_CAL: {
        int mxx, myy, mww, mhh;
        _menu_rect(p, _WL_MENU_CAL, &mxx, &myy, &mww, &mhh);
        int lx = x - mxx, ly = y - my;
        if ((kind == 2 || kind == 1) && ly < 38) {
            if (lx < 32) {
                if (--p->cal_mon < 0) { p->cal_mon = 11; p->cal_year--; }
            } else if (lx > mww - 32) {
                if (++p->cal_mon > 11) { p->cal_mon = 0; p->cal_year++; }
            } else {
                p->menu = _WL_MENU_NONE;
            }
            return true;
        }
        if (kind == 2 || kind == 1) p->menu = _WL_MENU_NONE;
        return true;
    }
    case _WL_MENU_VOL: {
        int mxx, myy, mww, mhh;
        _menu_rect(p, _WL_MENU_VOL, &mxx, &myy, &mww, &mhh);
        int lx = x - mxx, ly = y - my;
        if (!p->vol_avail) {
            if (kind == 1) p->menu = _WL_MENU_NONE;
            return true;
        }
        int track_y = 30, track_h = _VOL_POP_H - 66;
        if (kind == 1 && lx >= 4 && lx < _VOL_POP_W - 4 &&
            ly >= track_y - 10 && ly < track_y + track_h + 10) {
            p->vol_drag = 1;
            int rel = track_y + track_h - ly;
            _vol_set(p, (rel * 100) / track_h);
            return true;
        }
        if (kind == 0 && p->vol_drag) {
            int rel = track_y + track_h - ly;
            _vol_set(p, (rel * 100) / track_h);
            return true;
        }
        if (kind == 2 && p->vol_drag) {
            p->vol_drag = 0;
            return true;
        }
        if (kind == 1) {
            p->menu = _WL_MENU_NONE;      /* click outside slider */
            p->vol_drag = 0;
        }
        return true;
    }
    default:
        return false;
    }
}

bool vt_wl_panel_axis(vt_wl_panel_t *p, int x, int y, int dir) {
    if (!p) return false;
    if (y < p->bar_h) {
        /* wheel over the volume button adjusts the volume directly */
        int vx = _rx_vol(p);
        if (x >= vx && x < vx + _VOL_W && p->vol_avail) {
            _vol_set(p, p->vol + (dir > 0 ? -5 : 5));
            return true;
        }
        return false;
    }
    if (p->menu == _WL_MENU_APPS && vt_wl_panel_contains(p, x, y) &&
        x >= _MENU_X + _MENU_CAT_W) {
        size_t total = _app_row_count(p);
        if (total > (size_t)p->app_rows) {
            p->app_scroll += dir > 0 ? 3 : -3;
            if (p->app_scroll < 0) p->app_scroll = 0;
            if ((size_t)p->app_scroll + (size_t)p->app_rows > total)
                p->app_scroll = (int)total - p->app_rows;
            if (p->app_scroll < 0) p->app_scroll = 0;
        }
        return true;
    }
    if (p->menu == _WL_MENU_VOL && p->vol_avail &&
        vt_wl_panel_contains(p, x, y)) {
        _vol_set(p, p->vol + (dir > 0 ? -5 : 5));
        return true;
    }
    return false;
}

bool vt_wl_panel_key(vt_wl_panel_t *p, const char *combo, uint32_t cp) {
    if (!p) return false;
    if (p->menu == _WL_MENU_NONE) return false;
    if (combo && vt_streq(combo, "Escape")) {
        p->menu = _WL_MENU_NONE;
        return true;
    }
    if (p->menu == _WL_MENU_APPS) {
        if (combo && vt_streq(combo, "BackSpace")) {
            size_t l = strlen(p->search);
            /* UTF-8 aware: drop the last codepoint */
            while (l > 0 && ((p->search[l - 1] & 0xc0) == 0x80)) l--;
            if (l > 0) l--;
            p->search[l] = 0;
            p->app_scroll = 0;
            return true;
        }
        if (cp >= 32 && strlen(p->search) + 5 < sizeof(p->search)) {
            char ub[8];
            int n = 0;
            if (cp < 0x80) ub[n++] = (char)cp;
            else if (cp < 0x800) {
                ub[n++] = (char)(0xc0 | (cp >> 6));
                ub[n++] = (char)(0x80 | (cp & 0x3f));
            } else {
                ub[n++] = (char)(0xe0 | (cp >> 12));
                ub[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                ub[n++] = (char)(0x80 | (cp & 0x3f));
            }
            ub[n] = 0;
            strcat(p->search, ub);
            p->app_scroll = 0;
            return true;
        }
    }
    return false;
}
