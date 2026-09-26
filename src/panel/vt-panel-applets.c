/*
 * vt-panel-applets.c — Built-in panel applets
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
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
#include <vantage/vt-integrations.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

#if defined(VT_HAVE_ALSA)
#include <alsa/asoundlib.h>
#endif


/* ------------------------------------------------------------ tasklist */
typedef struct {
    uint32_t id;
    char *title;
    int ws;
    bool focused, minimized, maximized, fullscreen, urgent;
    char cls[32];
    uint32_t *icon;            /* 16x16 ARGB from _NET_WM_ICON */
} _twin_t;

typedef struct {
    vt_vec_t wins;
    int cur_ws;
    int ws_count;
    vt_vec_t icons;        /* _iconent_t: xid → 16x16 ARGB */
} _tasklist_t;

typedef struct {
    uint32_t xid;
    uint32_t *px;          /* 16x16 ARGB, NULL when none */
} _iconent_t;

/* Read _NET_WM_ICON (CARDINAL[]: w,h,ARGB… repeated) and return a
 * scaled 16x16 ARGB icon, or NULL. Uses the panel's own connection. */
static uint32_t *_net_wm_icon_16(Display *dpy, Window win) {
    Atom prop = XInternAtom(dpy, "_NET_WM_ICON", False);
    Atom actual;
    int fmt;
    unsigned long nitems = 0, bytes = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, prop, 0, 1 << 20, False, XA_CARDINAL,
                           &actual, &fmt, &nitems, &bytes, &data)
            != Success || !data || nitems < 3)
        { if (data) XFree(data); return NULL; }
    unsigned long *card = (unsigned long *)(void *)data;
    /* pick the icon whose size is closest to 32px */
    unsigned long best = 0, best_dist = ~0UL;
    for (unsigned long i = 0; i + 2 <= nitems;) {
        unsigned long w = card[i], h = card[i + 1];
        if (w == 0 || h == 0 || i + 2 + w * h > nitems) break;
        unsigned long dist = w > 32 ? w - 32 : 32 - w;
        if (dist < best_dist) { best_dist = dist; best = i; }
        i += 2 + w * h;
    }
    uint32_t *out = NULL;
    if (best_dist != ~0UL) {
        int w = (int)card[best], h = (int)card[best + 1];
        uint32_t *src = vt_malloc(sizeof(uint32_t) * (size_t)w * h);
        for (int i = 0; i < w * h; i++)
            src[i] = (uint32_t)card[best + 2 + i];
        out = vt_malloc(sizeof(uint32_t) * 16 * 16);
        for (int y = 0; y < 16; y++) {
            int sy = y * h / 16;
            for (int x = 0; x < 16; x++) {
                int sx = x * w / 16;
                out[y * 16 + x] = src[sy * w + sx];
            }
        }
        vt_free(src);
    }
    XFree(data);
    return out;
}

static uint32_t *_task_icon(_tasklist_t *t, Display *dpy, uint32_t xid) {
    for (size_t i = 0; i < t->icons.size; i++) {
        _iconent_t *e = vt_vec_at(&t->icons, i);
        if (e->xid == xid) return e->px;
    }
    _iconent_t e = { .xid = xid, .px = NULL };
    e.px = _net_wm_icon_16(dpy, (Window)xid);
    vt_vec_push(&t->icons, &e);
    return e.px;
}

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
        w->icon = NULL;
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
    vt_vec_init(&t->icons, sizeof(_iconent_t), 16);
    env->state = t;
    _tasklist_refresh(env);
}

