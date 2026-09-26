/*
 * vt-compositor-x11.c — X11 composite manager engine
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Uses XComposite (manual redirection), XDamage and XRender:
 *   - all root children are redirected; the compositor is the only thing
 *     that paints the root, so override-redirect menus/tooltips, docks and
 *     normal clients are handled uniformly
 *   - damage-driven: repaints only when a DamageNotify arrives, paced to
 *     the output refresh rate (software vsync)
 *   - honors _NET_WM_WINDOW_OPACITY via a repeating 1x1 alpha mask
 *   - optional soft shadows (radial gradient), default OFF (performance
 *     first — zero cost when disabled)
 *   - fullscreen fast path: a fully-covering opaque top window skips all
 *     windows below it
 *   - no animation loop when idle: zero CPU when nothing is damaged
 */

#define VT_LOG_DOMAIN "compositor-x11"
#include <vantage/vt-compositor.h>
#include <vantage/vt-x11.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-wm.h>

#if defined(VT_HAVE_X11) && defined(VT_HAVE_XCOMPOSITE) && \
    defined(VT_HAVE_XDAMAGE) && defined(VT_HAVE_XRENDER) && defined(VT_HAVE_XFIXES)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <string.h>
#include <stdlib.h>

typedef struct _cwin {
    Window           win;
    Pixmap           pixmap;
    Picture          picture;
    Damage           damage;
    XRenderPictFormat *fmt;
    int              x, y, w, h;
    bool             viewable;
    bool             argb;
    unsigned long    opacity;
    bool             damaged;
} _cwin_t;

struct vt_compositor_x11 {
    vt_compositor_t *comp;
    struct vt_wm    *wm;
    Display         *dpy;
    Window           root;
    Window           owner;          /* selection owner window */
    vt_vec_t         wins;           /* _cwin_t* */
    Picture          root_pic;
    Pixmap           back;
    Picture          back_pic;
    int              back_w, back_h;
    Picture          alpha_1x1;      /* repeating alpha mask */
    Pixmap           alpha_px;
    Picture          shadow_pic;     /* radial gradient alpha */
    Picture          solid_black;
    int              damage_base;
    bool             redirected;
    int              sink_id;
    bool             dirty;          /* need repaint */
    uint64_t         last_paint_us;
    int              frame_us;       /* pacing interval */
    int              screen_w, screen_h;
};

static struct vt_compositor_x11 *_cm = NULL;

/* ------------------------------------------------------------ helpers */
static void _cw_free(struct vt_compositor_x11 *cm, _cwin_t *cw) {
    if (cw->damage) XDamageDestroy(cm->dpy, cw->damage);
    if (cw->picture) XRenderFreePicture(cm->dpy, cw->picture);
    if (cw->pixmap) XFreePixmap(cm->dpy, cw->pixmap);
    vt_free(cw);
}

static _cwin_t *_cw_find(struct vt_compositor_x11 *cm, Window w) {
    for (size_t i = 0; i < cm->wins.size; i++) {
        _cwin_t *c = *(_cwin_t **)vt_vec_at(&cm->wins, i);
        if (c->win == w) return c;
    }
    return NULL;
}

static void _cw_remove(struct vt_compositor_x11 *cm, Window w) {
    for (size_t i = 0; i < cm->wins.size; i++) {
        _cwin_t **pp = vt_vec_at(&cm->wins, i);
        if ((*pp)->win == w) {
            _cw_free(cm, *pp);
            vt_vec_remove(&cm->wins, i);
            cm->dirty = true;
            return;
        }
    }
}

