/* ------------------------------------------------------------ launcher */
/* The Vantage start button: opens the application menu built from XDG
 * .desktop entries, grouped into the standard categories, plus a
 * Quit Session entry that opens the session actions menu. */

#define VT_LOG_DOMAIN "panel-applets"
#include "vt-panel-internal.h"
#include <vantage/vt-config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <vantage/vt-integrations.h>

typedef struct {
    char *name;
    char *exec;
    char *icon;
    char *category;     /* display category ("Internet", ...) */
} _desk_entry_t;

/* Category table: .desktop Categories= keyword -> display name. Order
 * defines the menu order; anything unmatched lands in "Other". */
static const struct { const char *key; const char *label; } _cat_table[] = {
    { "Utility",     "Utilities"  },
    { "Development", "Development"},
    { "Education",   "Education"  },
    { "Science",     "Education"  },
    { "Game",        "Games"      },
    { "Graphics",    "Graphics"   },
    { "Network",     "Internet"   },
    { "AudioVideo",  "Multimedia" },
    { "Audio",       "Multimedia" },
    { "Video",       "Multimedia" },
    { "Office",      "Office"     },
    { "System",      "System"     },
    { "Settings",    "System"     },
    { "Core",        "System"     },
};
#define _N_CATS ((int)(sizeof(_cat_table) / sizeof(_cat_table[0])))

static const char *const _cat_order[] = {
    "Accessories2", /* replaced at runtime: see below */
};
/* full ordered category list shown in the menu */
static const char *const _menu_cats[] = {
    "Accessories", "Development", "Education", "Games", "Graphics",
    "Internet", "Multimedia", "Office", "System", "Utilities", "Other",
};
#define _N_MENU_CATS ((int)(sizeof(_menu_cats) / sizeof(_menu_cats[0])))

static const char *_category_of(const char *desktop_categories) {
    if (!desktop_categories || !*desktop_categories) return "Other";
    for (int i = 0; i < _N_CATS; i++)
        if (strstr(desktop_categories, _cat_table[i].key))
            return _cat_table[i].label;
    return "Other";
}

/* display category for a Utility-only entry: Utilities; with
 * AudioVideo etc. handled above, plain "Utility" goes to Accessories
 * unless it also carries Terminal (then Utilities). */
static const char *_category_detailed(const char *cats, bool terminal) {
    if (!cats || !*cats) return "Other";
    /* Utility+something-else is handled by the table order */
    for (int i = 0; i < _N_CATS; i++)
        if (strstr(cats, _cat_table[i].key))
            return _cat_table[i].label;
    if (strstr(cats, "Utility")) return terminal ? "Utilities" : "Accessories";
    return "Other";
}

typedef struct {
    vt_vec_t entries;     /* _desk_entry_t */
    bool loaded;
    Window menu_win;      /* override-redirect popup */
    XftDraw *menu_draw;
    int  menu_rows;       /* visible app rows */
    int  menu_sel;        /* hovered app row, -1 none */
    int  menu_cat;        /* hovered category row, -1 none */
    int  cur_cat;         /* selected category index */
    int  scroll;          /* first visible app row */
} _launcher_t;

static void _launcher_menu_paint(vt_applet_env_t *env);
static void _launcher_menu_handle(vt_panel_t *p, XEvent *ev);
static void _launcher_menu_close(vt_applet_env_t *env);
static void _launcher_menu_open(vt_applet_env_t *env);

static char *_strip_field(char *s) {
    /* cut at %U/%u/%F/%f placeholders */
    char *p = strstr(s, "%");
    if (p) *p = 0;
    /* trim */
    return vt_strtrim(s);
}

static int _desk_cmp(const void *a, const void *b);

