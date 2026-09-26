/*
 * vt-panel.c — Vantage panel (X11 dock window)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * A real X11 dock: _NET_WM_WINDOW_TYPE_DOCK window spanning the primary
 * output, with _NET_WM_STRUT_PARTIAL so the WM reserves screen space.
 * Double-buffered XRender/Xft drawing, click handling, an IPC client
 * subscription to the WM for live tasklist/workspace updates.
 *
 * All applets are built-in C modules behind vt_applet_impl_t; external
 * plugins can be dlopen'd later through the public vt_panel_plugin_api_t.
 */

#define VT_LOG_DOMAIN "panel"
#include "vt-panel-internal.h"
#include <vantage/vt-config.h>
#include <vantage/vt-x11.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <math.h>

typedef struct {
    vt_panel_applet_kind_t kind;
    const vt_applet_impl_t *impl;
    void *state;
    bool inited;
    vt_rect_t area;
} _applet_t;



typedef struct vt_panel_priv {
    vt_pctx_t        ctx;
    Window           root;
    int              screen;
    int              root_w, root_h;
    vt_vec_t         slots;          /* _applet_t */
    vt_ipc_t        *ipc;
    bool             ipc_ok;
    uint64_t         last_tick;
    bool             need_layout;
    bool             need_paint;
    int              press_x, press_y;
    int              press_applet;
    /* popup override: an applet-owned override-redirect window whose
     * events are routed directly to the applet (launcher menu etc.) */
    Window           popup;
    void           (*popup_cb)(struct vt_panel *, XEvent *);
} vt_panel_priv_t;

static void _applet_init(vt_panel_t *p, _applet_t *a) {
    if (a->inited || !a->impl || !a->impl->init) return;
    vt_panel_priv_t *pv = p->priv;
    vt_applet_env_t env = { .panel = p, .ctx = &pv->ctx,
                            .area = a->area, .state = a->state };
    a->impl->init(&env);
    a->state = env.state;   /* impls allocate into env.state */
    a->inited = true;
}


void vt_panel_set_popup(vt_panel_t *p, Window w,
                        void (*cb)(struct vt_panel *, XEvent *)) {
    if (!p || !p->priv) return;
    vt_panel_priv_t *pv = p->priv;
    pv->popup = w;
    pv->popup_cb = cb;
}

vt_applet_env_t vt_panel_find_env(vt_panel_t *p, vt_panel_applet_kind_t kind) {
    if (!p || !p->priv) return (vt_applet_env_t){ .panel = p };
    vt_panel_priv_t *pv = p->priv;
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        if (a->kind == kind)
            return (vt_applet_env_t){ .panel = p, .ctx = &pv->ctx,
                                      .area = a->area, .state = a->state };
    }
    return (vt_applet_env_t){ .panel = p };
}

/* ------------------------------------------------------- drawing utils */
void vt_pctx_rounded_rect(vt_pctx_t *ctx, int x, int y, int w, int h,
                          int radius, vt_pcol_t fill) {
    if (radius <= 0 || radius * 2 > w || radius * 2 > h) {
        vt_pctx_rect(ctx, x, y, w, h, fill);
        return;
    }
    XRenderColor c = _col_to_xrc(fill);
    /* body minus corners */
    XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                          (short)(x + radius), (short)y,
                          (unsigned)(w - 2 * radius), (unsigned)h);
    XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                          (short)x, (short)(y + radius),
                          (unsigned)w, (unsigned)(h - 2 * radius));
    /* quarter-circle corners via 1-pixel approximation arcs */
    for (int i = 0; i < radius; i++) {
        int t = (int)((double)radius * sqrt(1.0 - ((double)(radius - i) *
                 (radius - i)) / ((double)radius * radius)));
        XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                              (short)(x + radius - t), (short)(y + radius - i),
                              1u, 1u);
        XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                              (short)(x + w - radius), (short)(y + radius - i),
                              1u, 1u);
        XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                              (short)(x + radius - t), (short)(y + h - radius + i),
                              1u, 1u);
        XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                              (short)(x + w - radius), (short)(y + h - radius + i),
                              1u, 1u);
    }
}

void vt_pctx_rect(vt_pctx_t *ctx, int x, int y, int w, int h, vt_pcol_t fill) {
    XRenderColor c = _col_to_xrc(fill);
    XRenderFillRectangle(ctx->dpy, PictOpOver, ctx->back_pic, &c,
                         (short)x, (short)y, (unsigned)w, (unsigned)h);
}