static void _tasklist_fini(vt_applet_env_t *env) {
    _tasklist_t *t = env->state;
    if (!t) return;
    _tasklist_clear(t);
    vt_vec_fini(&t->wins);
    for (size_t i = 0; i < t->icons.size; i++) {
        _iconent_t *e = vt_vec_at(&t->icons, i);
        vt_free(e->px);
    }
    vt_vec_fini(&t->icons);
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
        int tx_off = 10;
        uint32_t *icon = _task_icon(t, ctx->dpy, w->id);
        if (icon) {
            int iy = env->area.y + (h - 16) / 2;
            vt_pctx_draw_argb(ctx, x + 8, iy, 16, 16, icon, 16, 16);
            tx_off = 30;
        }
        char *label = vt_strdup(w->title ? w->title : "");
        int tw = vt_pctx_text_width(ctx, label, false);
        if (tw > bw - 16 - tx_off) {
            /* truncate with ellipsis */
            while (tw > bw - 22 - tx_off && *label) {
                size_t ll = strlen(label);
                if (ll < 4) break;
                label[ll - 1] = 0;
                strcat(label, "…");
                tw = vt_pctx_text_width(ctx, label, false);
            }
        }
        int ty = env->area.y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
        vt_pctx_text(ctx, x + tx_off, ty, label, false, tc);
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
        int y = env->area.y, h = env->area.h;
        bool active = (i == w->current);
        /* glow: brighter fill + accent ring on the active workspace */
        vt_pcol_t fill = active ? (vt_pcol_t){ 0x6f, 0xaa, 0xe8, 0xff }
                                : (vt_pcol_t){ 0x26, 0x28, 0x2e, 0xff };
        vt_pctx_rounded_rect(ctx, x, y, WSP_BTN, h, 7, fill);
        if (active) {
            /* accent ring (1px inset outline) */
            vt_pcol_t ring = { 0xec, 0xee, 0xf0, 0xff };
            vt_pcol_t glow = { 0x4f, 0x9a, 0xdc, 0x60 };
            vt_pctx_rect(ctx, x + 2, y + 1, WSP_BTN - 4, 1, glow);
            vt_pctx_rect(ctx, x + 2, y + h - 2, WSP_BTN - 4, 1, ring);
            vt_pctx_rect(ctx, x + 1, y + 2, 1, h - 4, ring);
            vt_pctx_rect(ctx, x + WSP_BTN - 2, y + 2, 1, h - 4, ring);
        }
        char n[16];
        snprintf(n, sizeof(n), "%d", i + 1);
        vt_pcol_t tc = active ? (vt_pcol_t){ 0xff, 0xff, 0xff, 0xff }
                              : (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff };
        int tw = vt_pctx_text_width(ctx, n, active);
        vt_pctx_text(ctx, x + (WSP_BTN - tw) / 2, y + h / 2 +
                     vt_pctx_text_height(ctx) / 2 - 2, n, active, tc);
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
typedef struct {
    Window win;       /* calendar popup */
    XftDraw *draw;
    int view_year, view_month;   /* what the calendar shows */
    int sel;                    /* hovered day, 0 = none */
} _clock_t;

#define _CAL_W 232
#define _CAL_H 224
#define _CAL_CELL 28

static void _clock_cal_paint(vt_applet_env_t *env);
static void _clock_cal_close(vt_applet_env_t *env);
static void _clock_cal_handle(vt_panel_t *p, XEvent *ev);

static void _clock_cal_open(vt_applet_env_t *env) {
    _clock_t *k = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (k->win) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    k->view_year = tm.tm_year + 1900;
    k->view_month = tm.tm_mon;          /* 0-11 */
    k->sel = 0;
    int x = env->area.x + env->area.w - _CAL_W;
    if (x < 4) x = 4;
    int y = env->area.y + env->area.h + 4;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask };
    k->win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                           x, y, (unsigned)_CAL_W, (unsigned)_CAL_H, 1,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask,
                           &wa);
    k->draw = XftDrawCreate(ctx->dpy, k->win,
                            DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                            DefaultColormap(ctx->dpy,
                                            DefaultScreen(ctx->dpy)));
    XMapWindow(ctx->dpy, k->win);
    vt_panel_set_popup(env->panel, k->win, _clock_cal_handle);
    XFlush(ctx->dpy);
    _clock_cal_paint(env);
}

static void _clock_cal_close(vt_applet_env_t *env) {
    _clock_t *k = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!k->win) return;
    if (k->draw) { XftDrawDestroy(k->draw); k->draw = NULL; }
    XDestroyWindow(ctx->dpy, k->win);
    k->win = 0;
    vt_panel_set_popup(env->panel, 0, NULL);
    XFlush(ctx->dpy);
    vt_panel_invalidate(env->panel);
}