static void _load_desktop_dir(_launcher_t *l, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!vt_strendswith(de->d_name, ".desktop")) continue;
        char *path = vt_strprintf("%s/%s", dir, de->d_name);
        size_t len = 0;
        char *content = vt_file_read_all(path, &len);
        vt_free(path);
        if (!content) continue;
        char *name = NULL, *exec = NULL, *icon = NULL, *cats = NULL,
             *nodisplay = NULL, *onlyin = NULL, *terminal = NULL;
        char *save = NULL;
        for (char *line = strtok_r(content, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (name && exec && cats) break;
            if (vt_strstartswith(line, "Name=") && !name)
                name = vt_strdup(line + 5);
            else if (vt_strstartswith(line, "Name[") && !name) {
                char *eq = strchr(line, '=');
                if (eq) name = vt_strdup(eq + 1);
            }
            else if (vt_strstartswith(line, "Exec=") && !exec)
                exec = vt_strdup(line + 5);
            else if (vt_strstartswith(line, "Icon=") && !icon)
                icon = vt_strdup(line + 5);
            else if (vt_strstartswith(line, "Categories=") && !cats)
                cats = vt_strdup(line + 11);
            else if (vt_strstartswith(line, "NoDisplay=true"))
                nodisplay = vt_strdup("1");
            else if (vt_strstartswith(line, "Terminal=true"))
                terminal = vt_strdup("1");
            else if (vt_strstartswith(line, "OnlyShowIn="))
                onlyin = vt_strdup(line + 12);
        }
        vt_free(content);
        if (nodisplay || !name || !exec) {
            vt_free(name); vt_free(exec); vt_free(icon); vt_free(cats);
            vt_free(nodisplay); vt_free(onlyin); vt_free(terminal);
            continue;
        }
        if (onlyin && !strstr(onlyin, "Vantage") && !strstr(onlyin, "GNOME")
            && !strstr(onlyin, "XFCE")) {
            vt_free(name); vt_free(exec); vt_free(icon); vt_free(cats);
            vt_free(onlyin); vt_free(terminal);
            continue;
        }
        _desk_entry_t e = { .name = name, .exec = _strip_field(exec),
                            .icon = icon,
                            .category = vt_strdup(_category_detailed(
                                cats, terminal != NULL)) };
        vt_free(cats);
        vt_free(terminal);
        vt_vec_push(&l->entries, &e);
        vt_free(nodisplay);
        vt_free(onlyin);
    }
    closedir(d);
}

static void _launcher_init(vt_applet_env_t *env) {
    _launcher_t *l = vt_malloc0(sizeof(*l));
    vt_vec_init(&l->entries, sizeof(_desk_entry_t), 32);
    l->menu_sel = -1;
    l->menu_cat = -1;
    l->cur_cat = 0;
    env->state = l;
}

static void _launcher_fini(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    if (!l) return;
    for (size_t i = 0; i < l->entries.size; i++) {
        _desk_entry_t *e = vt_vec_at(&l->entries, i);
        vt_free(e->name); vt_free(e->exec); vt_free(e->icon);
        vt_free(e->category);
    }
    vt_vec_fini(&l->entries);
    vt_free(l);
}

static void _launcher_load(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    if (l->loaded) return;
    l->loaded = true;
    char *user = vt_strprintf("%s/.local/share/applications", vt_home_dir());
    _load_desktop_dir(l, "/usr/share/applications");
    _load_desktop_dir(l, "/usr/local/share/applications");
    _load_desktop_dir(l, user);
    vt_free(user);
    vt_vec_sort(&l->entries, _desk_cmp);
    vt_logi("launcher: %zu applications", l->entries.size);
}