static XftColor *_xft_color(vt_pctx_t *ctx, vt_pcol_t c, XftColor *out) {
    XRenderColor x = _col_to_xrc(c);
    XftColor *existing = NULL;
    if (c.r == 0xec && c.g == 0xee && c.b == 0xf0) existing = &ctx->text_col;
    else if (c.r == 0x90 && c.g == 0x93 && c.b == 0x99) existing = &ctx->text_dim_col;
    if (existing && existing->pixel) { *out = *existing; return out; }
    XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, DefaultScreen(ctx->dpy)),
                  DefaultColormap(ctx->dpy, DefaultScreen(ctx->dpy)), &x, out);
    return out;
}

/* ---- per-codepoint font fallback --------------------------------
 * A single XftFont covers a limited charset; when the panel's default
 * sans lacks a glyph (Cyrillic on Cantarell-primary systems, CJK on
 * Western systems) the character would render as nothing. The helper
 * below picks, per codepoint, the first face that actually has the
 * glyph: the primary font, then lazily-opened fontconfig matches for
 * the specific character. This is what makes Russian .desktop names
 * render correctly without bundling a font. */
#define _VT_FB_FACES 4

static XftFont *_fb_face(vt_pctx_t *ctx, FcChar32 cp) {
    static XftFont *faces[_VT_FB_FACES];
    static FcChar32 cps[_VT_FB_FACES];
    static int n_faces;
    for (int i = 0; i < n_faces; i++)
        if (cps[i] == cp) return faces[i];
    if (n_faces >= _VT_FB_FACES) return faces[0];
    /* fontconfig: any sans font that actually covers this codepoint */
    FcPattern *pat = FcNameParse((const FcChar8 *)"sans");
    if (!pat) return NULL;
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddInteger(pat, FC_SIZE, ctx->font ? 10 : 10);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcPattern *mat = FcFontMatch(NULL, pat, &res);
    XftFont *f = NULL;
    if (mat) {
        f = XftFontOpenPattern(ctx->dpy, mat);
        if (f && !XftCharExists(ctx->dpy, f, cp)) {
            XftFontClose(ctx->dpy, f);
            f = NULL;
        } else if (f == NULL) {
            FcPatternDestroy(mat);
        }
    }
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    if (!f) return NULL;
    faces[n_faces] = f;
    cps[n_faces] = cp;
    n_faces++;
    return f;
}

static XftFont *_font_for_cp(vt_pctx_t *ctx, XftFont *primary, FcChar32 cp) {
    if (!primary || XftCharExists(ctx->dpy, primary, cp)) return primary;
    XftFont *fb = _fb_face(ctx, cp);
    return fb ? fb : primary;
}

