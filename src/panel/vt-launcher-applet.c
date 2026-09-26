/* ------------------------------------------------------------ launcher */
/* The Vantage Programs button: opens the application menu built from
 * XDG .desktop entries via the SHARED vt-apps database (the exact same
 * parser, category table and locale handling the native Wayland panel
 * uses), grouped into the standard categories, with a search bar,
 * scrolling, real icon-theme icons, and a Quit Session entry that
 * opens the session actions menu.
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 * Copyright (c) 2026 FemBoyGamerTechGuy
 */

#define VT_LOG_DOMAIN "panel-applets"
#include "vt-panel-internal.h"
#include <vantage/vt-config.h>
#include <vantage/vt-apps.h>
#include <vantage/vt-icons.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <pwd.h>
#include <X11/keysym.h>
#include <vantage/vt-integrations.h>

typedef struct {
    vt_apps_t *apps;         /* shared .desktop database */
    bool loaded;

    Window menu_win;         /* override-redirect popup */
    XftDraw *menu_draw;
    int  menu_rows;          /* visible app rows */
    int  menu_sel;           /* hovered app row, -1 none */
    int  cur_cat;            /* selected category TABLE index */
    int  scroll;             /* first visible app row */
    char search[64];
    bool search_focused;

    /* icon cache: name → _LI_SZ x _LI_SZ ARGB (quality-scaled) */
    vt_vec_t icon_keys;      /* char* */
    vt_vec_t icon_px;        /* uint32_t* */
    vt_icon_theme_t *theme;
} _launcher_t;

/* 24px display size with area-averaged/bilinear resampling and SVG
 * rasterization at the exact size — the old 18px nearest-neighbour
 * icons looked small and low-resolution on both backends */
#define _LI_SZ 24

/* ---- menu geometry (mirrors the Wayland panel's menu) ---- */
#define _AM_X        8
#define _AM_CAT_W    150
#define _AM_W        560
#define _AM_ROW      30
#define _AM_ROWS     12
#define _AM_SEARCH_H 34
#define _AM_H        (_AM_SEARCH_H + _AM_ROWS * _AM_ROW + 8)

static void _launcher_menu_paint(vt_applet_env_t *env);
static void _launcher_menu_handle(vt_panel_t *p, XEvent *ev);
static void _launcher_menu_close(vt_applet_env_t *env);
static void _launcher_menu_open(vt_applet_env_t *env);

/* ------------------------------------------------------ icon cache */
static uint32_t *_icon_get(_launcher_t *l, const char *name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < l->icon_keys.size; i++)
        if (vt_streq(*(const char *const *)vt_vec_at(&l->icon_keys, i), name))
            return *(uint32_t *const *)vt_vec_at(&l->icon_px, i);
    if (!l->theme) l->theme = vt_icon_theme_load();
    /* one call: theme lookup + SVG-at-size / box-filtered raster scaling */
    uint32_t *px = vt_icon_lookup_argb(l->theme, name, _LI_SZ);
    /* negative results are cached too (avoid re-resolving on repaint) */
    char *key = vt_strdup(name);
    vt_vec_push(&l->icon_keys, &key);
    vt_vec_push(&l->icon_px, &px);
    return px;
}

/* -------------------------------------------------------- app filtering */
static bool _app_matches(const vt_app_t *a, int cat_idx,
                         const char *search) {
    if (cat_idx < 0) return false;
    if (a->category != vt_apps_category_label(cat_idx)) return false;
    if (search && *search) {
        if (strcasestr(a->name, search)) return true;
        if (a->keywords && strcasestr(a->keywords, search)) return true;
        return false;
    }
    return true;
}

static const vt_app_t *_app_row(_launcher_t *l, int cat_idx,
                                const char *search, size_t row) {
    size_t r = 0;
    for (size_t i = 0; i < vt_apps_n(l->apps); i++) {
        const vt_app_t *a = vt_apps_at(l->apps, i);
        if (!_app_matches(a, cat_idx, search)) continue;
        if (r == row) return a;
        r++;
    }
    return NULL;
}

static size_t _app_count(_launcher_t *l, int cat_idx, const char *search) {
    size_t r = 0;
    for (size_t i = 0; i < vt_apps_n(l->apps); i++)
        if (_app_matches(vt_apps_at(l->apps, i), cat_idx, search)) r++;
    return r;
}

