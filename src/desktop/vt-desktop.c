/*
 * vt-desktop.c — Vantage desktop surface (X11)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * A _NET_WM_WINDOW_TYPE_DESKTOP child of the root, rendered bottom-most
 * and present on every workspace. Draws the wallpaper (color, gradient,
 * image, or video via the wallpaper engine + software renderer → XImage
 * → XPutImage), shows desktop icons (the .desktop files in ~/Desktop), and
 * offers a right-click application menu.
 *
 * Video wallpapers are stepped at most ~30 fps and PAUSED whenever a
 * fullscreen window covers the desktop (checked once per second via the
 * root's _NET_ACTIVE_WINDOW state) — no decode work while invisible.
 */

#define VT_LOG_DOMAIN "desktop"
#include <vantage/vt-desktop.h>
#include <vantage/vt-wallpaper.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-config.h>
#include <vantage/vt-ipc.h>
#include <vantage/vt-x11.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>

typedef struct {
    char *name;
    char *exec;
    int x, y, w, h;
} _icon_t;

typedef struct {
    uint8_t     *bgrx;          /* scratch for RGBA→BGRX conversion */
    size_t       bgrx_sz;
    Display      *dpy;
    Window        win;
    Window        root;
    int           screen;
    int           w, h;
    GC            gc;
    XftDraw      *xft;
    XftFont      *font;
    XftColor      text_col;
    vt_renderer_t *sw;           /* software renderer for the wallpaper */
    vt_wallpaper_t *wall;
    uint64_t      last_frame_ms;
    uint64_t      last_visible_check_ms;
    bool          video_paused;
    vt_vec_t      icons;         /* _icon_t */
    /* context menu */
    Window        menu;
    XftDraw      *menu_draw;
    int           menu_sel;
    int           menu_n;
    char        **menu_items;    /* static strings */
    vt_ipc_t     *ipc;
} _dt_priv_t;

static void _dt_paint_wallpaper(struct vt_desktop *d);
static void _dt_menu_handle(struct vt_desktop *d, XEvent *ev);

/* ------------------------------------------------------------- wallpaper */
static void _dt_wallpaper_from_config(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    vt_config_t *cfg = vt_config_new_defaults();
    vt_config_load(cfg, vt_config_default_path());
    const char *mode = vt_config_get(cfg, "wallpaper", "mode", "gradient");
    vt_wallpaper_t *w = p->wall;
    if (vt_strcaseeq(mode, "color")) {
        vt_wallpaper_set_kind(w, VT_WALLPAPER_COLOR);
    } else if (vt_strcaseeq(mode, "image")) {
        vt_wallpaper_set_kind(w, VT_WALLPAPER_IMAGE);
        vt_wallpaper_set_path(w, vt_config_get(cfg, "wallpaper", "path",
                                               ""));
    } else if (vt_strcaseeq(mode, "video")) {
        vt_wallpaper_set_kind(w, VT_WALLPAPER_VIDEO);
        vt_wallpaper_set_path(w, vt_config_get(cfg, "wallpaper", "path",
                                               ""));
        vt_wallpaper_set_loop(w, vt_config_get_bool(cfg, "wallpaper",
                                                    "loop", true));
    } else {
        vt_wallpaper_set_kind(w, VT_WALLPAPER_GRADIENT);
    }
    vt_color_t a, b;
    a.r = 0.07f; a.g = 0.09f; a.b = 0.14f; a.a = 1.0f;
    b.r = 0.15f; b.g = 0.19f; b.b = 0.31f; b.a = 1.0f;
    const char *ca = vt_config_get(cfg, "wallpaper", "color_a", NULL);
    const char *cb = vt_config_get(cfg, "wallpaper", "color_b", NULL);
    if (ca) sscanf(ca, "%f,%f,%f", &a.r, &a.g, &a.b);
    if (cb) sscanf(cb, "%f,%f,%f", &b.r, &b.g, &b.b);
    int dir = (int)vt_config_get_int(cfg, "wallpaper", "gradient_dir", 1);
    vt_wallpaper_set_color(w, a, b, dir);
    vt_config_free(cfg);
}