/* decode one UTF-8 codepoint; returns bytes consumed */
static int _utf8_cp(const char *s, FcChar32 *out) {
    const unsigned char *u = (const unsigned char *)s;
    if ((u[0] & 0x80) == 0) { *out = u[0]; return 1; }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
        *out = ((FcChar32)(u[0] & 0x1f) << 6) | (u[1] & 0x3f);
        return 2;
    }
    if ((u[0] & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 &&
        (u[2] & 0xc0) == 0x80) {
        *out = ((FcChar32)(u[0] & 0x0f) << 12) |
               ((FcChar32)(u[1] & 0x3f) << 6) | (u[2] & 0x3f);
        return 3;
    }
    if ((u[0] & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 &&
        (u[2] & 0xc0) == 0x80 && (u[3] & 0xc0) == 0x80) {
        *out = ((FcChar32)(u[0] & 0x07) << 18) |
               ((FcChar32)(u[1] & 0x3f) << 12) |
               ((FcChar32)(u[2] & 0x3f) << 6) | (u[3] & 0x3f);
        return 4;
    }
    *out = u[0];
    return 1;
}

/* run-splitting draw core: works on any XftDraw target (the panel
 * back buffer, popup menu windows, …) so EVERY text surface gets the
 * same per-codepoint glyph fallback */
static int _text_fallback_draw(vt_pctx_t *ctx, XftDraw *dst, int x, int y,
                               const char *utf8, XftFont *f, XftColor *c) {
    int x0 = x;
    const char *p = utf8;
    while (*p) {
        FcChar32 cp;
        int n = _utf8_cp(p, &cp);
        XftFont *rf = _font_for_cp(ctx, f, cp);
        const char *run = p;
        int runlen = n;
        p += n;
        while (*p) {
            FcChar32 cp2;
            int n2 = _utf8_cp(p, &cp2);
            if (_font_for_cp(ctx, f, cp2) != rf) break;
            runlen += n2;
            p += n2;
        }
        XftDrawStringUtf8(dst, c, rf, x, y, (const FcChar8 *)run, runlen);
        XGlyphInfo gi;
        XftTextExtentsUtf8(ctx->dpy, rf, (const FcChar8 *)run, runlen, &gi);
        x += gi.xOff;
    }
    return x - x0;
}

int vt_pctx_text(vt_pctx_t *ctx, int x, int y, const char *utf8, bool bold,
                 vt_pcol_t col) {
    if (!utf8 || !*utf8) return 0;
    XftFont *f = bold ? ctx->font_bold : ctx->font;
    if (!f) f = ctx->font;
    if (!f) return 0;
    XftColor c;
    _xft_color(ctx, col, &c);
    return _text_fallback_draw(ctx, ctx->xft, x, y, utf8, f, &c);
}

int vt_pctx_menu_text(vt_pctx_t *ctx, XftDraw *dst, int x, int y,
                      const char *utf8, bool bold, vt_pcol_t col) {
    if (!ctx || !dst || !utf8 || !*utf8) return 0;
    XftFont *f = bold ? ctx->font_bold : ctx->font;
    if (!f) f = ctx->font;
    if (!f) return 0;
    XftColor c;
    _xft_color(ctx, col, &c);
    return _text_fallback_draw(ctx, dst, x, y, utf8, f, &c);
}

int vt_pctx_text_width(vt_pctx_t *ctx, const char *utf8, bool bold) {
    XftFont *f = bold ? ctx->font_bold : ctx->font;
    if (!f || !utf8) return 0;
    int x = 0;
    const char *p = utf8;
    while (*p) {
        FcChar32 cp;
        int n = _utf8_cp(p, &cp);
        XftFont *rf = _font_for_cp(ctx, f, cp);
        const char *run = p;
        int runlen = n;
        p += n;
        while (*p) {
            FcChar32 cp2;
            int n2 = _utf8_cp(p, &cp2);
            if (_font_for_cp(ctx, f, cp2) != rf) break;
            runlen += n2;
            p += n2;
        }
        XGlyphInfo gi;
        XftTextExtentsUtf8(ctx->dpy, rf, (const FcChar8 *)run, runlen, &gi);
        x += gi.xOff;
    }
    return x;
}

/* Draw an ARGB-8888 image (with alpha) onto any XRender Picture
 * target (panel back buffer, menu windows …). Used for application
 * and window icons resolved from the user's icon theme. */
void vt_pctx_draw_argb_pic(Display *dpy, Picture dst, int dx, int dy,
                           int dw, int dh, const uint32_t *argb,
                           int sw, int sh) {
    if (!dpy || !dst || !argb || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    /* nearest-neighbor rescale into a depth-32 XImage */
    XImage *img = XCreateImage(dpy, NULL, 32, ZPixmap, 0, NULL,
                               (unsigned)dw, (unsigned)dh, 32, 0);
    if (!img) return;
    img->data = vt_malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        uint32_t *row = (uint32_t *)(img->data + (size_t)y * img->bytes_per_line);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            uint32_t p = argb[sy * sw + sx];
            /* X ZPixmap little-endian: BGRX byte order */
            row[x] = ((p & 0xff000000u) >> 24) << 24 |
                     ((p & 0x000000ffu) << 16) |
                     (p & 0x0000ff00u) |
                     ((p & 0x00ff0000u) >> 16);
        }
    }
    Pixmap px = XCreatePixmap(dpy, DefaultRootWindow(dpy),
                              (unsigned)dw, (unsigned)dh, 32);
    GC gc = XCreateGC(dpy, px, 0, NULL);
    XPutImage(dpy, px, gc, img, 0, 0, 0, 0, (unsigned)dw, (unsigned)dh);
    XFreeGC(dpy, gc);
    XDestroyImage(img);
    XRenderPictFormat *fmt =
        XRenderFindStandardFormat(dpy, PictStandardARGB32);
    Picture pic = XRenderCreatePicture(dpy, px, fmt, 0, NULL);
    XRenderComposite(dpy, PictOpOver, pic, None, dst,
                     0, 0, 0, 0, dx, dy, (unsigned)dw, (unsigned)dh);
    XRenderFreePicture(dpy, pic);
    XFreePixmap(dpy, px);
}

void vt_pctx_draw_argb(vt_pctx_t *ctx, int dx, int dy, int dw, int dh,
                       const uint32_t *argb, int sw, int sh) {
    if (!ctx) return;
    vt_pctx_draw_argb_pic(ctx->dpy, ctx->back_pic, dx, dy, dw, dh,
                          argb, sw, sh);
}

int vt_pctx_text_height(vt_pctx_t *ctx) {
    if (!ctx->font) return 16;
    return ctx->font->ascent + ctx->font->descent;
}

void vt_panel_spawn(const char *cmd) {
    if (!cmd || !*cmd) return;
    vt_proc_spawn_detached(cmd);
}

/* ------------------------------------------------------------ ipc glue */
static int _h_wm_event(vt_ipc_t *ipc, const vt_ipc_msg_t *req,
                       vt_ipc_msg_t *resp, void *ud) {
    (void)ipc; (void)resp;
    vt_panel_t *panel = ud;
    if (req && req->payload && req->len)
        vt_panel_feed_event(panel, (const char *)req->payload);
    return 0;
}