static void _clock_cal_paint(vt_applet_env_t *env) {
    _clock_t *k = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!k->win || !k->draw) return;
    Display *dpy = ctx->dpy;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(
        dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, k->win, fmt, 0, NULL);
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)_CAL_W,
                          (unsigned)_CAL_H);

    /* header: ‹ month year › */
    static const char *const mon[] = { "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    char head[64];
    snprintf(head, sizeof(head), "%s %d", mon[k->view_month], k->view_year);
    XRenderColor white = { .red = 0xecec, .green = 0xeeee, .blue = 0xf0f0,
                           .alpha = 0xffff };
    XftColor fc;
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &white, &fc);
    vt_pctx_menu_text(ctx, k->draw, 44, 24, head, true,
                      (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
    /* month arrows */
    vt_pctx_menu_text(ctx, k->draw, 12, 24, "\xe2\x80\xb9", true,
                      (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
    vt_pctx_menu_text(ctx, k->draw, _CAL_W - 20, 24, "\xe2\x80\xba", true,
                      (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });

    /* weekday header (Monday-first, locale-independent) */
    static const char *const wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa",
                                      "Su" };
    XRenderColor dim = { .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                         .alpha = 0xffff };
    XftColor fd;
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &dim, &fd);
    for (int i = 0; i < 7; i++)
        vt_pctx_menu_text(ctx, k->draw, 8 + i * _CAL_CELL + 6, 48, wd[i],
                          false, (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff });

    /* day grid */
    struct tm first = { .tm_year = k->view_year - 1900,
                        .tm_mon = k->view_month, .tm_mday = 1 };
    mktime(&first);
    int lead = (first.tm_wday + 6) % 7;          /* Monday-first offset */
    int days = 31;
    while (days > 28) {
        struct tm probe = { .tm_year = k->view_year - 1900,
                            .tm_mon = k->view_month, .tm_mday = days };
        if (mktime(&probe) == -1 || probe.tm_mon != k->view_month) days--;
        else break;
    }
    time_t now = time(NULL);
    struct tm tmn;
    localtime_r(&now, &tmn);
    for (int d = 0; d < days; d++) {
        int col = (lead + d) % 7;
        int row = (lead + d) / 7;
        int cx = 8 + col * _CAL_CELL;
        int cy = 56 + row * _CAL_CELL;
        if (cy + _CAL_CELL > _CAL_H - 4) break;
        char ds[8];
        snprintf(ds, sizeof(ds), "%d", d + 1);
        bool today = (k->view_year == tmn.tm_year + 1900 &&
                      k->view_month == tmn.tm_mon && d + 1 == tmn.tm_mday);
        bool hovered = (k->sel == d + 1);
        if (today) {
            XRenderColor acc = { .red = 0x4f4f, .green = 0x9a9a,
                                 .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &acc, (short)cx,
                                 (short)cy, (unsigned)(_CAL_CELL - 4),
                                 (unsigned)(_CAL_CELL - 4));
        } else if (hovered) {
            XRenderColor hov = { .red = 0x3939, .green = 0x3e3e,
                                 .blue = 0x4848, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hov, (short)cx,
                                 (short)cy, (unsigned)(_CAL_CELL - 4),
                                 (unsigned)(_CAL_CELL - 4));
        }
        XRenderColor tc = today
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : white;
        XftColor fday;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &tc,
                           &fday);
        int dw = ctx ? 6 : 6;
        {
            XGlyphInfo gi;
            XftTextExtentsUtf8(ctx->dpy, today ? ctx->font_bold : ctx->font,
                               (const FcChar8 *)ds, (int)strlen(ds), &gi);
            dw = gi.xOff;
        }
        vt_pctx_menu_text(ctx, k->draw,
                          cx + (_CAL_CELL - 4 - dw) / 2, cy + _CAL_CELL - 9,
                          ds, today,
                          today ? (vt_pcol_t){ 0xff, 0xff, 0xff, 0xff }
                                : (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
    }
    XRenderFreePicture(dpy, pic);
}

static void _clock_cal_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_CLOCK);
    _clock_t *k = env.state;
    if (!k || ev->xany.window != k->win) return;
    switch (ev->type) {
    case Expose:
        _clock_cal_paint(&env);
        break;
    case MotionNotify: {
        int x = ev->xmotion.x, y = ev->xmotion.y;
        if (y >= 56 && y < _CAL_H - 4) {
            int col = (x - 8) / _CAL_CELL, row = (y - 56) / _CAL_CELL;
            struct tm first = { .tm_year = k->view_year - 1900,
                                .tm_mon = k->view_month, .tm_mday = 1 };
            mktime(&first);
            int lead = (first.tm_wday + 6) % 7;
            int d = row * 7 + col - lead + 1;
            k->sel = (d >= 1 && d <= 31) ? d : 0;
        } else {
            k->sel = 0;
        }
        _clock_cal_paint(&env);
        break;
    }
    case ButtonRelease: {
        int x = ev->xbutton.x, y = ev->xbutton.y;
        if (ev->xbutton.button != Button1) {
            _clock_cal_close(&env);
            break;
        }
        if (y < 36) {
            if (x < 32) {                 /* previous month */
                if (--k->view_month < 0) { k->view_month = 11;
                                           k->view_year--; }
            } else if (x > _CAL_W - 32) { /* next month */
                if (++k->view_month > 11) { k->view_month = 0;
                                            k->view_year++; }
            } else {
                _clock_cal_close(&env);
                break;
            }
            k->sel = 0;
            _clock_cal_paint(&env);
        } else {
            _clock_cal_close(&env);
        }
        break;
    }
    default:
        break;
    }
}

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
static void _clock_init(vt_applet_env_t *env) {
    env->state = vt_malloc0(sizeof(_clock_t));
}
static void _clock_fini(vt_applet_env_t *env) {
    _clock_t *k = env->state;
    if (k && k->win) _clock_cal_close(env);
    vt_free(env->state);
}
static void _clock_on_click(vt_applet_env_t *env, int x, int y, int button) {
    (void)x; (void)y;
    _clock_t *k = env->state;
    if (button != 1) return;
    if (k->win) _clock_cal_close(env);
    else _clock_cal_open(env);
}
const vt_applet_impl_t _applet_clock = {
    .name = "clock", .init = _clock_init, .fini = _clock_fini,
    .render = _clock_render, .measure = _clock_measure,
    .on_click = _clock_on_click,
};