static int _desk_cmp(const void *a, const void *b) {
    const _desk_entry_t *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

static int _launcher_measure(vt_applet_env_t *env) {
    _launcher_load(env);
    return 92;
}

/* count entries in a display category */
static int _cat_count(_launcher_t *l, const char *cat) {
    int n = 0;
    for (size_t i = 0; i < l->entries.size; i++) {
        _desk_entry_t *e = vt_vec_at(&l->entries, i);
        if (vt_streq(e->category, cat)) n++;
    }
    return n;
}

static void _launcher_render(vt_applet_env_t *env) {
    vt_pctx_t *ctx = env->ctx;
    int x = env->area.x, w = env->area.w, h = env->area.h;
    int y = env->area.y;
    /* Vantage start button: accent rounded square + V glyph + label */
    vt_pcol_t accent = { 0x4f, 0x9a, 0xdc, 0xff };
    vt_pcol_t fg = { 0xec, 0xee, 0xf0, 0xff };
    vt_pctx_rounded_rect(ctx, x + 6, y + 3, 22, h - 6, 6, accent);
    /* the V: two diagonal strokes */
    for (int i = 0; i < 5; i++) {
        vt_pctx_rect(ctx, x + 6 + 5 + i, y + 3 + 4 + i, 2, 2, fg);
        vt_pctx_rect(ctx, x + 6 + 15 - i, y + 3 + 4 + i, 2, 2, fg);
    }
    vt_pctx_rect(ctx, x + 6 + 9, y + 3 + 13, 2, 2, fg);
    vt_pctx_text(ctx, x + 34, y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2,
                 "Vantage", true, fg);
}

/* ---- the application menu (two panes: categories | applications) ---- */
#define _AM_CAT_W   120
#define _AM_ROW     26
#define _AM_W       480
#define _AM_ROWS    14

static void _launcher_menu_paint(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!l->menu_win || !l->menu_draw) return;
    Display *dpy = ctx->dpy;
    int w = _AM_W;
    int h = _AM_ROWS * _AM_ROW + 8;
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
        DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, l->menu_win, fmt, 0, NULL);
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)w,
                         (unsigned)h);
    /* separator between the panes */
    XRenderColor sep = { .red = 0x3939, .green = 0x3e3e, .blue = 0x4848,
                         .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &sep, _AM_CAT_W, 0, 1,
                         (unsigned)h);

    /* category pane */
    for (int i = 0; i < _N_MENU_CATS; i++) {
        int n = _cat_count(l, _menu_cats[i]);
        if (n == 0 && i != _N_MENU_CATS - 1) continue;   /* skip empty */
        int row_y = 4 + i * _AM_ROW;
        if (row_y + _AM_ROW > h) break;
        bool active = (i == l->cur_cat);
        if (active) {
            XRenderColor hi = { .red = 0x4f4f, .green = 0x9a9a,
                                .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, 2,
                                  (short)row_y, (unsigned)(_AM_CAT_W - 4),
                                  (unsigned)(_AM_ROW - 2));
        }
        XRenderColor tc = active
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : (XRenderColor){ .red = 0xecec, .green = 0xeeee,
                              .blue = 0xf0f0, .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                      DefaultColormap(dpy, DefaultScreen(dpy)), &tc, &fc);
        XftDrawStringUtf8(l->menu_draw, &fc, active ? ctx->font_bold
                                                    : ctx->font, 10,
                          row_y + _AM_ROW - 7,
                          (const FcChar8 *)_menu_cats[i],
                          (int)strlen(_menu_cats[i]));
        /* count badge */
        char badge[16];
        snprintf(badge, sizeof(badge), "%d", n);
        XRenderColor dim = { .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                             .alpha = 0xffff };
        XftColor fcd;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &dim,
                           &fcd);
        XftDrawStringUtf8(l->menu_draw, &fcd, ctx->font,
                          _AM_CAT_W - 8 - (int)strlen(badge) * 7,
                          row_y + _AM_ROW - 7, (const FcChar8 *)badge,
                          (int)strlen(badge));
        (void)fcd;
    }

    /* applications pane */
    const char *cat = _menu_cats[l->cur_cat];
    int row = 0;
    for (size_t i = 0; i < l->entries.size && row < _AM_ROWS; i++) {
        _desk_entry_t *e = vt_vec_at(&l->entries, i);
        if (!vt_streq(e->category, cat)) continue;
        int ry = 4 + row * _AM_ROW;
        bool sel = (row == l->menu_sel);
        if (sel) {
            XRenderColor hi = { .red = 0x3939, .green = 0x3e3e,
                                .blue = 0x4848, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, _AM_CAT_W + 2,
                                  (short)ry,
                                  (unsigned)(w - _AM_CAT_W - 4),
                                  (unsigned)(_AM_ROW - 2));
        }
        XRenderColor tc = sel
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : (XRenderColor){ .red = 0xecec, .green = 0xeeee,
                              .blue = 0xf0f0, .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                      DefaultColormap(dpy, DefaultScreen(dpy)), &tc, &fc);
        XftDrawStringUtf8(l->menu_draw, &fc, ctx->font, _AM_CAT_W + 10,
                          ry + _AM_ROW - 7, (const FcChar8 *)e->name,
                          (int)strlen(e->name));
        row++;
    }
    if (row == 0) {
        XRenderColor dim = { .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                             .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &dim,
                           &fc);
        const char *msg = "(no applications)";
        XftDrawStringUtf8(l->menu_draw, &fc, ctx->font, _AM_CAT_W + 10,
                          4 + _AM_ROW - 7, (const FcChar8 *)msg,
                          (int)strlen(msg));
    }
    XRenderFreePicture(dpy, pic);
}