/* client-side IPC dispatch: server events arrive via registered handlers */
static int _ipc_client_step(vt_panel_t *panel, int timeout_ms) {
    vt_panel_priv_t *p = panel->priv;
    if (!p->ipc) return 1;
    int rc = vt_ipc_step(p->ipc, timeout_ms);
    if (rc < 0 && rc != 1)
        p->ipc_ok = false;
    return 0;
}

/* Ask the WM for the current window list; returns a heap copy of the
 * response text (caller frees) or NULL. */
char *vt_panel_query_windows(vt_panel_t *p) {
    vt_panel_priv_t *pv = p ? p->priv : NULL;
    if (!pv || !pv->ipc) return NULL;
    vt_ipc_msg_t resp = {0};
    if (vt_ipc_call(pv->ipc, VT_IPC_MSG_WM_QUERY, "", 0, &resp, 1500)
        != VT_IPC_OK)
        return NULL;
    char *out = vt_strndup(resp.payload ? (const char *)resp.payload : "",
                           resp.len);
    vt_ipc_msg_free(&resp);
    return out;
}

char *vt_panel_query_workspaces(vt_panel_t *p) {
    vt_panel_priv_t *pv = p ? p->priv : NULL;
    if (!pv || !pv->ipc) return NULL;
    vt_ipc_msg_t resp = {0};
    if (vt_ipc_call(pv->ipc, VT_IPC_MSG_WM_WS_QUERY, "", 0, &resp, 1500)
        != VT_IPC_OK)
        return NULL;
    char *out = vt_strndup(resp.payload ? (const char *)resp.payload : "",
                           resp.len);
    vt_ipc_msg_free(&resp);
    return out;
}

void vt_panel_send_wm(vt_panel_t *p, uint32_t msg, const char *payload) {
    vt_panel_priv_t *pv = p ? p->priv : NULL;
    if (!pv || !pv->ipc) return;
    vt_ipc_msg_t resp = {0};
    vt_ipc_call(pv->ipc, msg, payload ? payload : "",
                payload ? (uint32_t)strlen(payload) : 0, &resp, 500);
    vt_ipc_msg_free(&resp);
}

void vt_panel_send_session(uint32_t msg, const char *payload) {
    /* The session manager serves the same protocol on its own socket in
     * XDG_RUNTIME_DIR; logout and friends are served there. */
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) {
        vt_logw("panel: XDG_RUNTIME_DIR unset — cannot reach the session "
                "manager");
        return;
    }
    char *path = vt_strprintf("%s/vantage-session.sock", rd);
    vt_ipc_t *ipc = vt_ipc_new_client(path);
    vt_free(path);
    if (!ipc) {
        vt_logw("panel: cannot connect to vantage-session.sock — is the "
                "session manager running?");
        return;
    }
    vt_ipc_msg_t resp = {0};
    vt_ipc_call(ipc, msg, payload ? payload : "",
                payload ? (uint32_t)strlen(payload) : 0, &resp, 1500);
    vt_ipc_msg_free(&resp);
    vt_ipc_free(ipc);
}

/* ------------------------------------------------------------ applets */
int vt_panel_add_applet(vt_panel_t *p, vt_panel_applet_kind_t kind) {
    if (!p || !p->priv) return -1;
    vt_panel_priv_t *pv = p->priv;
    if (pv->slots.size >= 16) return -1;
    const vt_applet_impl_t *impl = NULL;
    switch (kind) {
    case VT_PANEL_APPLET_LAUNCHER:   impl = &_applet_launcher; break;
    case VT_PANEL_APPLET_TASKLIST:   impl = &_applet_tasklist; break;
    case VT_PANEL_APPLET_CLOCK:      impl = &_applet_clock; break;
    case VT_PANEL_APPLET_WORKSPACES: impl = &_applet_workspaces; break;
    case VT_PANEL_APPLET_TRAY:       impl = &_applet_tray; break;
    case VT_PANEL_APPLET_VOLUME:     impl = &_applet_volume; break;
    case VT_PANEL_APPLET_NETWORK:    impl = &_applet_network; break;
    case VT_PANEL_APPLET_BATTERY:    impl = &_applet_battery; break;
    case VT_PANEL_APPLET_USER:       impl = &_applet_user; break;
    default: return -1;
    }
    _applet_t a = { .kind = kind, .impl = impl, .state = NULL,
                    .inited = false, .area = { 0, 0, 0, 0 } };
    vt_vec_push(&pv->slots, &a);
    if (p->running) {
        _applet_t *slot = vt_vec_at(&pv->slots, pv->slots.size - 1);
        _applet_init(p, slot);
    }
    pv->need_layout = true;
    return (int)pv->slots.size - 1;
}