static void _cw_sync_surface(struct vt_compositor_x11 *cm, _cwin_t *cw) {
    /* (re)bind the server-side name-window-pixmap + picture + damage */
    XWindowAttributes wa;
    if (!XGetWindowAttributes(cm->dpy, cw->win, &wa)) {
        cw->viewable = false;
        return;
    }
    cw->x = wa.x; cw->y = wa.y;
    cw->w = wa.width; cw->h = wa.height;
    cw->viewable = (wa.map_state == IsViewable);
    cw->argb = (wa.depth == 32);
    if (!cw->viewable) return;
    if (cw->picture && cw->w > 0 && cw->h > 0) return; /* unchanged */
    if (cw->picture) {
        XRenderFreePicture(cm->dpy, cw->picture);
        cw->picture = 0;
    }
    if (cw->pixmap) {
        XFreePixmap(cm->dpy, cw->pixmap);
        cw->pixmap = 0;
    }
    cw->fmt = XRenderFindVisualFormat(cm->dpy, wa.visual);
    if (!cw->fmt) cw->fmt = XRenderFindStandardFormat(cm->dpy, PictStandardRGB24);
    if (!cw->fmt) return;
    cw->pixmap = XCompositeNameWindowPixmap(cm->dpy, cw->win);
    if (!cw->pixmap) return;
    XRenderPictureAttributes pa = { .subwindow_mode = IncludeInferiors };
    cw->picture = XRenderCreatePicture(cm->dpy, cw->pixmap, cw->fmt,
                                       CPSubwindowMode, &pa);
    if (!cw->damage)
        cw->damage = XDamageCreate(cm->dpy, cw->win, XDamageReportNonEmpty);
    cm->dirty = true;
}

static _cwin_t *_cw_ensure(struct vt_compositor_x11 *cm, Window w) {
    _cwin_t *cw = _cw_find(cm, w);
    if (cw) return cw;
    cw = vt_malloc0(sizeof(*cw));
    cw->win = w;
    cw->opacity = 0xffffffff;
    unsigned long op = 0xffffffff;
    if (vt_x11_get_cardinal_property(w, vt_x11_atoms()->net_wm_window_opacity, &op))
        cw->opacity = op;
    vt_vec_push(&cm->wins, &cw);
    _cw_sync_surface(cm, cw);
    return cw;
}

/* --------------------------------------------------------- back buffer */
static bool _ensure_back(struct vt_compositor_x11 *cm) {
    if (cm->back_pic && cm->back_w == cm->screen_w && cm->back_h == cm->screen_h)
        return true;
    if (cm->back_pic) XRenderFreePicture(cm->dpy, cm->back_pic);
    if (cm->back) XFreePixmap(cm->dpy, cm->back);
    cm->back = XCreatePixmap(cm->dpy, cm->root, (unsigned)cm->screen_w,
                             (unsigned)cm->screen_h, 24);
    if (!cm->back) return false;
    XRenderPictFormat *fmt =
        XRenderFindStandardFormat(cm->dpy, PictStandardRGB24);
    if (!fmt) return false;
    cm->back_pic = XRenderCreatePicture(cm->dpy, cm->back, fmt, 0, NULL);
    cm->back_w = cm->screen_w;
    cm->back_h = cm->screen_h;
    return cm->back_pic != 0;
}

static bool _ensure_alpha(struct vt_compositor_x11 *cm) {
    if (cm->alpha_1x1) return true;
    cm->alpha_px = XCreatePixmap(cm->dpy, cm->root, 1, 1, 8);
    XRenderPictFormat *fmt =
        XRenderFindStandardFormat(cm->dpy, PictStandardA8);
    if (!fmt) return false;
    XRenderPictureAttributes pa = { .repeat = True };
    cm->alpha_1x1 = XRenderCreatePicture(cm->dpy, cm->alpha_px, fmt,
                                         CPRepeat, &pa);
    return cm->alpha_1x1 != 0;
}

static void _set_alpha(struct vt_compositor_x11 *cm, unsigned char a) {
    _ensure_alpha(cm);
    if (!cm->alpha_1x1) return;
    XRenderColor c = { .red = 0, .green = 0, .blue = 0, .alpha = a };
    XRenderFillRectangle(cm->dpy, PictOpSrc, cm->alpha_1x1, &c, 0, 0, 1, 1);
}

/* ------------------------------------------------------------ painting */
static void _paint_window(struct vt_compositor_x11 *cm, _cwin_t *cw) {
    if (!cw->picture || !cw->viewable || cw->w <= 0 || cw->h <= 0) return;
    Picture mask = None;
    if (cm->comp->cfg.enable_transparency && cw->opacity < 0xffffffff) {
        unsigned char a = (unsigned char)(cw->opacity >> 8);
        if (a == 0) return;
        _set_alpha(cm, a);
        mask = cm->alpha_1x1;
    }
    XRenderComposite(cm->dpy, PictOpOver, cw->picture, mask, cm->back_pic,
                     0, 0, 0, 0, cw->x, cw->y, (unsigned)cw->w, (unsigned)cw->h);
}