/* ------------------------------------------------------------- applet */
static void _launcher_init(vt_applet_env_t *env) {
    _launcher_t *l = vt_malloc0(sizeof(*l));
    vt_vec_init(&l->icon_keys, sizeof(char *), 32);
    vt_vec_init(&l->icon_px, sizeof(uint32_t *), 32);
    l->menu_sel = -1;
    l->cur_cat = -1;
    env->state = l;
}

static void _launcher_fini(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    if (!l) return;
    for (size_t i = 0; i < l->icon_keys.size; i++) {
        char **k = vt_vec_at(&l->icon_keys, i);
        vt_free(*k);
    }
    for (size_t i = 0; i < l->icon_px.size; i++) {
        uint32_t **v = vt_vec_at(&l->icon_px, i);
        vt_free(*v);
    }
    vt_vec_fini(&l->icon_keys);
    vt_vec_fini(&l->icon_px);
    if (l->theme) vt_icon_theme_free(l->theme);
    vt_apps_free(l->apps);
    vt_free(l);
}

static void _launcher_load(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    if (l->loaded) return;
    l->loaded = true;
    l->apps = vt_apps_load();
    /* default category: first one that has applications */
    for (int i = 0; i < vt_apps_category_count(); i++) {
        if (vt_apps_in_category(l->apps, i) > 0) { l->cur_cat = i; break; }
    }
}

static int _launcher_measure(vt_applet_env_t *env) {
    _launcher_load(env);
    return 132;
}

static void _launcher_render(vt_applet_env_t *env) {
    vt_pctx_t *ctx = env->ctx;
    _launcher_t *l = env->state;
    int x = env->area.x, h = env->area.h;
    int y = env->area.y;
    /* Programs button: accent rounded square + a themed start icon
     * (start-here from the active icon theme — like an XFCE start
     * button uses the theme's logo), with the drawn grid glyph as the
     * honest fallback when the theme has none */
    vt_pcol_t accent = { 0x4f, 0x9a, 0xdc, 0xff };
    vt_pcol_t fg = { 0xec, 0xee, 0xf0, 0xff };
    vt_pctx_rounded_rect(ctx, x + 4, y + 3, 30, h - 6, 7, accent);
    uint32_t *icon = NULL;
    {
        const char *envn = getenv("VANTAGE_START_ICON");
        if (envn && *envn) icon = _icon_get(l, envn);
        if (!icon) icon = _icon_get(l, "start-here");
        if (!icon) icon = _icon_get(l, "vantage-start");
    }
    if (icon) {
        vt_pctx_draw_argb(ctx, x + 7, y + (h - _LI_SZ) / 2, _LI_SZ, _LI_SZ,
                          icon, _LI_SZ, _LI_SZ);
    } else {
        for (int gy = 0; gy < 3; gy++)
            for (int gx = 0; gx < 3; gx++)
                vt_pctx_rect(ctx, x + 7 + 5 + gx * 5, y + 3 + 5 + gy * 5,
                             2, 2, fg);
    }
    vt_pctx_text(ctx, x + 38, y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2,
                 "Programs", true, fg);
}

/* --------------------------------------------------- the Programs menu */
static void _launcher_menu_open(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (l->menu_win) return;
    _launcher_load(env);
    l->menu_rows = _AM_ROWS;
    l->scroll = 0;
    l->menu_sel = -1;
    l->search[0] = 0;
    l->search_focused = true;
    int x = env->area.x;
    int y = env->area.y + env->area.h + 4;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask |
                                              Button4Mask | Button5Mask |
                                              KeyPressMask };
    l->menu_win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                                x, y, (unsigned)_AM_W, (unsigned)_AM_H, 1,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel |
                                CWEventMask, &wa);
    l->menu_draw = XftDrawCreate(ctx->dpy, l->menu_win,
                                 DefaultVisual(ctx->dpy,
                                               DefaultScreen(ctx->dpy)),
                                 DefaultColormap(ctx->dpy,
                                                 DefaultScreen(ctx->dpy)));
    XMapWindow(ctx->dpy, l->menu_win);
    /* keyboard focus for the search bar (override-redirect windows
     * are not WM-managed; focus goes directly) */
    XSetInputFocus(ctx->dpy, l->menu_win, RevertToParent, CurrentTime);
    vt_panel_set_popup(env->panel, l->menu_win, _launcher_menu_handle);
    XFlush(ctx->dpy);
    _launcher_menu_paint(env);
}