int vt_panel_remove_applet(vt_panel_t *p, int idx) {
    if (!p || !p->priv) return -1;
    vt_panel_priv_t *pv = p->priv;
    if (idx < 0 || (size_t)idx >= pv->slots.size) return -1;
    _applet_t *a = vt_vec_at(&pv->slots, (size_t)idx);
    if (a->impl && a->impl->fini)
        a->impl->fini(&(vt_applet_env_t){ .panel = p, .ctx = &pv->ctx,
                                          .area = a->area, .state = a->state });
    vt_vec_remove(&pv->slots, (size_t)idx);
    pv->need_layout = true;
    return 0;
}

/* ------------------------------------------------------------- layout */
static void _layout(vt_panel_t *p) {
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;
    int h = ctx->h;
    int x = 6;
    /* measure all applets */
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        int w = 0;
        if (a->impl && a->impl->measure)
            w = a->impl->measure(&(vt_applet_env_t){
                .panel = p, .ctx = ctx, .area = a->area, .state = a->state });
        a->area = (vt_rect_t){ .x = x, .y = 2, .w = w, .h = h - 4 };
        x += w + 8;
    }
    /* right-aligned applets: user/clock/volume/workspaces/battery/
     * network/tray get pinned to the right edge in reverse registration
     * order — the intended reading order (left→right) ends up:
     *   [workspaces] [volume] [clock] [username]  (plus battery/net/tray) */
    int xr = ctx->w - 6;
    for (size_t i = pv->slots.size; i > 0; i--) {
        _applet_t *a = vt_vec_at(&pv->slots, i - 1);
        bool right = a->kind == VT_PANEL_APPLET_CLOCK ||
                     a->kind == VT_PANEL_APPLET_USER ||
                     a->kind == VT_PANEL_APPLET_BATTERY ||
                     a->kind == VT_PANEL_APPLET_NETWORK ||
                     a->kind == VT_PANEL_APPLET_VOLUME ||
                     a->kind == VT_PANEL_APPLET_WORKSPACES ||
                     a->kind == VT_PANEL_APPLET_TRAY;
        if (!right) continue;
        a->area.x = xr - a->area.w;
        xr -= a->area.w + 8;
        if (xr < x) xr = x; /* overlap guard */
    }
    pv->need_layout = false;
}

static void _paint(vt_panel_t *p) {
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;
    /* background */
    XRenderColor bg = ctx->bg_col;
    XRenderFillRectangle(ctx->dpy, PictOpSrc, ctx->back_pic, &bg, 0, 0,
                         (unsigned)ctx->w, (unsigned)ctx->h);
    /* applet separator accents */
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        if (a->impl && a->impl->render)
            a->impl->render(&(vt_applet_env_t){
                .panel = p, .ctx = ctx, .area = a->area, .state = a->state });
    }
    /* present */
    XRenderComposite(ctx->dpy, PictOpSrc, ctx->back_pic, None, ctx->win_pic,
                     0, 0, 0, 0, 0, 0, (unsigned)ctx->w, (unsigned)ctx->h);
    XFlush(ctx->dpy);
    pv->need_paint = false;
}

void vt_panel_render(vt_panel_t *p) {
    if (p && p->priv) ((vt_panel_priv_t *)p->priv)->need_paint = true;
}

/* --------------------------------------------------------------- X glue */
static void _ensure_window(vt_panel_t *p) {
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;
    Display *dpy = ctx->dpy;
    int y = 0;
    if (p->pos == VT_PANEL_POS_BOTTOM)
        y = pv->root_h - p->height;
    XMoveResizeWindow(dpy, ctx->win, 0, y, (unsigned)pv->root_w,
                      (unsigned)p->height);
    /* strut */
    unsigned long strut[12] = {0};
    if (p->pos == VT_PANEL_POS_TOP) {
        strut[2] = (unsigned long)(p->height + y); /* top */
        strut[8] = 0; strut[9] = (unsigned long)(pv->root_w - 1);
    } else {
        strut[3] = (unsigned long)p->height;       /* bottom */
        strut[10] = 0; strut[11] = (unsigned long)(pv->root_w - 1);
    }
    Atom strut_p = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    XChangeProperty(dpy, ctx->win, strut_p, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)strut, 12);
    unsigned long strut4[4] = { 0, 0, strut[2], strut[3] };
    XChangeProperty(dpy, ctx->win, XInternAtom(dpy, "_NET_WM_STRUT", False),
                    XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)strut4, 4);
}

