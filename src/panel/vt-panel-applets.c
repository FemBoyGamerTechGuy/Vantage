/*
 * vt-panel-applets.c — Built-in panel applets
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * launcher  — menu of XDG .desktop entries, spawns the selected Exec
 * tasklist  — live window buttons via WM IPC (click: focus/minimize)
 * workspaces— desktop switcher via WM IPC
 * clock     — local time, 1 s tick
 * volume    — ALSA master volume (click: mute toggle)
 * network   — sysfs link state + wireless level
 * battery   — sysfs power supply capacity + status
 * tray      — XEmbed system tray (owns _NET_SYSTEM_TRAY_S<n>)
 */

#define VT_LOG_DOMAIN "panel-applets"
#include "vt-panel-internal.h"
#include <vantage/vt-config.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(VT_HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif

/* ------------------------------------------------------------ launcher */
typedef struct {
    char *name;
    char *exec;
    char *icon;      /* icon name (unused for drawing yet) */
} _desk_entry_t;

typedef struct {
    vt_vec_t entries;     /* _desk_entry_t */
    bool loaded;
    Window menu_win;      /* override-redirect popup */
    XftDraw *menu_draw;
    int  menu_rows;
    int  menu_sel;
    int  scroll;
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
        char *name = NULL, *exec = NULL, *icon = NULL, *nodisplay = NULL,
             *onlyin = NULL;
        char *save = NULL;
        for (char *line = strtok_r(content, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (name && exec && icon) break;
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
            else if (vt_strstartswith(line, "NoDisplay=true"))
                nodisplay = vt_strdup("1");
            else if (vt_strstartswith(line, "OnlyShowIn="))
                onlyin = vt_strdup(line + 12);
        }
        vt_free(content);
        if (nodisplay || !name || !exec) {
            vt_free(name); vt_free(exec); vt_free(icon);
            vt_free(nodisplay); vt_free(onlyin);
            continue;
        }
        if (onlyin && !strstr(onlyin, "Vantage") && !strstr(onlyin, "GNOME")
            && !strstr(onlyin, "XFCE")) {
            vt_free(name); vt_free(exec); vt_free(icon); vt_free(onlyin);
            continue;
        }
        _desk_entry_t e = { .name = name, .exec = _strip_field(exec),
                            .icon = icon };
        vt_vec_push(&l->entries, &e);
        vt_free(nodisplay);
        vt_free(onlyin);
    }
    closedir(d);
}

static void _launcher_init(vt_applet_env_t *env) {
    _launcher_t *l = vt_malloc0(sizeof(*l));
    vt_vec_init(&l->entries, sizeof(_desk_entry_t), 32);
    env->state = l;
}

static void _launcher_fini(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    if (!l) return;
    for (size_t i = 0; i < l->entries.size; i++) {
        _desk_entry_t *e = vt_vec_at(&l->entries, i);
        vt_free(e->name); vt_free(e->exec); vt_free(e->icon);
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
    return 34;
}

static void _launcher_render(vt_applet_env_t *env) {
    vt_pctx_t *ctx = env->ctx;
    int cx = env->area.x + env->area.w / 2;
    int cy = env->area.y + env->area.h / 2;
    /* a simple grid "app launcher" glyph */
    vt_pcol_t fg = { 0xec, 0xee, 0xf0, 0xff };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            vt_pctx_rect(ctx, cx - 7 + c * 6, cy - 7 + r * 6, 4, 4, fg);
}

static void _launcher_menu_paint(vt_applet_env_t *env) {
    _launcher_t *l = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!l->menu_win || !l->menu_draw) return;
    Display *dpy = ctx->dpy;
    int w = 260, row_h = 24;
    int h = l->menu_rows * row_h + 8;
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
        DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, l->menu_win, fmt, 0, NULL);
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)w,
                         (unsigned)h);
    for (int i = 0; i < l->menu_rows; i++) {
        _desk_entry_t *e = vt_vec_at(&l->entries,
                                     (size_t)(i + l->scroll));
        bool sel = (i == l->menu_sel);
        if (sel) {
            XRenderColor hi = { .red = 0x4f4f, .green = 0x9a9a,
                                .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, 2,
                                  (short)(4 + i * row_h), (unsigned)(w - 4),
                                  (unsigned)(row_h - 2));
        }
        XRenderColor tc = sel
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : (XRenderColor){ .red = 0xecec, .green = 0xeeee,
                              .blue = 0xf0f0, .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                      DefaultColormap(dpy, DefaultScreen(dpy)), &tc, &fc);
        XftDrawStringUtf8(l->menu_draw, &fc, ctx->font, 10,
                          4 + i * row_h + row_h - 7,
                          (const FcChar8 *)e->name, (int)strlen(e->name));
    }
    XRenderFreePicture(dpy, pic);
}