static void _paint_shadow(struct vt_compositor_x11 *cm, _cwin_t *cw) {
    if (!cw->picture || !cw->viewable || cw->w <= 0 || cw->h <= 0) return;
    int r = cm->comp->cfg.shadow_radius;
    if (r <= 0) return;
    /* soft-ish shadow: layered translucent black silhouettes behind the
     * window. Cheap (three fills), default OFF — zero cost when disabled. */
    int off = r / 3;
    XRenderColor c1 = { .red = 0, .green = 0, .blue = 0, .alpha = 0x2800 };
    XRenderColor c2 = { .red = 0, .green = 0, .blue = 0, .alpha = 0x3800 };
    XRenderColor c3 = { .red = 0, .green = 0, .blue = 0, .alpha = 0x5000 };
    XRenderFillRectangle(cm->dpy, PictOpOver, cm->back_pic, &c1,
                         (short)(cw->x - r), (short)(cw->y - r + off),
                         (unsigned)(cw->w + 2 * r), (unsigned)(cw->h + 2 * r - off));
    XRenderFillRectangle(cm->dpy, PictOpOver, cm->back_pic, &c2,
                         (short)(cw->x - r / 2), (short)(cw->y - r / 2 + off),
                         (unsigned)(cw->w + r), (unsigned)(cw->h + r - off));
    XRenderFillRectangle(cm->dpy, PictOpOver, cm->back_pic, &c3,
                         (short)(cw->x - r / 4), (short)(cw->y - r / 4 + off),
                         (unsigned)(cw->w + r / 2), (unsigned)(cw->h + r / 2 - off));
}

static void _repaint(struct vt_compositor_x11 *cm) {
    if (!_ensure_back(cm)) return;

    Window r, parent, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(cm->dpy, cm->root, &r, &parent, &children, &n)) return;

    /* children come back bottom-to-top in stacking order */
    _cwin_t **order = vt_malloc(sizeof(_cwin_t *) * (n > 0 ? n : 1));
    size_t no = 0;
    for (unsigned i = 0; i < n; i++) {
        _cwin_t *cw = _cw_ensure(cm, children[i]);
        if (cw) order[no++] = cw;
    }
    if (children) XFree(children);

    /* clear background */
    XRenderColor bg = { .red = 0x1800, .green = 0x1800, .blue = 0x1c00,
                        .alpha = 0xffff };
    XRenderFillRectangle(cm->dpy, PictOpSrc, cm->back_pic, &bg, 0, 0,
                         (unsigned)cm->screen_w, (unsigned)cm->screen_h);

    /* fullscreen fast-path: top-most viewable window covering the screen
     * with full opacity skips everything below it */
    size_t start = 0;
    for (size_t i = no; i > 0; i--) {
        _cwin_t *cw = order[i - 1];
        if (!cw->viewable) continue;
        if (cw->x <= 0 && cw->y <= 0 && cw->w >= cm->screen_w &&
            cw->h >= cm->screen_h && cw->opacity >= 0xffffffff) {
            start = i - 1;
            break;
        }
    }

    vt_logd("compositor-x11: repaint %zu windows (start=%zu) frame=%u",
            no, start, cm->comp->frames_drawn);
    for (size_t i = start; i < no; i++) {
        _cwin_t *cw = order[i];
        vt_logd("  paint 0x%lx %dx%d+%d+%d viewable=%d pic=%lu",
                (unsigned long)cw->win, cw->w, cw->h, cw->x, cw->y,
                cw->viewable, (unsigned long)cw->picture);
        if (!cw->viewable) continue;
        if (cm->comp->cfg.enable_shadows) _paint_shadow(cm, cw);
        _paint_window(cm, cw);
        cw->damaged = false;
        if (cw->damage) XDamageSubtract(cm->dpy, cw->damage, None, None);
    }
    vt_free(order);

    /* present: back buffer → root */
    if (!cm->root_pic) {
        XRenderPictFormat *fmt = XRenderFindVisualFormat(
            cm->dpy, DefaultVisual(cm->dpy, DefaultScreen(cm->dpy)));
        if (fmt) cm->root_pic = XRenderCreatePicture(cm->dpy, cm->root, fmt,
                                                     0, NULL);
    }
    if (cm->root_pic)
        XRenderComposite(cm->dpy, PictOpSrc, cm->back_pic, None, cm->root_pic,
                         0, 0, 0, 0, 0, 0, (unsigned)cm->screen_w,
                         (unsigned)cm->screen_h);
    XFlush(cm->dpy);
    cm->comp->frames_drawn++;
}