/* the Quit Session row (bottom of the category pane) */
#define _AM_QUIT_Y(h_) ((h_) - _AM_ROW - 4)

static void _launcher_menu_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_LAUNCHER);
    _launcher_t *l = env.state;
    if (!l || ev->xany.window != l->menu_win) return;
    int w = _AM_W, h = _AM_ROWS * _AM_ROW + 8;
    switch (ev->type) {
    case Expose:
        _launcher_menu_paint(&env);
        break;
    case MotionNotify: {
        int x = ev->xmotion.x, y = ev->xmotion.y;
        if (x < _AM_CAT_W) {
            int ci = (y - 4) / _AM_ROW;
            l->menu_cat = (ci >= 0 && ci < _N_MENU_CATS) ? ci : -1;
            l->menu_sel = -1;
            if (l->menu_cat >= 0 && _cat_count(l, _menu_cats[l->menu_cat]) > 0)
                l->cur_cat = l->menu_cat;
        } else {
            int ri = (y - 4) / _AM_ROW;
            l->menu_sel = (ri >= 0 && ri < _AM_ROWS) ? ri : -1;
            l->menu_cat = -1;
        }
        _launcher_menu_paint(&env);
        break;
    }
    case ButtonRelease: {
        if (ev->xbutton.button == Button1) {
            int x = ev->xbutton.x, y = ev->xbutton.y;
            if (y >= _AM_QUIT_Y(h)) {
                /* Quit Session -> open the session actions menu */
                _launcher_menu_close(&env);
                vt_panel_user_menu_open(p);
                break;
            }
            if (x < _AM_CAT_W) {
                int ci = (y - 4) / _AM_ROW;
                if (ci >= 0 && ci < _N_MENU_CATS &&
                    (_cat_count(l, _menu_cats[ci]) > 0 || ci == _N_MENU_CATS - 1)) {
                    l->cur_cat = ci;
                    l->scroll = 0;
                }
                _launcher_menu_paint(&env);
                break;
            }
            int ri = (y - 4) / _AM_ROW;
            const char *cat = _menu_cats[l->cur_cat];
            int row = 0;
            for (size_t i = 0; i < l->entries.size && row <= ri; i++) {
                _desk_entry_t *e = vt_vec_at(&l->entries, i);
                if (!vt_streq(e->category, cat)) continue;
                if (row == ri) {
                    vt_logi("launcher: spawn '%s'", e->exec);
                    vt_panel_spawn(e->exec);
                    _launcher_menu_close(&env);
                    break;
                }
                row++;
            }
        } else {
            _launcher_menu_close(&env);
        }
        break;
    }
    default:
        break;
    }
    (void)w;
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

static void _launcher_menu_open(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (l->menu_win) return;
    _launcher_load(env);
    if (l->entries.size == 0) return;
    l->menu_rows = _AM_ROWS;
    l->scroll = 0;
    l->menu_sel = -1;
    l->menu_cat = -1;
    int w = _AM_W, h = _AM_ROWS * _AM_ROW + 8;
    int x = env->area.x;
    int y = env->area.y + env->area.h + 4;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask };
    l->menu_win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                                x, y, (unsigned)w, (unsigned)h, 1,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel |
                                CWEventMask, &wa);
    l->menu_draw = XftDrawCreate(ctx->dpy, l->menu_win,
                                 DefaultVisual(ctx->dpy,
                                               DefaultScreen(ctx->dpy)),
                                 DefaultColormap(ctx->dpy,
                                                 DefaultScreen(ctx->dpy)));
    XMapWindow(ctx->dpy, l->menu_win);
    vt_panel_set_popup(env->panel, l->menu_win,
                       _launcher_menu_handle);
    XFlush(ctx->dpy);
    _launcher_menu_paint(env);
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