int vt_panel_start(vt_panel_t *p) {
    if (!p) return VT_ERR_INVAL;
    vt_panel_priv_t *pv = p->priv;
    if (!pv) return VT_ERR_INVAL;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        vt_loge("panel: cannot open display");
        return VT_ERR;
    }
    pv->screen = DefaultScreen(dpy);
    pv->root = RootWindow(dpy, pv->screen);
    pv->root_w = DisplayWidth(dpy, pv->screen);
    pv->root_h = DisplayHeight(dpy, pv->screen);
    vt_pctx_t *ctx = &pv->ctx;
    ctx->dpy = dpy;
    ctx->w = pv->root_w;
    ctx->h = p->height;
    ctx->bg_col = (XRenderColor){ .red = 0x1a1a, .green = 0x1c1c,
                                  .blue = 0x2222, .alpha = 0xd800 };

    XSetWindowAttributes wa = { .override_redirect = False,
                                .background_pixel = None,
                                .event_mask = ExposureMask | ButtonPressMask |
                                              ButtonReleaseMask |
                                              PointerMotionMask |
                                              StructureNotifyMask };
    ctx->win = XCreateWindow(dpy, pv->root, 0, 0,
                              (unsigned)ctx->w, (unsigned)p->height, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWEventMask,
                              &wa);
    /* EWMH dock hints */
    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, ctx->win, type, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&dock, 1);
    Atom wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, ctx->win, wm_name, utf8, 8, PropModeReplace,
                    (const unsigned char *)"Vantage Panel", 13);
    XStoreName(dpy, ctx->win, "Vantage Panel");
    /* stay on all desktops */
    Atom wm_desk = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    unsigned long all = 0xffffffff;
    XChangeProperty(dpy, ctx->win, wm_desk, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&all, 1);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom skip_tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    XChangeProperty(dpy, ctx->win, state, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&skip_tb, 1);

    _ensure_window(p);

    /* render targets */
    ctx->back = XCreatePixmap(dpy, ctx->win, (unsigned)ctx->w,
                              (unsigned)p->height,
                              (unsigned)DefaultDepth(dpy, pv->screen));
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
        DefaultVisual(dpy, pv->screen));
    ctx->win_pic = XRenderCreatePicture(dpy, ctx->win, fmt, 0, NULL);
    ctx->back_pic = XRenderCreatePicture(dpy, ctx->back, fmt, 0, NULL);
    ctx->gc = XCreateGC(dpy, ctx->win, 0, NULL);
    ctx->xft = XftDrawCreate(dpy, ctx->back, DefaultVisual(dpy, pv->screen),
                             DefaultColormap(dpy, pv->screen));
    const char *family = getenv("VANTAGE_FONT");
    if (!family || !*family) family = "Sans";
    char *spec = vt_strprintf("%s:size=%d", family,
                              p->height > 30 ? 10 : 9);
    ctx->font = XftFontOpenName(dpy, pv->screen, spec);
    char *spec_b = vt_strprintf("%s:size=%d:style=bold", family,
                                p->height > 30 ? 10 : 9);
    ctx->font_bold = XftFontOpenName(dpy, pv->screen, spec_b);
    vt_free(spec);
    vt_free(spec_b);
    if (!ctx->font) {
        vt_loge("panel: cannot open font");
        return VT_ERR;
    }
    /* text colors */
    XRenderColor tc = { .red = 0xec00 + 0xec, .green = 0xee00 + 0xee,
                        .blue = 0xf000 + 0xf0, .alpha = 0xffff };
    tc.red = 0xecec; tc.green = 0xeeee; tc.blue = 0xf0f0;
    XftColorAllocValue(dpy, DefaultVisual(dpy, pv->screen),
                  DefaultColormap(dpy, pv->screen), &tc, &ctx->text_col);
    XRenderColor dc = { .red = 0x9090, .green = 0x9393, .blue = 0x9999,
                        .alpha = 0xffff };
    XftColorAllocValue(dpy, DefaultVisual(dpy, pv->screen),
                  DefaultColormap(dpy, pv->screen), &dc, &ctx->text_dim_col);
    XRenderColor ac = { .red = 0x4f4f, .green = 0x9a9a, .blue = 0xdcdc,
                        .alpha = 0xffff };
    XftColorAllocValue(dpy, DefaultVisual(dpy, pv->screen),
                  DefaultColormap(dpy, pv->screen), &ac, &ctx->accent_col);

    XMapWindow(dpy, ctx->win);
    XFlush(dpy);

    /* IPC client: subscribe to WM events */
    pv->ipc = vt_ipc_new_client(NULL);
    vt_ipc_register(pv->ipc, VT_IPC_MSG_WM_EVENT, _h_wm_event, p);
    vt_ipc_register(pv->ipc, VT_IPC_MSG_WM_WS_EVENT, _h_wm_event, p);
    if (vt_ipc_send(pv->ipc, VT_IPC_MSG_SUBSCRIBE, VT_IPC_MSG_REQUEST, "",
                    0) == VT_IPC_OK)
        pv->ipc_ok = true;

    p->running = true;
    p->visible = true;
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        _applet_init(p, a);
    }
    pv->need_layout = true;
    pv->need_paint = true;
    vt_logi("panel: started (%dx%d, %zu applets, ipc=%s)",
            ctx->w, p->height, pv->slots.size, pv->ipc_ok ? "yes" : "no");
    return VT_OK;
}