static bool _dt_fullscreen_covers(_dt_priv_t *p) {
    /* True when the active window is fullscreen → wallpaper invisible */
    Atom act = vt_x11_atoms()->net_active_window;
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    if (XGetWindowProperty(p->dpy, p->root, act, 0, 4, False, XA_WINDOW,
                           &actual, &fmt, &n, &bytes, &data) != Success)
        return false;
    if (!data || n < 1) { if (data) XFree(data); return false; }
    Window active = *(Window *)(void *)data;
    XFree(data);
    if (!active) return false;
    unsigned char *st = NULL;
    unsigned long sn = 0;
    bool fs = false;
    if (vt_x11_get_window_property(active, vt_x11_atoms()->net_wm_state,
                                   vt_x11_atoms()->atom_atom, &st, &sn)) {
        Atom *atoms = (Atom *)(void *)st;
        for (unsigned long i = 0; i < sn; i++)
            if (atoms[i] == vt_x11_atoms()->net_wm_state_fullscreen)
                fs = true;
        XFree(st);
    }
    return fs;
}

/* --------------------------------------------------------------- icons */
static void _dt_load_icons(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    char *dir = vt_strprintf("%s/Desktop", vt_home_dir());
    DIR *dd = opendir(dir);
    vt_free(dir);
    if (!dd) return;
    struct dirent *de;
    while ((de = readdir(dd))) {
        if (!vt_strendswith(de->d_name, ".desktop")) continue;
        char *path = vt_strprintf("%s/Desktop/%s", vt_home_dir(),
                                  de->d_name);
        size_t len = 0;
        char *content = vt_file_read_all(path, &len);
        vt_free(path);
        if (!content) continue;
        char *name = NULL, *exec = NULL, *save = NULL;
        for (char *line = strtok_r(content, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (vt_strstartswith(line, "Name=") && !name)
                name = vt_strdup(line + 5);
            else if (vt_strstartswith(line, "Exec=") && !exec)
                exec = vt_strdup(line + 5);
        }
        vt_free(content);
        if (!name || !exec) { vt_free(name); vt_free(exec); continue; }
        /* strip desktop placeholders */
        char *pp = strstr(exec, "%");
        if (pp) *pp = 0;
        _icon_t ic = { .name = name, .exec = vt_strtrim(exec),
                       .w = 84, .h = 92 };
        /* grid: left-to-right, top-to-bottom, 6 rows then next column */
        size_t idx = p->icons.size;
        ic.x = 16 + (int)(idx / 6) * 96;
        ic.y = 16 + (int)(idx % 6) * 100;
        vt_vec_push(&p->icons, &ic);
        vt_free(exec);
    }
    closedir(dd);
    if (p->icons.size)
        vt_logi("desktop: %zu icons", p->icons.size);
}

static void _dt_draw_icons(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    if (!p->xft || !p->font) return;
    for (size_t i = 0; i < p->icons.size; i++) {
        _icon_t *ic = vt_vec_at(&p->icons, i);
        /* icon square */
        XRenderColor c = { .red = 0x3a3a, .green = 0x3a3a,
                           .blue = 0x4a4a, .alpha = 0xc000 };
        XRenderPictFormat *fmt = XRenderFindVisualFormat(p->dpy,
            DefaultVisual(p->dpy, p->screen));
        Picture pic = XRenderCreatePicture(p->dpy, p->win, fmt, 0, NULL);
        XRenderFillRectangle(p->dpy, PictOpOver, pic, &c,
                             (short)(ic->x + 22), (short)ic->y, 40u, 40u);
        XRenderFreePicture(p->dpy, pic);
        /* label */
        int tw = 0;
        XGlyphInfo gi;
        XftTextExtentsUtf8(p->dpy, p->font, (const FcChar8 *)ic->name,
                           (int)strlen(ic->name), &gi);
        tw = gi.xOff;
        int tx = ic->x + (ic->w - tw) / 2;
        if (tx < 2) tx = 2;
        XftDrawStringUtf8(p->xft, &p->text_col, p->font, tx, ic->y + 62,
                          (const FcChar8 *)ic->name,
                          (int)strlen(ic->name));
    }
}

/* --------------------------------------------------------- context menu */
static void _dt_menu_close(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    if (!p->menu) return;
    if (p->menu_draw) { XftDrawDestroy(p->menu_draw); p->menu_draw = NULL; }
    XDestroyWindow(p->dpy, p->menu);
    p->menu = 0;
    XFlush(p->dpy);
}

static void _dt_menu_open(struct vt_desktop *d, int x, int y) {
    _dt_priv_t *p = d->priv;
    if (p->menu) _dt_menu_close(d);
    static char *items[] = {
        (char *)"Open Terminal",
        (char *)"Open File Manager",
        (char *)"Vantage Settings",
        (char *)"Display Information",
        (char *)"Refresh Wallpaper",
        (char *)"Log Out",
    };
    p->menu_items = items;
    p->menu_n = 6;
    p->menu_sel = -1;
    int w = 200, row = 26;
    int h = p->menu_n * row + 8;
    XSetWindowAttributes wa = { .override_redirect = True,
                                .background_pixel = 0x22221c1a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask };
    p->menu = XCreateWindow(p->dpy, p->root, (int)x, (int)y,
                            (unsigned)w, (unsigned)h, 1, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask,
                            &wa);
    p->menu_draw = XftDrawCreate(p->dpy, p->menu,
                                 DefaultVisual(p->dpy, p->screen),
                                 DefaultColormap(p->dpy, p->screen));
    XMapWindow(p->dpy, p->menu);
    XFlush(p->dpy);
}

static void _dt_menu_paint(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    if (!p->menu || !p->menu_draw) return;
    int w = 200, row = 26;
    XRenderColor bg = { .red = 0x1a1a, .green = 0x1c1c, .blue = 0x2222,
                        .alpha = 0xffff };
    XRenderPictFormat *fmt = XRenderFindVisualFormat(p->dpy,
        DefaultVisual(p->dpy, p->screen));
    Picture pic = XRenderCreatePicture(p->dpy, p->menu, fmt, 0, NULL);
    XRenderFillRectangle(p->dpy, PictOpSrc, pic, &bg, 0, 0, (unsigned)w,
                         (unsigned)(p->menu_n * row + 8));
    for (int i = 0; i < p->menu_n; i++) {
        if (i == p->menu_sel) {
            XRenderColor hi = { .red = 0x4f4f, .green = 0x9a9a,
                                .blue = 0xdcdc, .alpha = 0xffff };
            XRenderFillRectangle(p->dpy, PictOpOver, pic, &hi, 2,
                                  (short)(4 + i * row), (unsigned)(w - 4),
                                  (unsigned)(row - 2));
        }
        XRenderColor tc = (i == p->menu_sel)
            ? (XRenderColor){ .red = 0xffff, .green = 0xffff,
                              .blue = 0xffff, .alpha = 0xffff }
            : (XRenderColor){ .red = 0xecec, .green = 0xeeee,
                              .blue = 0xf0f0, .alpha = 0xffff };
        XftColor fc;
        XftColorAllocValue(p->dpy, DefaultVisual(p->dpy, p->screen),
                           DefaultColormap(p->dpy, p->screen), &tc, &fc);
        XftDrawStringUtf8(p->menu_draw, &fc, p->font, 12,
                          4 + i * row + row - 7,
                          (const FcChar8 *)p->menu_items[i],
                          (int)strlen(p->menu_items[i]));
    }
    XRenderFreePicture(p->dpy, pic);
}

static void _dt_menu_activate(struct vt_desktop *d, int idx) {
    switch (idx) {
    case 0: /* terminal */
        vt_proc_spawn_detached("xterm 2>/dev/null || "
                               "x-terminal-emulator 2>/dev/null || "
                               "weston-terminal 2>/dev/null || "
                               "alacritty 2>/dev/null || st");
        break;
    case 1: /* file manager */
        vt_proc_spawn_detached("thunar 2>/dev/null || pcmanfm 2>/dev/null || "
                               "nautilus 2>/dev/null || xdg-open .");
        break;
    case 2: /* settings */
        vt_proc_spawn_detached("vantage-settings");
        break;
    case 3: /* display info */
        vt_proc_spawn_detached("xmessage \"`vantage-diagnostics 2>&1`\" "
                               "2>/dev/null || vantage-diagnostics");
        break;
    case 4: /* refresh wallpaper */
        {
            _dt_priv_t *p = d->priv;
            vt_wallpaper_free(p->wall);
            p->wall = vt_wallpaper_new();
            _dt_wallpaper_from_config(d);
            _dt_paint_wallpaper(d);
        }
        break;
    case 5: /* log out → tell the session via IPC */
        {
            _dt_priv_t *p = d->priv;
            if (p->ipc)
                vt_ipc_send(p->ipc, VT_IPC_MSG_WM_LOGOUT, VT_IPC_MSG_REQUEST,
                            "logout", 6);
            else
                vt_proc_spawn_detached("vantage-remote ws 2>/dev/null");
        }
        break;
    default:
        break;
    }
}

static void _dt_menu_handle(struct vt_desktop *d, XEvent *ev) {
    _dt_priv_t *p = d->priv;
    if (ev->xany.window != p->menu) return;
    switch (ev->type) {
    case Expose:
        _dt_menu_paint(d);
        break;
    case MotionNotify:
        p->menu_sel = (ev->xmotion.y - 4) / 26;
        _dt_menu_paint(d);
        break;
    case ButtonRelease:
        if (ev->xbutton.button == Button1) {
            int idx = (ev->xbutton.y - 4) / 26;
            if (idx >= 0 && idx < p->menu_n) _dt_menu_activate(d, idx);
        }
        _dt_menu_close(d);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------- wallpaper */
static void _dt_blit(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    int fw = 0, fh = 0;
    const uint8_t *buf = vt_renderer_framebuffer(p->sw, &fw, &fh);
    if (!buf || fw != p->w || fh != p->h) return;
    /* RGBA → BGRX into the scratch buffer (never swap in place: the
     * renderer owns the source and video repaints often) */
    size_t need = (size_t)fw * fh * 4;
    if (p->bgrx_sz < need) {
        p->bgrx = vt_realloc(p->bgrx, need);
        p->bgrx_sz = need;
    }
    const uint32_t *src = (const uint32_t *)buf;
    uint32_t *dst = (uint32_t *)p->bgrx;
    for (size_t i = 0; i < (size_t)fw * fh; i++) {
        uint32_t v = src[i];              /* RRGGBBAA little-endian */
        dst[i] = (v & 0xff00ff00) | ((v & 0x000000ff) << 16) |
                 ((v & 0x00ff0000) >> 16);
    }
    XImage *img = XCreateImage(p->dpy, DefaultVisual(p->dpy, p->screen),
                               (unsigned)DefaultDepth(p->dpy, p->screen),
                               ZPixmap, 0, (char *)p->bgrx, (unsigned)fw,
                               (unsigned)fh, 32, fw * 4);
    if (!img) return;
    XPutImage(p->dpy, p->win, p->gc, img, 0, 0, 0, 0, (unsigned)fw,
              (unsigned)fh);
    img->data = NULL; /* scratch owned by us */
    XDestroyImage(img);
}

static void _dt_paint_wallpaper(struct vt_desktop *d) {
    _dt_priv_t *p = d->priv;
    if (!p->sw || !p->wall) return;
    /* step() performs the one-time image/video frame load into the
     * texture before rendering */
    vt_wallpaper_step(p->wall, p->sw, (uint32_t)p->w, (uint32_t)p->h);
    vt_renderer_begin(p->sw, p->w, p->h);
    vt_rect_t full = { 0, 0, p->w, p->h };
    vt_wallpaper_render(p->wall, p->sw, full);
    vt_renderer_end(p->sw);
    _dt_blit(d);
    _dt_draw_icons(d);
}

/* ------------------------------------------------------------- public */
int vt_desktop_start(vt_desktop_t *d) {
    if (!d || !d->priv) return VT_ERR_INVAL;
    _dt_priv_t *p = d->priv;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        vt_loge("desktop: cannot open display");
        return VT_ERR;
    }
    p->dpy = dpy;
    p->screen = DefaultScreen(dpy);
    p->root = RootWindow(dpy, p->screen);
    p->w = DisplayWidth(dpy, p->screen);
    p->h = DisplayHeight(dpy, p->screen);

    XSetWindowAttributes wa = { .override_redirect = False,
                                .background_pixel = 0x14161a,
                                .event_mask = ExposureMask |
                                              ButtonPressMask |
                                              ButtonReleaseMask |
                                              StructureNotifyMask };
    p->win = XCreateWindow(dpy, p->root, 0, 0, (unsigned)p->w,
                           (unsigned)p->h, 0, CopyFromParent, InputOutput,
                           CopyFromParent, CWBackPixel | CWEventMask, &wa);
    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desk = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, p->win, type, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&desk, 1);
    Atom wm_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    unsigned long all = 0xffffffff;
    XChangeProperty(dpy, p->win, wm_desk, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&all, 1);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom below = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(dpy, p->win, state, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&below, 1);
    XStoreName(dpy, p->win, "Vantage Desktop");
    /* bottom of the stack */
    XWindowChanges wc = { .stack_mode = Below };
    XConfigureWindow(dpy, p->win, CWStackMode, &wc);
    p->gc = XCreateGC(dpy, p->win, 0, NULL);
    p->xft = XftDrawCreate(dpy, p->win, DefaultVisual(dpy, p->screen),
                           DefaultColormap(dpy, p->screen));
    p->font = XftFontOpenName(dpy, p->screen, "Sans:size=9");
    XRenderColor tc = { .red = 0xecec, .green = 0xeeee, .blue = 0xf0f0,
                        .alpha = 0xffff };
    XftColorAllocValue(dpy, DefaultVisual(dpy, p->screen),
                       DefaultColormap(dpy, p->screen), &tc, &p->text_col);

    /* wallpaper engine on the software renderer */
    p->sw = vt_renderer_new(VT_RENDERER_SW);
    vt_renderer_init(p->sw);
    p->wall = vt_wallpaper_new();
    _dt_wallpaper_from_config(d);
    if (p->wall->kind == VT_WALLPAPER_VIDEO)
        vt_wallpaper_load(p->wall, p->wall->path);

    _dt_load_icons(d);
    XMapWindow(dpy, p->win);
    _dt_paint_wallpaper(d);
    XFlush(dpy);
    d->running = true;
    vt_logi("desktop: started %dx%d (%s wallpaper)", p->w, p->h,
            p->wall->kind == VT_WALLPAPER_VIDEO ? "video"
            : p->wall->kind == VT_WALLPAPER_IMAGE ? "image"
            : p->wall->kind == VT_WALLPAPER_GRADIENT ? "gradient"
            : "color");
    return VT_OK;
}

void vt_desktop_stop(vt_desktop_t *d) {
    if (!d || !d->priv) return;
    _dt_priv_t *p = d->priv;
    _dt_menu_close(d);
    if (p->xft) XftDrawDestroy(p->xft);
    if (p->font) XftFontClose(p->dpy, p->font);
    vt_free(p->bgrx);
    if (p->sw) vt_renderer_free(p->sw);
    if (p->wall) vt_wallpaper_free(p->wall);
    for (size_t i = 0; i < p->icons.size; i++) {
        _icon_t *ic = vt_vec_at(&p->icons, i);
        vt_free(ic->name);
        vt_free(ic->exec);
    }
    vt_vec_fini(&p->icons);
    if (p->ipc) vt_ipc_free(p->ipc);
    if (p->gc) XFreeGC(p->dpy, p->gc);
    if (p->win) XDestroyWindow(p->dpy, p->win);
    if (p->dpy) XCloseDisplay(p->dpy);
    d->running = false;
}

vt_desktop_t *vt_desktop_new(vt_renderer_t *r) {
    (void)r; /* the desktop always uses its own software renderer */
    vt_desktop_t *d = vt_malloc0(sizeof(*d));
    d->priv = vt_malloc0(sizeof(_dt_priv_t));
    _dt_priv_t *p = d->priv;
    vt_vec_init(&p->icons, sizeof(_icon_t), 8);
    d->show_icons = true;
    return d;
}

void vt_desktop_set_show_icons(vt_desktop_t *d, bool on) {
    if (d) d->show_icons = on;
}

int vt_desktop_add_icon(vt_desktop_t *d, const char *name, const char *path,
                        const char *icon, int x, int y) {
    if (!d || !d->priv || !name) return -1;
    _dt_priv_t *p = d->priv;
    _icon_t ic = { .name = vt_strdup(name),
                   .exec = vt_strdup(path ? path : ""),
                   .x = x, .y = y, .w = 84, .h = 92 };
    (void)icon;
    vt_vec_push(&p->icons, &ic);
    return (int)p->icons.size - 1;
}

void vt_desktop_remove_icon(vt_desktop_t *d, const char *name) {
    if (!d || !d->priv || !name) return;
    _dt_priv_t *p = d->priv;
    for (size_t i = 0; i < p->icons.size; i++) {
        _icon_t *ic = vt_vec_at(&p->icons, i);
        if (vt_streq(ic->name, name)) {
            vt_free(ic->name);
            vt_free(ic->exec);
            vt_vec_remove(&p->icons, i);
            return;
        }
    }
}

void vt_desktop_render(vt_desktop_t *d) {
    if (d && d->priv && d->running) _dt_paint_wallpaper(d);
}

void vt_desktop_show_menu(vt_desktop_t *d, int x, int y) {
    if (d && d->priv) _dt_menu_open(d, x, y);
}

void vt_desktop_free(vt_desktop_t *d) {
    if (!d) return;
    vt_desktop_stop(d);
    vt_free(d->priv);
    vt_free(d);
}

int vt_desktop_step(vt_desktop_t *d, int timeout_ms) {
    if (!d || !d->running || !d->priv) return VT_ERR_INVAL;
    _dt_priv_t *p = d->priv;
    while (XPending(p->dpy)) {
        XEvent ev;
        XNextEvent(p->dpy, &ev);
        if (p->menu && ev.xany.window == p->menu) {
            _dt_menu_handle(d, &ev);
            continue;
        }
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) _dt_paint_wallpaper(d);
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != p->w || ev.xconfigure.height != p->h) {
                p->w = ev.xconfigure.width;
                p->h = ev.xconfigure.height;
                XMoveResizeWindow(p->dpy, p->win, 0, 0, (unsigned)p->w,
                                  (unsigned)p->h);
                _dt_paint_wallpaper(d);
            }
            break;
        case ButtonPress: {
            XButtonEvent *be = &ev.xbutton;
            if (be->window == p->win) {
                if (be->button == Button3)
                    _dt_menu_open(d, be->x, be->y);
                else if (be->button == Button1) {
                    /* icon double-click */
                    for (size_t i = 0; i < p->icons.size; i++) {
                        _icon_t *ic = vt_vec_at(&p->icons, i);
                        if (be->x >= ic->x && be->x < ic->x + ic->w &&
                            be->y >= ic->y && be->y < ic->y + ic->h) {
                            static uint64_t last_click = 0;
                            static size_t last_idx = (size_t)-1;
                            uint64_t now = vt_time_now_ms();
                            if (i == last_idx &&
                                now - last_click < 400) {
                                vt_logi("desktop: launch '%s'", ic->exec);
                                vt_proc_spawn_detached(ic->exec);
                            }
                            last_click = now;
                            last_idx = i;
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    /* wallpaper animation (video): step at ~30 fps max, pause when
     * covered by a fullscreen window (checked once per second) */
    if (p->wall && p->wall->kind == VT_WALLPAPER_VIDEO) {
        uint64_t now = vt_time_now_ms();
        if (now - p->last_visible_check_ms >= 1000) {
            p->last_visible_check_ms = now;
            bool covered = _dt_fullscreen_covers(p);
            if (covered != p->video_paused) {
                p->video_paused = covered;
                if (covered) vt_wallpaper_pause(p->wall);
                else vt_wallpaper_resume(p->wall);
                vt_logd("desktop: video wallpaper %s",
                        covered ? "paused" : "resumed");
            }
        }
        if (!p->video_paused && now - p->last_frame_ms >= 33) {
            p->last_frame_ms = now;
            vt_wallpaper_step(p->wall, p->sw, (uint32_t)p->w, (uint32_t)p->h);
            vt_renderer_begin(p->sw, p->w, p->h);
            vt_rect_t full = { 0, 0, p->w, p->h };
            vt_wallpaper_render(p->wall, p->sw, full);
            vt_renderer_end(p->sw);
            _dt_blit(d);
            _dt_draw_icons(d);
        }
    }
    if (timeout_ms > 0 && !XPending(p->dpy)) {
        struct pollfd fd = { .fd = ConnectionNumber(p->dpy), .events = POLLIN };
        poll(&fd, 1, timeout_ms);
    }
    XFlush(p->dpy);
    return VT_OK;
}