static void _launcher_menu_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_LAUNCHER);
    _launcher_t *l = env.state;
    if (!l || ev->xany.window != l->menu_win) return;
    switch (ev->type) {
    case Expose:
        _launcher_menu_paint(&env);
        break;
    case MotionNotify:
        l->menu_sel = (ev->xmotion.y - 4) / 24;
        _launcher_menu_paint(&env);
        break;
    case ButtonRelease: {
        if (ev->xbutton.button == Button1) {
            int idx = (ev->xbutton.y - 4) / 24 + l->scroll;
            if (idx >= 0 && (size_t)idx < l->entries.size) {
                _desk_entry_t *e = vt_vec_at(&l->entries, (size_t)idx);
                vt_logi("launcher: spawn '%s'", e->exec);
                vt_panel_spawn(e->exec);
            }
            _launcher_menu_close(&env);
        } else {
            _launcher_menu_close(&env);
        }
        break;
    }
    default:
        break;
    }
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
    if (l->entries.size > 16) l->menu_rows = 16;
    else l->menu_rows = (int)l->entries.size;
    l->scroll = 0;
    l->menu_sel = -1;
    int w = 260, row_h = 24;
    int h = l->menu_rows * row_h + 8;
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

/* ------------------------------------------------------------ tasklist */
typedef struct {
    uint32_t id;
    char *title;
    int ws;
    bool focused, minimized, maximized, fullscreen, urgent;
    char cls[32];
} _twin_t;

typedef struct {
    vt_vec_t wins;
    int cur_ws;
    int ws_count;
} _tasklist_t;

static void _task_parse_line(const char *line, _twin_t *w) {
    /* format: id\ttitle\tws\tflags\tclass\tappid */
    char *copy = vt_strdup(line);
    char *fields[6] = {0};
    int nf = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, "\t", &save); tok && nf < 6;
         tok = strtok_r(NULL, "\t", &save))
        fields[nf++] = tok;
    if (nf >= 4) {
        w->id = (uint32_t)strtoul(fields[0], NULL, 0);
        w->title = vt_strdup(fields[1] ? fields[1] : "");
        w->ws = atoi(fields[2]);
        const char *fl = fields[3];
        w->focused = strchr(fl, 'F') != NULL;
        w->minimized = strchr(fl, 'M') != NULL;
        w->maximized = strchr(fl, 'X') != NULL;
        w->fullscreen = strchr(fl, 'S') != NULL;
        w->urgent = strchr(fl, 'U') != NULL;
        snprintf(w->cls, sizeof(w->cls), "%s", fields[4] ? fields[4] : "");
    }
    vt_free(copy);
}

static void _tasklist_clear(_tasklist_t *t) {
    for (size_t i = 0; i < t->wins.size; i++) {
        _twin_t *w = vt_vec_at(&t->wins, i);
        vt_free(w->title);
    }
    vt_vec_clear(&t->wins);
}