void vt_panel_stop(vt_panel_t *p) {
    if (!p || !p->priv) return;
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        if (a->impl && a->impl->fini)
            a->impl->fini(&(vt_applet_env_t){ .panel = p, .ctx = ctx,
                                              .area = a->area,
                                              .state = a->state });
    }
    if (pv->ipc) vt_ipc_free(pv->ipc);
    if (ctx->xft) XftDrawDestroy(ctx->xft);
    if (ctx->font) XftFontClose(ctx->dpy, ctx->font);
    if (ctx->font_bold) XftFontClose(ctx->dpy, ctx->font_bold);
    if (ctx->win_pic) XRenderFreePicture(ctx->dpy, ctx->win_pic);
    if (ctx->back_pic) XRenderFreePicture(ctx->dpy, ctx->back_pic);
    if (ctx->back) XFreePixmap(ctx->dpy, ctx->back);
    if (ctx->gc) XFreeGC(ctx->dpy, ctx->gc);
    if (ctx->win) XDestroyWindow(ctx->dpy, ctx->win);
    if (ctx->dpy) XCloseDisplay(ctx->dpy);
    p->running = false;
}

vt_panel_t *vt_panel_new(vt_renderer_t *r) {
    (void)r;
    vt_panel_t *p = vt_malloc0(sizeof(*p));
    p->pos = VT_PANEL_POS_TOP;
    p->height = 32;
    p->visible = true;
    p->priv = vt_malloc0(sizeof(vt_panel_priv_t));
    vt_panel_priv_t *pv = p->priv;
    vt_vec_init(&pv->slots, sizeof(_applet_t), 8);
    return p;
}

void vt_panel_free(vt_panel_t *p) {
    if (!p) return;
    vt_panel_stop(p);
    if (p->priv) {
        vt_panel_priv_t *pv = p->priv;
        vt_vec_fini(&pv->slots);
        vt_free(pv);
    }
    vt_free(p);
}

void vt_panel_set_pos(vt_panel_t *p, vt_panel_pos_t pos) {
    if (!p) return;
    p->pos = pos;
    if (p->running && p->priv) {
        _ensure_window(p);
        ((vt_panel_priv_t *)p->priv)->need_paint = true;
    }
}
void vt_panel_set_height(vt_panel_t *p, int h) {
    if (!p || h < 16 || h > 128) return;
    p->height = h;
    if (p->running && p->priv) {
        vt_panel_priv_t *pv = p->priv;
        vt_pctx_t *ctx = &pv->ctx;
        XftDrawDestroy(ctx->xft);
        XftFontClose(ctx->dpy, ctx->font);
        if (ctx->font_bold) XftFontClose(ctx->dpy, ctx->font_bold);
        XRenderFreePicture(ctx->dpy, ctx->back_pic);
        XFreePixmap(ctx->dpy, ctx->back);
        ctx->h = h;
        ctx->back = XCreatePixmap(ctx->dpy, ctx->win, (unsigned)ctx->w,
                                  (unsigned)h,
                                  (unsigned)DefaultDepth(ctx->dpy, pv->screen));
        XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy,
            DefaultVisual(ctx->dpy, pv->screen));
        ctx->back_pic = XRenderCreatePicture(ctx->dpy, ctx->back, fmt, 0, NULL);
        ctx->xft = XftDrawCreate(ctx->dpy, ctx->back,
                                 DefaultVisual(ctx->dpy, pv->screen),
                                 DefaultColormap(ctx->dpy, pv->screen));
        char *spec = vt_strprintf("%s:size=%d", "Sans", h > 30 ? 10 : 9);
        ctx->font = XftFontOpenName(ctx->dpy, pv->screen, spec);
        vt_free(spec);
        _ensure_window(p);
        pv->need_layout = true;
        pv->need_paint = true;
    }
}