static void _launcher_menu_close(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!l->menu_win) return;
    if (l->menu_draw) { XftDrawDestroy(l->menu_draw); l->menu_draw = NULL; }
    XDestroyWindow(ctx->dpy, l->menu_win);
    l->menu_win = 0;
    vt_panel_set_popup(env->panel, 0, NULL);
    XFlush(ctx->dpy);
    vt_panel_invalidate(env->panel);
}

static void _launcher_menu_paint(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!l->menu_win || !l->menu_draw) return;
    Display *dpy = ctx->dpy;
    int w = _AM_W, h = _AM_H;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
        DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, l->menu_win, fmt, 0, NULL);
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)w,
                         (unsigned)h);

    /* text helper with the same per-codepoint font fallback the panel
     * bar uses (Russian/CJK app names render correctly) */
    #define _MT(txt, xx, yy, colr, bold_)                              \
        vt_pctx_menu_text(ctx, l->menu_draw, (xx), (yy), (txt),         \
                          (bold_), (colr))

    /* --- search bar --- */
    XRenderColor sbg = { .red = 0x2a2a, .green = 0x2e2e, .blue = 0x3535,
                         .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &sbg, 6, 5,
                          (unsigned)(w - 12), _AM_SEARCH_H - 12);
    vt_pcol_t dim = { 0x90, 0x93, 0x99, 0xff };
    _MT("Search:", 14, 26, dim, false);
    vt_pcol_t fgc = { 0xec, 0xee, 0xf0, 0xff };
    _MT(l->search, 14 + 52, 26, fgc, false);
    if (l->search_focused) {
        XGlyphInfo gi;
        XftTextExtentsUtf8(dpy, ctx->font, (const FcChar8 *)l->search,
                           (int)strlen(l->search), &gi);
        XRenderColor caret = { .red = 0x6f6f, .green = 0xaaaa,
                               .blue = 0xe8e8, .alpha = 0xffff };
        XRenderFillRectangle(dpy, PictOpSrc, pic, &caret,
                             (short)(14 + 52 + gi.xOff + 2), 12, 6, 14);
    }

    /* separator between the panes */
    XRenderColor sep = { .red = 0x3939, .green = 0x3e3e, .blue = 0x4848,
                         .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &sep, _AM_CAT_W,
                         _AM_SEARCH_H, 1, (unsigned)(h - _AM_SEARCH_H));

    /* --- category pane: COMPACT rows (no gaps for empty categories) */
    int cat_row = 0;
    int cat_rows_max = (h - _AM_SEARCH_H - _AM_ROW - 8) / _AM_ROW;
    for (int ci = 0; ci < vt_apps_category_count(); ci++) {
        if (cat_row >= cat_rows_max) break;
        int n = vt_apps_in_category(l->apps, ci);
        if (n == 0 && ci != vt_apps_category_count() - 1) continue;
        int row_y = _AM_SEARCH_H + 4 + cat_row * _AM_ROW;
        bool active = (ci == l->cur_cat);
        if (active) {
            XRenderColor hi = { .red = 0x4f4f, .green = 0x9a9a,
                                .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, 2,
                                  (short)row_y, (unsigned)(_AM_CAT_W - 4),
                                  (unsigned)(_AM_ROW - 2));
        }
        vt_pcol_t tc = fgc;
        if (active) tc = (vt_pcol_t){ 0xff, 0xff, 0xff, 0xff };
        _MT(vt_apps_category_label(ci), 10, row_y + _AM_ROW - 7, tc, active);
        char badge[16];
        snprintf(badge, sizeof(badge), "%d", n);
        vt_pcol_t dcol = dim;
        _MT(badge, _AM_CAT_W - 8 - (int)strlen(badge) * 7,
            row_y + _AM_ROW - 7, dcol, false);
        cat_row++;
    }

    /* Quit Session pinned at the bottom of the category pane */
    {
        int qy = h - _AM_ROW - 4;
        XRenderColor qb = { .red = 0x2626, .green = 0x2828,
                            .blue = 0x2e2e, .alpha = 0xffff };
        XRenderFillRectangle(dpy, PictOpSrc, pic, &qb, 2, (short)qy,
                             (unsigned)(_AM_CAT_W - 4),
                             (unsigned)(_AM_ROW - 2));
        vt_pcol_t warn = { 0xe0, 0x7a, 0x50, 0xff };
        _MT("Quit Session", 10, qy + _AM_ROW - 7, warn, false);
    }

    /* --- applications pane (scrollable, with icons) --- */
    size_t total = _app_count(l, l->cur_cat, l->search);
    for (int r = 0; r < _AM_ROWS; r++) {
        const vt_app_t *a = _app_row(l, l->cur_cat, l->search,
                                     (size_t)(l->scroll + r));
        if (!a) break;
        int ry = _AM_SEARCH_H + 4 + r * _AM_ROW;
        bool sel = (r == l->menu_sel);
        if (sel) {
            XRenderColor hi = { .red = 0x3939, .green = 0x3e3e,
                                .blue = 0x4848, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, _AM_CAT_W + 2,
                                  (short)ry, (unsigned)(w - _AM_CAT_W - 4),
                                  (unsigned)(_AM_ROW - 2));
        }
        uint32_t *icon = _icon_get(l, a->icon);
        int iy = ry + (_AM_ROW - _LI_SZ) / 2;
        if (icon)
            vt_pctx_draw_argb_pic(dpy, pic, _AM_CAT_W + 8, iy, _LI_SZ,
                                  _LI_SZ, icon, _LI_SZ, _LI_SZ);
        else {
            XRenderColor fb = { .red = 0x3939, .green = 0x3e3e,
                                .blue = 0x4848, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &fb, _AM_CAT_W + 8,
                                  (short)iy, (unsigned)_LI_SZ,
                                  (unsigned)_LI_SZ);
        }
        vt_pcol_t name_col = fgc;
        if (sel) name_col = (vt_pcol_t){ 0xff, 0xff, 0xff, 0xff };
        _MT(a->name, _AM_CAT_W + 8 + _LI_SZ + 8, ry + _AM_ROW - 7,
            name_col, false);
    }
    if (total == 0) {
        _MT("(no applications)", _AM_CAT_W + 34, _AM_SEARCH_H + _AM_ROW - 7,
            dim, false);
    }

    /* --- scrollbar when the list overflows --- */
    if (total > (size_t)_AM_ROWS) {
        int track = _AM_ROWS * _AM_ROW - 8;
        int sb_h = (int)((double)_AM_ROWS / (double)total * track);
        if (sb_h < 14) sb_h = 14;
        int sb_y = _AM_SEARCH_H + 4 +
                   (int)((double)l->scroll / (double)total *
                         (double)(track - sb_h));
        XRenderColor trk = { .red = 0x2a2a, .green = 0x2e2e,
                             .blue = 0x3535, .alpha = 0xffff };
        XRenderFillRectangle(dpy, PictOpSrc, pic, &trk, w - 8,
                             (short)(_AM_SEARCH_H + 4), 4,
                             (unsigned)track);
        XRenderColor knb = { .red = 0x4f4f, .green = 0x9a9a,
                             .blue = 0xdcdc, .alpha = 0xffff };
        XRenderFillRectangle(dpy, PictOpSrc, pic, &knb, w - 8, (short)sb_y,
                             4, (unsigned)sb_h);
    }

    XRenderFreePicture(dpy, pic);
    XFlush(dpy);
    #undef _MT
}