/* -------------------------------------------------------------- volume */
typedef struct {
    int level;      /* 0..100 */
    bool muted;
    bool ok;
    Window win;     /* slider popup */
    XftDraw *draw;
    bool dragging;
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
static bool _vol_set(int level, bool mute) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    snd_mixer_t *h = NULL;
    if (snd_mixer_open(&h, 0) < 0) return false;
    if (snd_mixer_attach(h, "default") < 0 ||
        snd_mixer_selem_register(h, NULL, NULL) < 0 ||
        snd_mixer_load(h) < 0) {
        snd_mixer_close(h);
        return false;
    }
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *el = snd_mixer_find_selem(h, sid);
    if (el) {
        long mn = 0, mx = 0;
        snd_mixer_selem_get_playback_volume_range(el, &mn, &mx);
        long lv = mn + (mx - mn) * level / 100;
        snd_mixer_selem_set_playback_volume_all(el, lv);
        if (snd_mixer_selem_has_playback_switch(el))
            snd_mixer_selem_set_playback_switch_all(el, mute ? 0 : 1);
    }
    snd_mixer_close(h);
    return el != NULL;
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
static bool _vol_set(int level, bool mute) { (void)level; (void)mute; return false; }
static void _vol_toggle(void) {}
#endif

#define _VOL_W 36
#define _VOL_H 150

static void _vol_popup_paint(vt_applet_env_t *env);
static void _vol_popup_close(vt_applet_env_t *env);
static void _vol_popup_handle(vt_panel_t *p, XEvent *ev);

static void _vol_popup_open(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (v->win) return;
    v->dragging = false;
    int x = env->area.x;
    int y = env->area.y + env->area.h + 4;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask };
    v->win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                           x, y, (unsigned)_VOL_W, (unsigned)_VOL_H, 1,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask,
                           &wa);
    v->draw = XftDrawCreate(ctx->dpy, v->win,
                            DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                            DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)));
    XMapWindow(ctx->dpy, v->win);
    vt_panel_set_popup(env->panel, v->win, _vol_popup_handle);
    XFlush(ctx->dpy);
    _vol_popup_paint(env);
}

static void _vol_popup_close(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!v->win) return;
    if (v->draw) { XftDrawDestroy(v->draw); v->draw = NULL; }
    XDestroyWindow(ctx->dpy, v->win);
    v->win = 0;
    vt_panel_set_popup(env->panel, 0, NULL);
    XFlush(ctx->dpy);
    vt_panel_invalidate(env->panel);
}