/* ---------------------------------------------------------- event loop */
static void _dispatch_x(vt_panel_t *p) {
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;
    while (XPending(ctx->dpy)) {
        XEvent ev;
        XNextEvent(ctx->dpy, &ev);
        if (pv->popup && ev.xany.window == pv->popup && pv->popup_cb) {
            pv->popup_cb(p, &ev);
            continue;
        }
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0) pv->need_paint = true;
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != ctx->w) {
                ctx->w = ev.xconfigure.width;
                XRenderFreePicture(ctx->dpy, ctx->back_pic);
                XFreePixmap(ctx->dpy, ctx->back);
                ctx->back = XCreatePixmap(ctx->dpy, ctx->win,
                                          (unsigned)ctx->w,
                                          (unsigned)p->height,
                                          (unsigned)DefaultDepth(ctx->dpy,
                                                                 pv->screen));
                XRenderPictFormat *fmt = XRenderFindVisualFormat(ctx->dpy,
                    DefaultVisual(ctx->dpy, pv->screen));
                ctx->back_pic = XRenderCreatePicture(ctx->dpy, ctx->back, fmt,
                                                     0, NULL);
                pv->need_layout = true;
                pv->need_paint = true;
            }
            break;
        case ButtonPress: {
            XButtonEvent *be = &ev.xbutton;
            int ax = be->x, ay = be->y;
            /* wheel events arrive as buttons 4/5 */
            if (be->button == 4 || be->button == 5) {
                for (size_t i = 0; i < pv->slots.size; i++) {
                    _applet_t *a = vt_vec_at(&pv->slots, i);
                    if (ax >= a->area.x && ax < a->area.x + a->area.w &&
                        ay >= a->area.y && ay < a->area.y + a->area.h) {
                        if (a->impl && a->impl->on_wheel)
                            a->impl->on_wheel(&(vt_applet_env_t){
                                .panel = p, .ctx = ctx, .area = a->area,
                                .state = a->state },
                                be->button == 4 ? 1 : -1);
                        break;
                    }
                }
                break;
            }
            for (size_t i = 0; i < pv->slots.size; i++) {
                _applet_t *a = vt_vec_at(&pv->slots, i);
                if (ax >= a->area.x && ax < a->area.x + a->area.w &&
                    ay >= a->area.y && ay < a->area.y + a->area.h) {
                    if (a->impl && a->impl->on_click)
                        a->impl->on_click(&(vt_applet_env_t){
                            .panel = p, .ctx = ctx, .area = a->area,
                            .state = a->state }, ax - a->area.x,
                            ay - a->area.y, be->button);
                    break;
                }
            }
            pv->need_paint = true;
            break;
        }
        default:
            break;
        }
    }
}

/* public applet state accessor used by applets */
vt_rect_t *vt_panel_applet_area(vt_panel_t *p, int idx);
vt_rect_t *vt_panel_applet_area(vt_panel_t *p, int idx) {
    if (!p || !p->priv) return NULL;
    vt_panel_priv_t *pv = p->priv;
    if (idx < 0 || (size_t)idx >= pv->slots.size) return NULL;
    _applet_t *a = vt_vec_at(&pv->slots, (size_t)idx);
    return &a->area;
}

void vt_panel_invalidate(void *panel) {
    vt_panel_t *p = panel;
    if (p && p->priv) ((vt_panel_priv_t *)p->priv)->need_paint = true;
}

/* feed an IPC event line into applets (used by the ipc client handler) */
void vt_panel_feed_event(vt_panel_t *p, const char *line) {
    if (!p || !p->priv || !line) return;
    vt_panel_priv_t *pv = p->priv;
    for (size_t i = 0; i < pv->slots.size; i++) {
        _applet_t *a = vt_vec_at(&pv->slots, i);
        if (a->impl && a->impl->on_ipc_event)
            a->impl->on_ipc_event(&(vt_applet_env_t){
                .panel = p, .ctx = &pv->ctx, .area = a->area,
                .state = a->state }, VT_IPC_MSG_WM_EVENT, line);
    }
    pv->need_layout = true;
    pv->need_paint = true;
}

int vt_panel_step(vt_panel_t *p, int timeout_ms) {
    if (!p || !p->running || !p->priv) return VT_ERR_INVAL;
    vt_panel_priv_t *pv = p->priv;
    vt_pctx_t *ctx = &pv->ctx;

    _dispatch_x(p);
    _ipc_client_step(p, 0);

    uint64_t now = vt_time_now_ms();
    if (now - pv->last_tick >= 1000) {
        pv->last_tick = now;
        for (size_t i = 0; i < pv->slots.size; i++) {
            _applet_t *a = vt_vec_at(&pv->slots, i);
            if (a->impl && a->impl->on_tick)
                a->impl->on_tick(&(vt_applet_env_t){
                    .panel = p, .ctx = ctx, .area = a->area,
                    .state = a->state }, now);
        }
        pv->need_paint = true;
    }
    if (pv->need_layout) _layout(p);
    if (pv->need_paint) _paint(p);

    /* idle wait: X fd + ipc fd */
    struct pollfd fds[2];
    int n = 0;
    fds[n].fd = ConnectionNumber(ctx->dpy);
    fds[n].events = POLLIN;
    n++;
    int ifd = pv->ipc ? vt_ipc_get_fd(pv->ipc) : -1;
    if (ifd >= 0) {
        fds[n].fd = ifd;
        fds[n].events = POLLIN;
        n++;
    }
    poll(fds, (nfds_t)n, timeout_ms);
    return VT_OK;
}
