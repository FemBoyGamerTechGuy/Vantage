/*
 * vt-panel-internal.h — internal panel drawing API (not installed)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * The panel renders with Xlib + XRender + Xft on its own connection.
 * Applets draw through this small API, which keeps them independent of
 * the vt_renderer_t abstraction (the panel is a normal X client; it does
 * not own the screen).
 */
#ifndef VANTAGE_PANEL_INTERNAL_H
#define VANTAGE_PANEL_INTERNAL_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <vantage/vt-panel.h>
#include <vantage/vt-ipc.h>

typedef struct {
    unsigned char r, g, b, a;
} vt_pcol_t;

typedef struct {
    Display        *dpy;
    Window          win;
    Pixmap          back;          /* double buffer */
    Picture         back_pic;
    Picture         win_pic;
    XRenderColor    bg_col;
    XftDraw        *xft;
    XftFont        *font;          /* normal */
    XftFont        *font_bold;
    XftColor        text_col;
    XftColor        text_dim_col;
    XftColor        accent_col;
    Picture         accent_pic;
    Pixmap          accent_px;
    int             w, h;
    GC              gc;
} vt_pctx_t;

/* colors */
static inline XRenderColor _col_to_xrc(vt_pcol_t c) {
    XRenderColor x = { .red = (unsigned short)(c.r * 0x101),
                       .green = (unsigned short)(c.g * 0x101),
                       .blue = (unsigned short)(c.b * 0x101),
                       .alpha = (unsigned short)(c.a * 0x101) };
    return x;
}

/* rounded rect via XRender (fallback: plain rect) */
void vt_pctx_rounded_rect(vt_pctx_t *ctx, int x, int y, int w, int h,
                          int radius, vt_pcol_t fill);
void vt_pctx_rect(vt_pctx_t *ctx, int x, int y, int w, int h, vt_pcol_t fill);
/* UTF-8 text; returns pixel width */
int  vt_pctx_text(vt_pctx_t *ctx, int x, int y, const char *utf8,
                  bool bold, vt_pcol_t col);
int  vt_pctx_text_width(vt_pctx_t *ctx, const char *utf8, bool bold);
int  vt_pctx_text_height(vt_pctx_t *ctx);
/* draw an ARGB-8888 image (alpha-composited) — used for icons */
void vt_pctx_draw_argb(vt_pctx_t *ctx, int dx, int dy, int dw, int dh,
                       const uint32_t *argb, int sw, int sh);
/* same, onto an arbitrary XRender Picture (popup menu windows) */
void vt_pctx_draw_argb_pic(Display *dpy, Picture dst, int dx, int dy,
                           int dw, int dh, const uint32_t *argb,
                           int sw, int sh);
/* UTF-8 text with font fallback onto any XftDraw target (menus) */
int  vt_pctx_menu_text(vt_pctx_t *ctx, XftDraw *dst, int x, int y,
                       const char *utf8, bool bold, vt_pcol_t col);

/* applet instance context — what each applet gets */
typedef struct vt_panel vt_panel_t;

typedef struct {
    vt_panel_t *panel;
    vt_pctx_t  *ctx;
    vt_rect_t   area;          /* assigned layout area */
    void       *state;         /* applet-private */
} vt_applet_env_t;

/* Built-in applet vtable (internal; the public plugin API in vt-panel.h
 * is a thin wrapper around this). */
typedef struct {
    const char *name;
    void (*init)(vt_applet_env_t *env);
    void (*fini)(vt_applet_env_t *env);
    int  (*measure)(vt_applet_env_t *env);              /* preferred width */
    void (*render)(vt_applet_env_t *env);
    void (*on_click)(vt_applet_env_t *env, int x, int y, int button);
    void (*on_wheel)(vt_applet_env_t *env, int dir);    /* +1 up / -1 down */
    void (*on_tick)(vt_applet_env_t *env, uint64_t now_ms); /* periodic */
    void (*on_ipc_event)(vt_applet_env_t *env, uint32_t msg, const char *payload);
} vt_applet_impl_t;

extern const vt_applet_impl_t _applet_launcher;
extern const vt_applet_impl_t _applet_tasklist;
extern const vt_applet_impl_t _applet_clock;
extern const vt_applet_impl_t _applet_workspaces;
extern const vt_applet_impl_t _applet_volume;
extern const vt_applet_impl_t _applet_network;
extern const vt_applet_impl_t _applet_battery;
extern const vt_applet_impl_t _applet_tray;
extern const vt_applet_impl_t _applet_user;

/* helper: launch a command */
void vt_panel_spawn(const char *cmd);

/* Session-manager IPC: send a command to vantage-session.sock
 * (logout etc.) — a thin client that connects, sends, disconnects. */
void vt_panel_send_session(uint32_t msg, const char *payload);

/* Open the username/session menu (used by the launcher's Quit Session
 * entry as well — implemented by the USER applet). */
void vt_panel_user_menu_open(struct vt_panel *p);

/* Find the applet env for a kind (state may be NULL if not added). */
vt_applet_env_t vt_panel_find_env(vt_panel_t *p, vt_panel_applet_kind_t kind);
/* Popup routing: route events for window `w` to `cb` (menu windows). */
void vt_panel_set_popup(vt_panel_t *p, Window w,
                        void (*cb)(struct vt_panel *, XEvent *));

/* IPC helpers (implemented in vt-panel.c) */
char *vt_panel_query_windows(vt_panel_t *p);
char *vt_panel_query_workspaces(vt_panel_t *p);
void  vt_panel_send_wm(vt_panel_t *p, uint32_t msg, const char *payload);
void  vt_panel_invalidate(void *panel);
void  vt_panel_feed_event(vt_panel_t *p, const char *line);
int   vt_panel_step(vt_panel_t *p, int timeout_ms);

#endif