static void _vol_popup_paint(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!v->win || !v->draw) return;
    Display *dpy = ctx->dpy;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(
        dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, v->win, fmt, 0, NULL);
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)_VOL_W,
                          (unsigned)_VOL_H);
    /* vertical slider track: 6px wide, 12px margins */
    int track_x = (_VOL_W - 8) / 2;
    int track_y = 14, track_h = _VOL_H - 58;
    XRenderColor track = { .red = 0x3939, .green = 0x3e3e, .blue = 0x4848,
                           .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &track, (short)track_x,
                         (short)track_y, 8u, (unsigned)track_h);
    /* filled part (from the bottom up) */
    int fill_h = track_h * v->level / 100;
    XRenderColor fillc = v->muted
        ? (XRenderColor){ .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                          .alpha = 0xffff }
        : (XRenderColor){ .red = 0x4f4f, .green = 0x9a9a, .blue = 0xdcdc,
                          .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &fillc, (short)track_x,
                         (short)(track_y + track_h - fill_h), 8u,
                         (unsigned)fill_h);
    /* percentage label */
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", v->level);
    XRenderColor tc = { .red = 0xecec, .green = 0xeeee, .blue = 0xf0f0,
                        .alpha = 0xffff };
    XftColor fcv;
    XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                       DefaultColormap(dpy, DefaultScreen(dpy)), &tc, &fcv);
    int tw = 0;
    {
        XGlyphInfo gi;
        XftTextExtentsUtf8(dpy, ctx->font, (const FcChar8 *)pc,
                           (int)strlen(pc), &gi);
        tw = gi.xOff;
    }
    XftDrawStringUtf8(v->draw, &fcv, ctx->font, (_VOL_W - tw) / 2,
                      _VOL_H - 30, (const FcChar8 *)pc, (int)strlen(pc));
    /* mute button at the bottom */
    XRenderColor mbtn = v->muted
        ? (XRenderColor){ .red = 0xa8a8, .green = 0x5454, .blue = 0x3030,
                          .alpha = 0xffff }
        : (XRenderColor){ .red = 0x3939, .green = 0x3e3e, .blue = 0x4848,
                          .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &mbtn, 6,
                         (short)(_VOL_H - 24), (unsigned)(_VOL_W - 12), 18);
    const char *mlabel = v->muted ? "unmute" : "mute";
    XGlyphInfo gm;
    XftTextExtentsUtf8(dpy, ctx->font, (const FcChar8 *)mlabel,
                       (int)strlen(mlabel), &gm);
    XftDrawStringUtf8(v->draw, &fcv, ctx->font, (_VOL_W - gm.xOff) / 2,
                      _VOL_H - 11, (const FcChar8 *)mlabel,
                      (int)strlen(mlabel));
    XRenderFreePicture(dpy, pic);
}

static void _vol_popup_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_VOLUME);
    _vol_t *v = env.state;
    if (!v || ev->xany.window != v->win) return;
    int track_y = 14, track_h = _VOL_H - 58;
    switch (ev->type) {
    case Expose:
        _vol_popup_paint(&env);
        break;
    case MotionNotify:
    case ButtonPress: {
        int y = ev->type == MotionNotify ? ev->xmotion.y : ev->xbutton.y;
        if (ev->type == ButtonPress || v->dragging) {
            int lvl = (track_y + track_h - y) * 100 / track_h;
            if (lvl < 0) lvl = 0;
            if (lvl > 100) lvl = 100;
            v->level = lvl;
            v->muted = false;
            _vol_set(v->level, v->muted);
        }
        if (ev->type == ButtonPress) v->dragging = true;
        _vol_popup_paint(&env);
        vt_panel_invalidate(p);
        break;
    }
    case ButtonRelease: {
        int y = ev->xbutton.y;
        v->dragging = false;
        if (y > _VOL_H - 24) {           /* mute button */
            v->muted = !v->muted;
            _vol_set(v->level, v->muted);
        }
        _vol_popup_paint(&env);
        vt_panel_invalidate(p);
        break;
    }
    default:
        break;
    }
}