static void _tasklist_refresh(vt_applet_env_t *env) {
    _tasklist_t *t = env->state;
    char *resp = vt_panel_query_windows(env->panel);
    if (!resp) return;
    _tasklist_clear(t);
    char *save = NULL;
    for (char *line = strtok_r(resp, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        _twin_t w = {0};
        _task_parse_line(line, &w);
        if (w.id) vt_vec_push(&t->wins, &w);
    }
    vt_free(resp);
    char *ws = vt_panel_query_workspaces(env->panel);
    if (ws) {
        char *p = strstr(ws, "current=");
        if (p) t->cur_ws = atoi(p + 8);
        vt_free(ws);
    }
    vt_panel_invalidate(env->panel);
}

static void _tasklist_init(vt_applet_env_t *env) {
    _tasklist_t *t = vt_malloc0(sizeof(*t));
    vt_vec_init(&t->wins, sizeof(_twin_t), 8);
    env->state = t;
    _tasklist_refresh(env);
}

static void _tasklist_fini(vt_applet_env_t *env) {
    _tasklist_t *t = env->state;
    if (!t) return;
    _tasklist_clear(t);
    vt_vec_fini(&t->wins);
    vt_free(t);
}

#define TASK_BTN_W 150
static int _tasklist_measure(vt_applet_env_t *env) {
    _tasklist_t *t = env->state;
    size_t n = 0;
    for (size_t i = 0; i < t->wins.size; i++) {
        _twin_t *w = vt_vec_at(&t->wins, i);
        if (w->ws == t->cur_ws || w->urgent) n++;
    }
    int w = (int)n * (TASK_BTN_W + 6);
    return w > 0 ? w : 4;
}

static void _tasklist_render(vt_applet_env_t *env) {
    _tasklist_t *t = env->state;
    vt_pctx_t *ctx = env->ctx;
    int x = env->area.x;
    int h = env->area.h;
    for (size_t i = 0; i < t->wins.size; i++) {
        _twin_t *w = vt_vec_at(&t->wins, i);
        if (w->ws != t->cur_ws && !w->urgent) continue;
        int bw = TASK_BTN_W;
        vt_pcol_t bg;
        if (w->focused)      bg = (vt_pcol_t){ 0x39, 0x3e, 0x48, 0xff };
        else if (w->urgent)  bg = (vt_pcol_t){ 0xa8, 0x54, 0x30, 0xff };
        else                 bg = (vt_pcol_t){ 0x26, 0x28, 0x2e, 0xff };
        vt_pctx_rounded_rect(ctx, x, env->area.y, bw - 6, h, 4, bg);
        vt_pcol_t tc = w->focused ? (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff }
                                  : (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff };
        if (w->minimized) tc.a = 0x90;
        char *label = vt_strdup(w->title ? w->title : "");
        int tw = vt_pctx_text_width(ctx, label, false);
        if (tw > bw - 24) {
            /* truncate with ellipsis */
            while (tw > bw - 30 && *label) {
                size_t ll = strlen(label);
                if (ll < 4) break;
                label[ll - 1] = 0;
                strcat(label, "…");
                tw = vt_pctx_text_width(ctx, label, false);
            }
        }
        int ty = env->area.y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
        vt_pctx_text(ctx, x + 10, ty, label, false, tc);
        vt_free(label);
        w->id = w->id; /* keep */
        x += bw;
    }
}

static void _tasklist_on_click(vt_applet_env_t *env, int x, int y,
                               int button) {
    (void)y;
    _tasklist_t *t = env->state;
    int idx = x / TASK_BTN_W;
    int n = 0;
    for (size_t i = 0; i < t->wins.size; i++) {
        _twin_t *w = vt_vec_at(&t->wins, i);
        if (w->ws != t->cur_ws && !w->urgent) continue;
        if (n == idx) {
            char payload[64];
            if (button == 2 || (button == 1 && w->focused)) {
                snprintf(payload, sizeof(payload), "id=%u", w->id);
                vt_panel_send_wm(env->panel, VT_IPC_MSG_WM_MINIMIZE, payload);
            } else if (button == 3) {
                snprintf(payload, sizeof(payload), "id=%u", w->id);
                vt_panel_send_wm(env->panel, VT_IPC_MSG_WM_CLOSE, payload);
            } else {
                snprintf(payload, sizeof(payload), "id=%u", w->id);
                vt_panel_send_wm(env->panel, VT_IPC_MSG_WM_FOCUS, payload);
            }
            return;
        }
        n++;
    }
}

static void _tasklist_on_ipc(vt_applet_env_t *env, uint32_t msg,
                             const char *payload) {
    if (msg == VT_IPC_MSG_WM_EVENT || msg == VT_IPC_MSG_WM_WS_EVENT)
        _tasklist_refresh(env);
}

const vt_applet_impl_t _applet_tasklist = {
    .name = "tasklist",
    .init = _tasklist_init,
    .fini = _tasklist_fini,
    .measure = _tasklist_measure,
    .render = _tasklist_render,
    .on_click = _tasklist_on_click,
    .on_ipc_event = _tasklist_on_ipc,
};

/* ---------------------------------------------------------- workspaces */
typedef struct {
    int count;
    int current;
} _wsp_t;

static void _wsp_refresh(vt_applet_env_t *env) {
    _wsp_t *w = env->state;
    char *resp = vt_panel_query_workspaces(env->panel);
    if (!resp) return;
    char *p = strstr(resp, "count=");
    if (p) w->count = atoi(p + 6);
    p = strstr(resp, "current=");
    if (p) w->current = atoi(p + 8);
    vt_free(resp);
}

static void _wsp_init(vt_applet_env_t *env) {
    _wsp_t *w = vt_malloc0(sizeof(*w));
    w->count = 4;
    w->current = 0;
    env->state = w;
    _wsp_refresh(env);
}
static void _wsp_fini(vt_applet_env_t *env) { vt_free(env->state); }

#define WSP_BTN 22
static int _wsp_measure(vt_applet_env_t *env) {
    _wsp_t *w = env->state;
    return w->count * (WSP_BTN + 4);
}
static void _wsp_render(vt_applet_env_t *env) {
    _wsp_t *w = env->state;
    vt_pctx_t *ctx = env->ctx;
    for (int i = 0; i < w->count; i++) {
        int x = env->area.x + i * (WSP_BTN + 4);
        vt_pcol_t bg = (i == w->current)
            ? (vt_pcol_t){ 0x4f, 0x9a, 0xdc, 0xff }
            : (vt_pcol_t){ 0x26, 0x28, 0x2e, 0xff };
        vt_pctx_rounded_rect(ctx, x, env->area.y, WSP_BTN, env->area.h, 4, bg);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        int ty = env->area.y + env->area.h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
        vt_pctx_text(ctx, x + WSP_BTN / 2 - 3, ty, n, false,
                     (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
    }
}
static void _wsp_on_click(vt_applet_env_t *env, int x, int y, int button) {
    (void)y; (void)button;
    _wsp_t *w = env->state;
    int idx = x / (WSP_BTN + 4);
    if (idx < 0 || idx >= w->count) return;
    char payload[32];
    snprintf(payload, sizeof(payload), "ws=%d", idx);
    vt_panel_send_wm(env->panel, VT_IPC_MSG_WM_WS_SWITCH, payload);
}
static void _wsp_on_ipc(vt_applet_env_t *env, uint32_t msg,
                        const char *payload) {
    if (msg == VT_IPC_MSG_WM_WS_EVENT && payload &&
        vt_strstartswith(payload, "workspace-changed")) {
        _wsp_t *w = env->state;
        w->current = atoi(payload + strlen("workspace-changed"));
        vt_panel_invalidate(env->panel);
    }
}
const vt_applet_impl_t _applet_workspaces = {
    .name = "workspaces", .init = _wsp_init, .fini = _wsp_fini,
    .measure = _wsp_measure, .render = _wsp_render,
    .on_click = _wsp_on_click, .on_ipc_event = _wsp_on_ipc,
};

/* --------------------------------------------------------------- clock */
static void _clock_render(vt_applet_env_t *env) {
    vt_pctx_t *ctx = env->ctx;
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (env->area.w > 120)
        strftime(buf, sizeof(buf), "%a %d %b  %H:%M", &tm);
    else
        strftime(buf, sizeof(buf), "%H:%M", &tm);
    int tw = vt_pctx_text_width(ctx, buf, false);
    int ty = env->area.y + env->area.h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
    vt_pctx_text(ctx, env->area.x + env->area.w - tw - 4, ty, buf, false,
                 (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
}
static int _clock_measure(vt_applet_env_t *env) {
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%a %d %b  %H:%M", &tm);
    return vt_pctx_text_width(env->ctx, buf, false) + 10;
}
const vt_applet_impl_t _applet_clock = {
    .name = "clock", .render = _clock_render, .measure = _clock_measure,
};

/* -------------------------------------------------------------- volume */
typedef struct {
    int level;      /* 0..100 */
    bool muted;
    bool ok;
} _vol_t;

#if defined(VT_HAVE_ALSA)
static bool _vol_read(_vol_t *v) {
    long lv = 0;
    int mute = 0;
    snd_mixer_t *h = NULL;
    if (snd_mixer_open(&h, 0) < 0) return false;
    if (snd_mixer_attach(h, "default") < 0 || snd_mixer_selem_register(h, NULL, NULL) < 0
        || snd_mixer_load(h) < 0) {
        snd_mixer_close(h);
        return false;
    }
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *el = snd_mixer_find_selem(h, sid);
    if (!el) { snd_mixer_close(h); return false; }
    long mn = 0, mx = 0;
    snd_mixer_selem_get_playback_volume_range(el, &mn, &mx);
    snd_mixer_selem_get_playback_volume(el, 0, &lv);
    if (snd_mixer_selem_has_playback_switch(el))
        snd_mixer_selem_get_playback_switch(el, 0, &mute);
    v->level = mx > mn ? (int)((lv - mn) * 100 / (mx - mn)) : 0;
    v->muted = !mute;
    snd_mixer_close(h);
    v->ok = true;
    return true;
}
static void _vol_toggle(void) {
    snd_mixer_t *h = NULL;
    if (snd_mixer_open(&h, 0) < 0) return;
    if (snd_mixer_attach(h, "default") < 0 || snd_mixer_selem_register(h, NULL, NULL) < 0
        || snd_mixer_load(h) < 0) {
        snd_mixer_close(h);
        return;
    }
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *el = snd_mixer_find_selem(h, sid);
    if (el && snd_mixer_selem_has_playback_switch(el)) {
        int cur = 0;
        snd_mixer_selem_get_playback_switch(el, 0, &cur);
        snd_mixer_selem_set_playback_switch_all(el, !cur);
    }
    snd_mixer_close(h);
}
#else
static bool _vol_read(_vol_t *v) { (void)v; return false; }
static void _vol_toggle(void) {}
#endif

static void _vol_render(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!v->ok) return;
    char buf[24];
    if (v->muted) snprintf(buf, sizeof(buf), "vol %d%% [muted]", v->level);
    else snprintf(buf, sizeof(buf), "vol %d%%", v->level);
    int tw = vt_pctx_text_width(ctx, buf, false);
    int ty = env->area.y + env->area.h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
    vt_pctx_text(ctx, env->area.x + env->area.w - tw, ty, buf, false,
                 (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
}
static int _vol_measure(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    return v->ok ? 78 : 0;
}
static void _vol_tick(vt_applet_env_t *env, uint64_t now) {
    (void)now;
    _vol_t *v = env->state;
    _vol_read(v);
}
static void _vol_click(vt_applet_env_t *env, int x, int y, int button) {
    (void)env; (void)x; (void)y;
    if (button == 1) _vol_toggle();
}
static void _vol_init(vt_applet_env_t *env) {
    _vol_t *v = vt_malloc0(sizeof(*v));
    env->state = v;
    _vol_read(v);
}
const vt_applet_impl_t _applet_volume = {
    .name = "volume", .init = _vol_init, .render = _vol_render,
    .measure = _vol_measure, .on_tick = _vol_tick, .on_click = _vol_click,
};

/* ------------------------------------------------------------- network */
typedef struct { bool up; char name[16]; int wifi; } _net_t;

static void _net_read(_net_t *n) {
    n->up = false;
    n->wifi = -1;
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (vt_streq(de->d_name, "lo")) continue;
        char *p = vt_strprintf("/sys/class/net/%s/operstate", de->d_name);
        size_t len = 0;
        char *state = vt_file_read_all(p, &len);
        vt_free(p);
        if (state && vt_strstartswith(state, "up")) {
            n->up = true;
            snprintf(n->name, sizeof(n->name), "%s", de->d_name);
            /* wireless quality */
            p = vt_strprintf("/proc/net/wireless");
            char *w = vt_file_read_all(p, &len);
            vt_free(p);
            if (w) {
                char *save = NULL;
                for (char *line = strtok_r(w, "\n", &save); line;
                     line = strtok_r(NULL, "\n", &save)) {
                    if (strstr(line, de->d_name)) {
                        /* iface: link level noise — take 3rd field */
                        int link = 0;
                        char tmp[256];
                        snprintf(tmp, sizeof(tmp), "%s", line);
                        char *sp = NULL;
                        char *tok = strtok_r(tmp, ": ", &sp); /* iface */
                        (void)tok;
                        tok = strtok_r(NULL, " ", &sp);        /* status */
                        (void)tok;
                        tok = strtok_r(NULL, " ", &sp);        /* link */
                        if (tok) link = atoi(tok);
                        n->wifi = link > 0 ? (link * 100) / 70 : 0;
                        break;
                    }
                }
                vt_free(w);
            }
        }
        vt_free(state);
        if (n->up) break;
    }
    closedir(d);
}
static void _net_render(vt_applet_env_t *env) {
    _net_t *n = env->state;
    vt_pctx_t *ctx = env->ctx;
    char buf[32];
    if (!n->up) snprintf(buf, sizeof(buf), "net: off");
    else if (n->wifi >= 0) snprintf(buf, sizeof(buf), "wifi %d%%", n->wifi);
    else snprintf(buf, sizeof(buf), "net: %s", n->name);
    vt_pcol_t c = n->up ? (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff }
                        : (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff };
    int tw = vt_pctx_text_width(ctx, buf, false);
    int ty = env->area.y + env->area.h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
    vt_pctx_text(ctx, env->area.x + env->area.w - tw, ty, buf, false, c);
}
static int _net_measure(vt_applet_env_t *env) {
    _net_t *n = env->state;
    return n->up ? (n->wifi >= 0 ? 68 : 60) : 48;
}
static void _net_tick(vt_applet_env_t *env, uint64_t now) {
    (void)now;
    _net_read(env->state);
}
static void _net_init(vt_applet_env_t *env) {
    _net_t *n = vt_malloc0(sizeof(*n));
    env->state = n;
    _net_read(n);
}
const vt_applet_impl_t _applet_network = {
    .name = "network", .init = _net_init, .render = _net_render,
    .measure = _net_measure, .on_tick = _net_tick,
};

/* ------------------------------------------------------------- battery */
typedef struct { int pct; bool charging; bool present; } _bat_t;

static void _bat_read(_bat_t *b) {
    b->present = false;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!vt_strstartswith(de->d_name, "BAT")) continue;
        char *p = vt_strprintf("/sys/class/power_supply/%s/capacity",
                               de->d_name);
        size_t len = 0;
        char *c = vt_file_read_all(p, &len);
        vt_free(p);
        if (c) {
            b->present = true;
            b->pct = atoi(c);
            vt_free(c);
        }
        p = vt_strprintf("/sys/class/power_supply/%s/status", de->d_name);
        char *st = vt_file_read_all(p, &len);
        vt_free(p);
        if (st) {
            b->charging = vt_strstartswith(st, "Charging");
            vt_free(st);
        }
        break;
    }
    closedir(d);
}
static void _bat_render(vt_applet_env_t *env) {
    _bat_t *b = env->state;
    if (!b->present) return;
    vt_pctx_t *ctx = env->ctx;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%d%%", b->charging ? "chg " : "", b->pct);
    vt_pcol_t c = b->pct <= 15 ? (vt_pcol_t){ 0xe0, 0x5a, 0x50, 0xff }
                               : (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff };
    int tw = vt_pctx_text_width(ctx, buf, false);
    int ty = env->area.y + env->area.h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
    vt_pctx_text(ctx, env->area.x + env->area.w - tw, ty, buf, false, c);
}
static int _bat_measure(vt_applet_env_t *env) {
    _bat_t *b = env->state;
    return b->present ? 54 : 0;
}
static void _bat_tick(vt_applet_env_t *env, uint64_t now) {
    (void)now;
    _bat_read(env->state);
}
static void _bat_init(vt_applet_env_t *env) {
    _bat_t *b = vt_malloc0(sizeof(*b));
    env->state = b;
    _bat_read(b);
}
const vt_applet_impl_t _applet_battery = {
    .name = "battery", .init = _bat_init, .render = _bat_render,
    .measure = _bat_measure, .on_tick = _bat_tick,
};

/* ----------------------------------------------------------------- tray */
/* XEmbed system tray: owns the _NET_SYSTEM_TRAY_S<screen> selection and
 * parents dock requests into a small embedding area. */
typedef struct {
    Window owner;
    Atom sel_atom;
    bool have;
} _tray_t;

static void _tray_init(vt_applet_env_t *env) {
    _tray_t *t = vt_malloc0(sizeof(*t));
    vt_pctx_t *ctx = env->ctx;
    char selname[64];
    snprintf(selname, sizeof(selname), "_NET_SYSTEM_TRAY_S%d",
             DefaultScreen(ctx->dpy));
    t->sel_atom = XInternAtom(ctx->dpy, selname, False);
    t->owner = XCreateSimpleWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                                   -10, -10, 1, 1, 0, 0, 0);
    XSetSelectionOwner(ctx->dpy, t->sel_atom, t->owner, CurrentTime);
    if (XGetSelectionOwner(ctx->dpy, t->sel_atom) == t->owner) {
        t->have = true;
        /* announce ourselves per the systray spec */
        XClientMessageEvent m = {0};
        m.type = ClientMessage;
        m.window = DefaultRootWindow(ctx->dpy);
        m.message_type = XInternAtom(ctx->dpy, "MANAGER", False);
        m.format = 32;
        m.data.l[0] = CurrentTime;
        m.data.l[1] = (long)t->sel_atom;
        m.data.l[2] = (long)t->owner;
        XSendEvent(ctx->dpy, DefaultRootWindow(ctx->dpy), False,
                   StructureNotifyMask, (XEvent *)&m);
        vt_logi("tray: manager selection acquired");
    } else {
        vt_logw("tray: another system tray owns the selection");
    }
    env->state = t;
}
static void _tray_fini(vt_applet_env_t *env) {
    _tray_t *t = env->state;
    if (!t) return;
    vt_pctx_t *ctx = env->ctx;
    if (t->have && t->owner)
        XSetSelectionOwner(ctx->dpy, t->sel_atom, None, CurrentTime);
    if (t->owner) XDestroyWindow(ctx->dpy, t->owner);
    vt_free(t);
}
static int _tray_measure(vt_applet_env_t *env) { (void)env; return 24; }
static void _tray_render(vt_applet_env_t *env) {
    (void)env;
    /* tray icons are embedded X windows; nothing to paint ourselves */
}
const vt_applet_impl_t _applet_tray = {
    .name = "tray", .init = _tray_init, .fini = _tray_fini,
    .measure = _tray_measure, .render = _tray_render,
};