/* the Quit Session row (bottom of the category pane) */
#define _AM_QUIT_Y (_AM_H - _AM_ROW - 4)

static void _search_backspace(_launcher_t *l) {
    size_t len = strlen(l->search);
    /* UTF-8 aware: strip one codepoint */
    while (len > 0 && (l->search[len - 1] & 0xc0) == 0x80) len--;
    if (len > 0) len--;
    l->search[len] = 0;
}

static void _launcher_menu_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_LAUNCHER);
    _launcher_t *l = env.state;
    if (!l || ev->xany.window != l->menu_win) return;
    switch (ev->type) {
    case Expose:
        _launcher_menu_paint(&env);
        break;
    case KeyPress: {
        KeySym ks;
        char buf[32];
        int n = XLookupString(&ev->xkey, buf, sizeof(buf) - 1, &ks, NULL);
        buf[n > 0 ? n : 0] = 0;
        if (ks == XK_Escape) {
            if (l->search[0]) { l->search[0] = 0; l->scroll = 0; }
            else _launcher_menu_close(&env);
        } else if (ks == XK_BackSpace) {
            _search_backspace(l);
            l->scroll = 0;
        } else if (n > 0 && (uint8_t)buf[0] >= 32 &&
                   strlen(l->search) + (size_t)n < sizeof(l->search)) {
            strcat(l->search, buf);
            l->scroll = 0;
        }
        _launcher_menu_paint(&env);
        break;
    }
    case MotionNotify: {
        int x = ev->xmotion.x, y = ev->xmotion.y;
        if (y < _AM_SEARCH_H) { l->menu_sel = -1; break; }
        if (x < _AM_CAT_W) {
            l->menu_sel = -1;
        } else {
            int ri = (y - _AM_SEARCH_H - 4) / _AM_ROW;
            l->menu_sel = (ri >= 0 && ri < _AM_ROWS) ? ri : -1;
        }
        _launcher_menu_paint(&env);
        break;
    }
    case ButtonPress:
        /* wheel scrolls the application list */
        if (ev->xbutton.button == Button4 ||
            ev->xbutton.button == Button5) {
            size_t total = _app_count(l, l->cur_cat, l->search);
            if (total > (size_t)_AM_ROWS) {
                int d = ev->xbutton.button == Button4 ? -3 : 3;
                l->scroll += d;
                if (l->scroll < 0) l->scroll = 0;
                if ((size_t)l->scroll + _AM_ROWS > total)
                    l->scroll = (int)total - _AM_ROWS;
                if (l->scroll < 0) l->scroll = 0;
            }
            _launcher_menu_paint(&env);
            break;
        }
        break;
    case ButtonRelease: {
        if (ev->xbutton.button != Button1) {
            _launcher_menu_close(&env);
            break;
        }
        int x = ev->xbutton.x, y = ev->xbutton.y;
        if (y < _AM_SEARCH_H) {
            l->search_focused = true;   /* click focuses the search box */
            _launcher_menu_paint(&env);
            break;
        }
        if (y >= _AM_QUIT_Y && x < _AM_CAT_W) {
            /* Quit Session -> open the session actions menu */
            _launcher_menu_close(&env);
            vt_panel_user_menu_open(p);
            break;
        }
        if (x < _AM_CAT_W) {
            /* COMPACT category rows: map the clicked row back to the
             * table index by walking the same visible sequence */
            int r = (y - _AM_SEARCH_H - 4) / _AM_ROW;
            int cat_row = 0, picked = -1;
            for (int ci = 0; ci < vt_apps_category_count(); ci++) {
                int n = vt_apps_in_category(l->apps, ci);
                if (n == 0 && ci != vt_apps_category_count() - 1) continue;
                if (cat_row == r) { picked = ci; break; }
                cat_row++;
            }
            if (picked >= 0) {
                l->cur_cat = picked;
                l->scroll = 0;
            }
            _launcher_menu_paint(&env);
            break;
        }
        int ri = (y - _AM_SEARCH_H - 4) / _AM_ROW;
        const vt_app_t *a = _app_row(l, l->cur_cat, l->search,
                                     (size_t)(l->scroll + ri));
        if (a) {
            char *cmd = vt_apps_launch_cmd(a);
            vt_logi("launcher: spawn '%s' [%s]", cmd,
                    a->terminal ? "terminal" : "direct");
            vt_panel_spawn(cmd);
            vt_free(cmd);
            _launcher_menu_close(&env);
        }
        break;
    }
    default:
        break;
    }
}

static void _launcher_on_click(vt_applet_env_t *env, int x, int y,
                               int button) {
    (void)x; (void)y; (void)button;
    _launcher_t *l = env->state;
    if (l->menu_win) _launcher_menu_close(env);
    else _launcher_menu_open(env);
}

const vt_applet_impl_t _applet_launcher = {
    .name = "launcher",
    .init = _launcher_init,
    .fini = _launcher_fini,
    .measure = _launcher_measure,
    .render = _launcher_render,
    .on_click = _launcher_on_click,
};