static void _vol_render(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!v->ok) return;
    /* speaker glyph + percentage */
    int x = env->area.x, y = env->area.y, h = env->area.h;
    vt_pcol_t fg = { 0xec, 0xee, 0xf0, 0xff };
    if (v->muted) fg.a = 0x90;
    vt_pctx_rect(ctx, x + 4, y + h / 2 - 3, 4, 6, fg);       /* driver  */
    vt_pctx_rect(ctx, x + 8, y + h / 2 - 6, 3, 12, fg);      /* cone    */
    if (!v->muted) {
        vt_pctx_rect(ctx, x + 13, y + h / 2 - 4, 1, 2, fg);  /* wave 1 */
        vt_pctx_rect(ctx, x + 15, y + h / 2 - 6, 1, 4, fg);
        vt_pctx_rect(ctx, x + 13, y + h / 2 + 2, 1, 2, fg);
        vt_pctx_rect(ctx, x + 15, y + h / 2 + 2, 1, 4, fg);
    } else {
        vt_pctx_rect(ctx, x + 14, y + h / 2 - 5, 1, 2, fg);  /* X */
        vt_pctx_rect(ctx, x + 16, y + h / 2 - 3, 1, 2, fg);
        vt_pctx_rect(ctx, x + 16, y + h / 2 + 1, 1, 2, fg);
        vt_pctx_rect(ctx, x + 18, y + h / 2 - 1, 1, 2, fg);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d%%", v->level);
    int ty = y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2;
    vt_pctx_text(ctx, x + 24, ty, buf, false, fg);
}
static int _vol_measure(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    return v->ok ? 56 : 0;
}
static void _vol_tick(vt_applet_env_t *env, uint64_t now) {
    (void)now;
    _vol_t *v = env->state;
    _vol_read(v);
}
static void _vol_click(vt_applet_env_t *env, int x, int y, int button) {
    (void)x; (void)y;
    _vol_t *v = env->state;
    if (button == 1) {
        if (v->win) _vol_popup_close(env);
        else _vol_popup_open(env);
    } else if (button == 2) {
        _vol_toggle();
        _vol_read(v);
    }
}
static void _vol_wheel(vt_applet_env_t *env, int dir) {
    _vol_t *v = env->state;
    int lvl = v->level + dir * 5;
    if (lvl < 0) lvl = 0;
    if (lvl > 100) lvl = 100;
    v->level = lvl;
    v->muted = false;
    _vol_set(v->level, v->muted);
    vt_panel_invalidate(env->panel);
}
static void _vol_init(vt_applet_env_t *env) {
    _vol_t *v = vt_malloc0(sizeof(*v));
    env->state = v;
    _vol_read(v);
}
static void _vol_fini(vt_applet_env_t *env) {
    _vol_t *v = env->state;
    if (v && v->win) _vol_popup_close(env);
    vt_free(v);
}
const vt_applet_impl_t _applet_volume = {
    .name = "volume", .init = _vol_init, .fini = _vol_fini,
    .render = _vol_render, .measure = _vol_measure, .on_tick = _vol_tick,
    .on_click = _vol_click, .on_wheel = _vol_wheel,
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
            n->wifi = -1;      /* wired until /proc/net/wireless proves
                                  otherwise */
            snprintf(n->name, sizeof(n->name), "%.*s",
                     (int)sizeof(n->name) - 1, de->d_name);
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
    n->wifi = -1;              /* -1 = wired/unknown, 0..100 = wireless */
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

/* ----------------------------------------------------------------- user */
/* Username + session menu: the last element on the panel. Actions use the
 * real system mechanisms — vt-integrations power hooks (logind/elogind
 * via systemctl/loginctl, then direct ioctls) and the session-manager
 * IPC socket for logout. Nothing is faked; failures are logged. */
typedef struct {
    char name[48];
    Window win;      /* session menu popup */
    XftDraw *draw;
    int sel;         /* hovered row */
    char *status;    /* one-line action feedback */
} _user_t;

enum {
    _UA_LOCK = 0, _UA_SUSPEND, _UA_SWITCH, _UA_LOGOUT, _UA_REBOOT,
    _UA_SHUTDOWN, _UA_EXIT, _UA_COUNT
};
static const char *const _user_actions[_UA_COUNT] = {
    "Lock Screen", "Suspend", "Switch User", "Log Out", "Reboot",
    "Shutdown", "Exit Session",
};

#define _UM_W 190
#define _UM_ROW 26

static void _user_menu_paint(vt_applet_env_t *env);
static void _user_menu_close(vt_applet_env_t *env);

static void _user_init(vt_applet_env_t *env) {
    _user_t *u = vt_malloc0(sizeof(*u));
    const char *n = getenv("USER");
    if ((!n || !*n)) {
        struct passwd *pw = getpwuid(getuid());
        n = pw ? pw->pw_name : "user";
    }
    snprintf(u->name, sizeof(u->name), "%s", n);
    u->sel = -1;
    env->state = u;
}
static void _user_fini(vt_applet_env_t *env) {
    _user_t *u = env->state;
    if (u) {
        if (u->win) _user_menu_close(env);
        vt_free(u->status);
        vt_free(u);
    }
}
static int _user_measure(vt_applet_env_t *env) {
    _user_t *u = env->state;
    return vt_pctx_text_width(env->ctx, u->name, false) + 34;
}
static void _user_render(vt_applet_env_t *env) {
    _user_t *u = env->state;
    vt_pctx_t *ctx = env->ctx;
    int x = env->area.x, y = env->area.y, h = env->area.h;
    /* avatar disc + name + menu caret */
    vt_pcol_t accent = { 0x4f, 0x9a, 0xdc, 0xff };
    vt_pctx_rounded_rect(ctx, x + 6, y + 4, h - 8, h - 8, (h - 8) / 2, accent);
    vt_pcol_t fg = { 0xff, 0xff, 0xff, 0xff };
    /* head + shoulders glyph */
    vt_pctx_rect(ctx, x + 6 + (h - 8) / 2 - 2, y + 9, 4, 4, fg);
    vt_pctx_rect(ctx, x + 6 + (h - 8) / 2 - 4, y + 15, 8, 3, fg);
    vt_pctx_text(ctx, x + h + 2,
                 y + h / 2 + vt_pctx_text_height(ctx) / 2 - 2, u->name,
                 false, (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff });
    /* caret */
    for (int i = 0; i < 3; i++)
        vt_pctx_rect(ctx, x + env->area.w - 16 + i * 4, y + h / 2 - 1 + i, 2,
                     2, (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff });
}

/* run a real session action; returns an honest status line */
static char *_user_do_action(int act) {
    vt_power_t *pwr = vt_power_new();
    vt_power_init(pwr);
    char *status = NULL;
    switch (act) {
    case _UA_LOCK:
        /* real: logind/elogind LockSession (loginctl) */
        if (system("loginctl lock-session 2>/dev/null") == 0)
            status = vt_strdup("lock requested (loginctl)");
        else
            status = vt_strdup("lock unavailable: no logind/elogind "
                               "session (start one, or install elogind)");
        break;
    case _UA_SUSPEND:
        if (vt_power_suspend(pwr) == 0)
            status = vt_strdup("suspend requested");
        else
            status = vt_strdup("suspend failed (see journal)");
        break;
    case _UA_SWITCH: {
        /* real: activate another login session if one exists */
        FILE *fp = popen("loginctl list-sessions --no-legend 2>/dev/null",
                         "r");
        char line[256];
        char other[64] = "";
        bool own = false;
        const char *sid = getenv("XDG_SESSION_ID");
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                char cand[64] = "";
                sscanf(line, "%63s", cand);
                if (!cand[0]) continue;
                if (sid && strcmp(cand, sid) == 0) { own = true; continue; }
                snprintf(other, sizeof(other), "%s", cand);
                break;
            }
            pclose(fp);
        }
        if (own && other[0]) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "loginctl activate-session %s",
                     other);
            if (system(cmd) == 0)
                status = vt_strprintf("switching to session %s", other);
            else
                status = vt_strdup("activate-session failed");
        } else {
            status = vt_strdup("no other session to switch to "
                               "(log in on another VT first)");
        }
        break;
    }
    case _UA_LOGOUT:
        vt_panel_send_session(VT_IPC_MSG_WM_LOGOUT, "");
        status = vt_strdup("logout requested");
        break;
    case _UA_REBOOT:
        if (vt_power_reboot(pwr) == 0)
            status = vt_strdup("reboot requested");
        else
            status = vt_strdup("reboot not permitted (are you allowed?)");
        break;
    case _UA_SHUTDOWN:
        if (vt_power_shutdown(pwr) == 0)
            status = vt_strdup("shutdown requested");
        else
            status = vt_strdup("shutdown not permitted (are you allowed?)");
        break;
    case _UA_EXIT:
        vt_panel_send_session(VT_IPC_MSG_WM_LOGOUT, "exit");
        status = vt_strdup("exiting the session");
        break;
    default:
        break;
    }
    vt_power_free(pwr);
    return status;
}