static void _maybe_paint(struct vt_compositor_x11 *cm) {
    if (!cm->dirty) return;
    uint64_t now = vt_time_now_us();
    if (cm->comp->cfg.enable_vsync && cm->last_paint_us &&
        now - cm->last_paint_us < (uint64_t)cm->frame_us)
        return; /* paced — the main loop will call again */
    cm->dirty = false;
    cm->last_paint_us = now;
    _repaint(cm);
}

/* --------------------------------------------------------- event sink */
static void _on_event(void *ud, void *event) {
    struct vt_compositor_x11 *cm = ud;
    XEvent *ev = event;
    vt_logd("compositor-x11: event type=%d win=0x%lx", ev->type,
            (unsigned long)ev->xany.window);
    switch (ev->type) {
    case MapNotify: {
        /* _cw_ensure: a newly mapped window must be tracked even when it
         * was never seen before (it is normally discovered during paint
         * via XQueryTree, but the damage/map event is what triggers the
         * repaint in the first place). */
        _cw_ensure(cm, ev->xmap.window);
        cm->dirty = true;
        break;
    }
    case UnmapNotify: {
        _cwin_t *cw = _cw_find(cm, ev->xunmap.window);
        if (cw) {
            cw->viewable = false;
            /* the backing pixmap is invalidated on unmap */
            if (cw->picture) { XRenderFreePicture(cm->dpy, cw->picture); cw->picture = 0; }
            if (cw->pixmap) { XFreePixmap(cm->dpy, cw->pixmap); cw->pixmap = 0; }
            cm->dirty = true;
        }
        break;
    }
    case DestroyNotify:
        _cw_remove(cm, ev->xdestroywindow.window);
        break;
    case ConfigureNotify: {
        _cwin_t *cw = _cw_ensure(cm, ev->xconfigure.window);
        if (cw) {
            if (ev->xconfigure.width != cw->w || ev->xconfigure.height != cw->h ||
                ev->xconfigure.x != cw->x || ev->xconfigure.y != cw->y) {
                if (cw->picture) {
                    XRenderFreePicture(cm->dpy, cw->picture);
                    cw->picture = 0;
                }
                if (cw->pixmap) {
                    XFreePixmap(cm->dpy, cw->pixmap);
                    cw->pixmap = 0;
                }
            }
            _cw_sync_surface(cm, cw);
            cm->dirty = true;
        }
        break;
    }
    case CirculateNotify:
    case ReparentNotify:
        cm->dirty = true;
        break;
    case PropertyNotify: {
        const vt_x11_atoms_t *a = vt_x11_atoms();
        if (ev->xproperty.atom == a->net_wm_window_opacity) {
            _cwin_t *cw = _cw_find(cm, ev->xproperty.window);
            if (cw) {
                unsigned long op = 0xffffffff;
                if (vt_x11_get_cardinal_property(cw->win,
                        a->net_wm_window_opacity, &op))
                    cw->opacity = op;
                cm->dirty = true;
            }
        }
        break;
    }
    default:
        if (cm->damage_base && ev->type == cm->damage_base + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)ev;
            _cwin_t *cw = _cw_find(cm, de->drawable);
            if (cw) { cw->damaged = true; cm->dirty = true; }
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------- engine */
bool vt_compositor_x11_available(void) {
    Display *dpy = vt_x11_display();
    if (!dpy) return false;
    int ev = 0, er = 0, maj = 0, min = 0;
    if (!XCompositeQueryExtension(dpy, &ev, &er)) return false;
    if (!XCompositeQueryVersion(dpy, &maj, &min)) return false;
    if (maj == 0 && min < 3) return false;
    if (!XDamageQueryExtension(dpy, &ev, &er)) return false;
    if (!XRenderQueryExtension(dpy, &ev, &er)) return false;
    return true;
}

int vt_compositor_attach_x11(vt_compositor_t *c, struct vt_wm *wm) {
    if (!c || _cm) return _cm ? VT_OK : VT_ERR_INVAL;
    Display *dpy = vt_x11_display();
    if (!dpy) return VT_ERR_INVAL;
    if (!vt_compositor_x11_available()) {
        vt_logw("compositor-x11: required X extensions missing "
                "(Composite>=0.3, Damage, Render); compositing disabled");
        return VT_ERR_NOTSUPP;
    }
    struct vt_compositor_x11 *cm = vt_malloc0(sizeof(*cm));
    cm->comp = c;
    cm->wm = wm;
    cm->dpy = dpy;
    cm->root = vt_x11_root();
    cm->screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    cm->screen_h = DisplayHeight(dpy, DefaultScreen(dpy));
    vt_vec_init(&cm->wins, sizeof(_cwin_t *), 16);
    cm->frame_us = 16666; /* ~60 Hz pacing */

    /* take composite manager ownership */
    cm->owner = XCreateSimpleWindow(dpy, cm->root, -10, -10, 1, 1, 0, 0, 0);
    XSetWindowAttributes sa = { .override_redirect = True };
    XChangeWindowAttributes(dpy, cm->owner, CWOverrideRedirect, &sa);
    XSetSelectionOwner(dpy, vt_x11_atoms()->net_wm_cm, cm->owner, CurrentTime);

    XCompositeRedirectSubwindows(dpy, cm->root, CompositeRedirectManual);
    cm->redirected = true;

    int ev = 0, er = 0;
    XDamageQueryExtension(dpy, &ev, &er);
    cm->damage_base = ev;

    if (c->backend)
        cm->sink_id = vt_backend_add_event_sink(c->backend, _on_event, cm);

    /* adopt existing windows */
    Window r, parent, *children = NULL;
    unsigned int n = 0;
    if (XQueryTree(dpy, cm->root, &r, &parent, &children, &n)) {
        for (unsigned i = 0; i < n; i++) _cw_ensure(cm, children[i]);
        if (children) XFree(children);
    }
    cm->dirty = true;
    _cm = cm;
    c->priv = cm;
    vt_logi("compositor-x11: active (%dx%d, damage base %d)",
            cm->screen_w, cm->screen_h, cm->damage_base);
    return VT_OK;
}

bool vt_compositor_x11_active(const vt_compositor_t *c) {
    return c && c->priv == _cm && _cm != NULL;
}

void vt_compositor_x11_stop(void) {
    struct vt_compositor_x11 *cm = _cm;
    if (!cm) return;
    if (cm->redirected)
        XCompositeUnredirectSubwindows(cm->dpy, cm->root,
                                       CompositeRedirectManual);
    if (cm->sink_id > 0 && cm->comp->backend)
        vt_backend_remove_event_sink(cm->comp->backend, cm->sink_id);
    for (size_t i = 0; i < cm->wins.size; i++) {
        _cwin_t *cw = *(_cwin_t **)vt_vec_at(&cm->wins, i);
        _cw_free(cm, cw);
    }
    vt_vec_fini(&cm->wins);
    if (cm->back_pic) XRenderFreePicture(cm->dpy, cm->back_pic);
    if (cm->back) XFreePixmap(cm->dpy, cm->back);
    if (cm->alpha_1x1) XRenderFreePicture(cm->dpy, cm->alpha_1x1);
    if (cm->alpha_px) XFreePixmap(cm->dpy, cm->alpha_px);
    if (cm->root_pic) XRenderFreePicture(cm->dpy, cm->root_pic);
    if (cm->owner) XDestroyWindow(cm->dpy, cm->owner);
    if (cm->comp) cm->comp->priv = NULL;
    vt_free(cm);
    _cm = NULL;
    vt_logi("compositor-x11: stopped");
}

void vt_compositor_x11_step(void) {
    if (_cm) _maybe_paint(_cm);
}

void vt_compositor_x11_screen_resized(void) {
    if (!_cm) return;
    _cm->screen_w = DisplayWidth(_cm->dpy, DefaultScreen(_cm->dpy));
    _cm->screen_h = DisplayHeight(_cm->dpy, DefaultScreen(_cm->dpy));
    _cm->dirty = true;
}

#else /* extensions not compiled in */

int vt_compositor_attach_x11(vt_compositor_t *c, struct vt_wm *wm) {
    (void)c; (void)wm;
    vt_logw("compositor-x11: built without Composite/Damage/Render support");
    return VT_ERR_NOTSUPP;
}
bool vt_compositor_x11_active(const vt_compositor_t *c) { (void)c; return false; }
bool vt_compositor_x11_available(void) { return false; }
void vt_compositor_x11_stop(void) {}
void vt_compositor_x11_step(void) {}
void vt_compositor_x11_screen_resized(void) {}

#endif