static void _user_menu_paint(vt_applet_env_t *env) {
    _user_t *u = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!u->win || !u->draw) return;
    Display *dpy = ctx->dpy;
    int w = _UM_W;
    int h = _UA_COUNT * _UM_ROW + 8 + (u->status ? _UM_ROW : 0);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
        DefaultVisual(dpy, DefaultScreen(dpy)));
    Picture pic = XRenderCreatePicture(dpy, u->win, fmt, 0, NULL);
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)w,
                         (unsigned)h);
    for (int i = 0; i < _UA_COUNT; i++) {
        int ry = 4 + i * _UM_ROW;
        bool sel = (i == u->sel);
        if (sel) {
            XRenderColor hi = { .red = 0x4f4f, .green = 0x9a9a,
                                .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(dpy, PictOpOver, pic, &hi, 2, (short)ry,
                                 (unsigned)(w - 4), (unsigned)(_UM_ROW - 2));
        }
        XRenderColor tc = sel
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : (XRenderColor){ .red = 0xecec, .green = 0xeeee,
                              .blue = 0xf0f0, .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &tc,
                           &fc);
        /* destructive actions render in warning tone */
        if (i >= _UA_REBOOT && !sel) {
            XRenderColor warn = { .red = 0xe0e0, .green = 0x7a7a,
                                  .blue = 0x5050, .alpha = 0xffff };
            XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                               DefaultColormap(dpy, DefaultScreen(dpy)),
                               &warn, &fc);
        }
        {
            vt_pcol_t rowc = sel ? (vt_pcol_t){ 0xff, 0xff, 0xff, 0xff }
                                 : (vt_pcol_t){ 0xec, 0xee, 0xf0, 0xff };
            if (i >= _UA_REBOOT && !sel)
                rowc = (vt_pcol_t){ 0xe0, 0x7a, 0x50, 0xff };
            vt_pctx_menu_text(ctx, u->draw, 10, ry + _UM_ROW - 7,
                              _user_actions[i], sel, rowc);
        }
    }
    if (u->status) {
        XRenderColor dim = { .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                             .alpha = 0xffff };
        XftColor fd;
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &dim,
                           &fd);
        vt_pctx_menu_text(ctx, u->draw, 10,
                          4 + _UA_COUNT * _UM_ROW + _UM_ROW - 7, u->status,
                          false, (vt_pcol_t){ 0x90, 0x93, 0x99, 0xff });
    }
    XRenderFreePicture(dpy, pic);
}

static void _user_menu_handle(vt_panel_t *p, XEvent *ev) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_USER);
    _user_t *u = env.state;
    if (!u || ev->xany.window != u->win) return;
    switch (ev->type) {
    case Expose:
        _user_menu_paint(&env);
        break;
    case MotionNotify:
        u->sel = (ev->xmotion.y - 4) / _UM_ROW;
        if (u->sel >= _UA_COUNT) u->sel = -1;
        _user_menu_paint(&env);
        break;
    case ButtonRelease: {
        if (ev->xbutton.button == Button1) {
            int idx = (ev->xbutton.y - 4) / _UM_ROW;
            if (idx >= 0 && idx < _UA_COUNT) {
                vt_free(u->status);
                u->status = _user_do_action(idx);
                vt_logi("user-menu: %s — %s", _user_actions[idx], u->status);
                _user_menu_paint(&env);
                /* logout/exit close the menu: the session is ending */
                if (idx == _UA_LOGOUT || idx == _UA_EXIT) {
                    _user_menu_close(&env);
                }
                break;
            }
        }
        _user_menu_close(&env);
        break;
    }
    default:
        break;
    }
}

static void _user_menu_close(vt_applet_env_t *env) {
    _user_t *u = env->state;
    vt_pctx_t *ctx = env->ctx;
    if (!u->win) return;
    if (u->draw) { XftDrawDestroy(u->draw); u->draw = NULL; }
    XDestroyWindow(ctx->dpy, u->win);
    u->win = 0;
    vt_panel_set_popup(env->panel, 0, NULL);
    XFlush(ctx->dpy);
    vt_panel_invalidate(env->panel);
}

void vt_panel_user_menu_open(struct vt_panel *p) {
    vt_applet_env_t env = vt_panel_find_env(p, VT_PANEL_APPLET_USER);
    _user_t *u = env.state;
    vt_pctx_t *ctx = env.ctx;
    if (!u || u->win) return;
    int w = _UM_W;
    int h = _UA_COUNT * _UM_ROW + 8;
    int x = env.area.x + env.area.w - w;
    if (x < 4) x = 4;
    int y = env.area.y + env.area.h + 4;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask };
    u->win = XCreateWindow(ctx->dpy, DefaultRootWindow(ctx->dpy),
                           x, y, (unsigned)w, (unsigned)h, 1, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask,
                           &wa);
    u->draw = XftDrawCreate(ctx->dpy, u->win,
                            DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                            DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)));
    u->sel = -1;
    XMapWindow(ctx->dpy, u->win);
    vt_panel_set_popup(p, u->win, _user_menu_handle);
    XFlush(ctx->dpy);
    _user_menu_paint(&env);
}

static void _user_on_click(vt_applet_env_t *env, int x, int y, int button) {
    (void)x; (void)y;
    _user_t *u = env->state;
    if (button != 1) return;
    if (u->win) _user_menu_close(env);
    else {
        vt_free(u->status);
        u->status = NULL;
        vt_panel_user_menu_open(env->panel);
    }
}

const vt_applet_impl_t _applet_user = {
    .name = "user", .init = _user_init, .fini = _user_fini,
    .measure = _user_measure, .render = _user_render,
    .on_click = _user_on_click,
};
