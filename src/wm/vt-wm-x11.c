/*
 * vt-wm-x11.c — X11 window manager engine (EWMH/ICCCM)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * The full window-management engine for the Xorg/XLibre backends:
 *   - client lifecycle (manage / withdraw / destroy)
 *   - EWMH root properties and client state atoms
 *   - focus (click-to-focus by default, sloppy optional)
 *   - stacking layers, raise/restack
 *   - workspaces, sticky windows, taskbar-visible client list
 *   - maximize / fullscreen / minimize with proper state transitions
 *   - edge-snap tiling on interactive move
 *   - keyboard hotkeys via XGrabKey, alt-drag move/resize via XGrabButton
 *   - workarea computation honoring panel struts
 *   - server-side decorations: a reparenting frame with title bar,
 *     close/maximize/minimize buttons, borders and edge resize grips
 *
 * The frame is a plain InputOutput window the WM owns; the client is
 * reparented into it at a fixed inset. _NET_FRAME_EXTENTS reports the
 * real inset so EWMH-aware panels/taskbars size correctly.
 */

#define VT_LOG_DOMAIN "wm-x11"
#include <vantage/vt-wm.h>
#include <vantage/vt-x11.h>
#include <vantage/vt-backend.h>

#if defined(VT_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#if defined(VT_HAVE_XCURSOR)
#include <X11/Xcursor/Xcursor.h>
#endif

#if defined(VT_HAVE_XFT)
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#endif

/* ------------------------------------------------------------- client */
typedef struct _client {
    vt_window_t      model;        /* embedded public model; id == Window */
    Window           win;
    Window           frame;        /* decoration frame (None if unframed) */
    bool             framed;
    int              fr_title;     /* live title-bar height (0 fullscreen) */
    Window           transient_for;
    bool             has_size_hints;
    long             min_w, min_h, max_w, max_h, base_w, base_h, inc_w, inc_h;
    bool             input_hint;
    bool             take_focus, delete_window;
    bool             is_dock;
    bool             is_desktop;
    bool             expect_unmap;
    unsigned long    opacity;
    Time             last_title_click;   /* double-click maximize */
} _client_t;

typedef struct {
    int x, y, w, h;
} _rect_t;

typedef struct vt_wm_x11 {
    vt_wm_t         *wm;
    Display         *dpy;
    Window           root;
    Window           wmwin;           /* _NET_SUPPORTING_WM_CHECK window */
    vt_vec_t         clients;         /* _client_t* — MRU order (last = recent) */
    vt_vec_t         stacking;        /* _client_t* — top first */
    vt_vec_t         grabs;           /* _grab_t */
    int              n_desktops;
    int              cur_desktop;
    int              sink_id;
    bool             sloppy_focus;    /* default: click-to-focus */
    bool             snap_enabled;
    Cursor           cur_move, cur_resize;
    Cursor           cur_default;      /* themed root cursor */
    bool             cur_default_set;
#if defined(VT_HAVE_XFT)
    XftFont         *tfont;           /* frame title font */
#endif
    /* interactive move/resize */
    bool             in_op;
    int              op_mode;         /* 0=move 1=resize */
    int              op_edge;
    _client_t       *op_client;
    int              op_start_x, op_start_y;
    int              op_win_x, op_win_y, op_win_w, op_win_h;
    _rect_t          workarea;
} vt_wm_x11_t;

typedef struct {
    char           *combo;
    int              keycode;
    unsigned int     mods;
} _grab_t;

struct vt_wm_x11;

/* ------------------------------------------------------------ helpers */
static volatile sig_atomic_t _probe_bad_access = 0;

static int _probe_err(Display *d, XErrorEvent *ev) {
    (void)d;
    if (ev->error_code == BadAccess) _probe_bad_access = 1;
    return 0;
}

static int _x_err(Display *d, XErrorEvent *ev) {
    (void)d; (void)ev;
    return 0; /* tolerant: races with dying clients are normal for a WM */
}

static int _layer_of(const _client_t *c) {
    if (c->model.fullscreen) return VT_WM_LAYER_OVERLAY;
    if (c->is_desktop) return VT_WM_LAYER_DESKTOP;
    if (c->is_dock) return VT_WM_LAYER_PANEL;
    if (c->model.layer == VT_WM_LAYER_BELOW) return VT_WM_LAYER_BELOW;
    if (c->model.layer == VT_WM_LAYER_ABOVE) return VT_WM_LAYER_ABOVE;
    return VT_WM_LAYER_NORMAL;
}

static int _cmp_layer_asc(const void *a, const void *b) {
    const _client_t *ca = *(_client_t *const *)a;
    const _client_t *cb = *(_client_t *const *)b;
    return _layer_of(ca) - _layer_of(cb);
}

static void _client_free(_client_t *c) {
    if (!c) return;
    vt_free(c->model.title);
    vt_free(c->model.app_id);
    vt_free(c->model.class_str);
    vt_free(c);
}

static _client_t *_find(vt_wm_x11_t *e, Window w) {
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (c->win == w) return c;
    }
    return NULL;
}

static void _push_model(vt_wm_x11_t *e, _client_t *c) {
    for (size_t i = 0; i < e->wm->windows.size; i++) {
        vt_window_t **pp = vt_vec_at(&e->wm->windows, i);
        if (*pp == &c->model) return;
    }
    vt_window_t *p = &c->model;
    vt_vec_push(&e->wm->windows, &p);
}

static void _remove_model(vt_wm_x11_t *e, _client_t *c) {
    for (size_t i = 0; i < e->wm->windows.size; i++) {
        vt_window_t **pp = vt_vec_at(&e->wm->windows, i);
        if (*pp == &c->model) { vt_vec_remove(&e->wm->windows, i); return; }
    }
}

static void _emit_win(vt_wm_x11_t *e, _client_t *c, vt_wm_event_t ev) {
    if (e->wm->on_window_event) e->wm->on_window_event(e->wm, &c->model, ev);
}

static void _emit_desktop(vt_wm_x11_t *e) {
    if (e->wm->on_desktop_changed) e->wm->on_desktop_changed(e->wm, e->cur_desktop);
}

/* -------------------------------------------------- property accessors */
static char *_get_title(_client_t *c, Display *dpy) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    char *t = vt_x11_get_utf8_property(c->win, a->net_wm_name);
    if (t && *t) return t;
    vt_free(t);
    XTextProperty tp;
    if (XGetTextProperty(dpy, c->win, &tp, XA_WM_NAME) && tp.value) {
        char **list = NULL; int n = 0;
        if (XmbTextPropertyToTextList(dpy, &tp, &list, &n) >= Success && n > 0
            && list && list[0]) {
            char *out = vt_strdup(list[0]);
            XFreeStringList(list);
            XFree(tp.value);
            return out;
        }
        if (list) XFreeStringList(list);
        char *out = vt_strndup((const char *)tp.value, tp.nitems);
        XFree(tp.value);
        return out;
    }
    return vt_strprintf("Window 0x%lx", (unsigned long)c->win);
}

static void _read_class(_client_t *c, Display *dpy) {
    XClassHint ch = { NULL, NULL };
    if (XGetClassHint(dpy, c->win, &ch)) {
        vt_free(c->model.class_str);
        vt_free(c->model.app_id);
        c->model.class_str = vt_strdup(ch.res_class ? ch.res_class : "");
        c->model.app_id = vt_strdup(ch.res_name ? ch.res_name : "");
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }
}

static void _read_size_hints(_client_t *c, Display *dpy) {
    XSizeHints sh;
    long supplied = 0;
    if (!XGetWMNormalHints(dpy, c->win, &sh, &supplied)) return;
    c->has_size_hints = true;
    if (sh.flags & PMinSize) { c->min_w = sh.min_width; c->min_h = sh.min_height; }
    if (sh.flags & PMaxSize) { c->max_w = sh.max_width; c->max_h = sh.max_height; }
    if (sh.flags & PBaseSize) { c->base_w = sh.base_width; c->base_h = sh.base_height; }
    if (sh.flags & PResizeInc) { c->inc_w = sh.width_inc; c->inc_h = sh.height_inc; }
}

static void _read_protocols(_client_t *c, Display *dpy) {
    Atom *protos = NULL; int n = 0;
    const vt_x11_atoms_t *a = vt_x11_atoms();
    if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
        for (int i = 0; i < n; i++) {
            if (protos[i] == a->wm_delete_window) c->delete_window = true;
            if (protos[i] == a->wm_take_focus) c->take_focus = true;
        }
        if (protos) XFree(protos);
    }
}

static void _set_frame_extents(vt_wm_x11_t *e, _client_t *c);
static void _frame_paint(vt_wm_x11_t *e, _client_t *c);
static void _frame_create(vt_wm_x11_t *e, _client_t *c);
static void _frame_destroy(vt_wm_x11_t *e, _client_t *c, bool destroyed);
static bool _framed(const _client_t *c);

/* --------------------------------------------------- _MOTIF_WM_HINTS */
/* mwm.h field semantics (de facto standard): hints[0]=flags,
 * hints[2]=decorations. MWM_HINTS_DECORATIONS=1<<2,
 * MWM_DECOR_ALL=1, MWM_DECOR_TITLE=1<<3 (2), MWM_DECOR_BORDER=1<<4 (4).
 *
 * GTK/Chromium/Firefox windows that draw their own headerbars set
 * decorations=0 here. A WM that ignores this DOUBLE-DECORATES those
 * windows — its own titlebar stacked on top of the app's. */
#define _MWM_HINTS_DECORATIONS (1L << 2)
#define _MWM_DECOR_ALL         (1L << 0)
#define _MWM_DECOR_TITLE       (1L << 3)
#define _MWM_DECOR_BORDER      (1L << 4)

/* returns true when the client asks for NO server-side decorations */
static bool _motif_undecorated(_client_t *c, Display *dpy) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    unsigned char *data = NULL;
    unsigned long n = 0;
    bool undecorated = false;
    if (vt_x11_get_window_property(c->win, a->motif_wm_hints, a->cardinal,
                                   &data, &n) && data && n >= 3) {
        unsigned long *h = (unsigned long *)(void *)data;
        if (h[0] & _MWM_HINTS_DECORATIONS) {
            unsigned long decor = h[2];
            if (decor & _MWM_DECOR_ALL) {
                undecorated = false;
            } else {
                /* no title AND no border → the window draws everything */
                if (!(decor & (_MWM_DECOR_TITLE | _MWM_DECOR_BORDER)))
                    undecorated = true;
                /* partial decorations (e.g. border only): still frame it —
                 * a minimal honest frame beats a floating borderless box */
            }
        }
        XFree(data);
    }
    return undecorated;
}

/* (re-)apply the motif decoration request: creates or removes the SSD
 * frame. Apps toggle this at runtime (browser CSD on/off). */
static void _apply_motif(vt_wm_x11_t *e, _client_t *c) {
    if (c->is_dock || c->is_desktop || c->model.fullscreen) return;
    bool want = !_motif_undecorated(c, e->dpy);
    if (want && !c->framed) {
        _frame_create(e, c);
        if (_framed(c)) {
            XMapWindow(e->dpy, c->frame);
            _frame_paint(e, c);
        }
        vt_logi("wm: 0x%lx re-framed (MOTIF decorations requested)",
                (unsigned long)c->win);
    } else if (!want && c->framed) {
        _frame_destroy(e, c, false);
        vt_logi("wm: 0x%lx decorations removed (client-side decorations)",
                (unsigned long)c->win);
    }
    _set_frame_extents(e, c);
    _emit_win(e, c, VT_WM_EVENT_STATE);
}

static void _read_type_and_state(_client_t *c) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    unsigned char *data = NULL;
    unsigned long n = 0;
    c->is_dock = c->is_desktop = false;
    if (vt_x11_get_window_property(c->win, a->net_wm_window_type, a->atom_atom,
                                   &data, &n)) {
        Atom *types = (Atom *)(void *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (types[i] == a->net_wm_window_type_dock) c->is_dock = true;
            else if (types[i] == a->net_wm_window_type_desktop) c->is_desktop = true;
        }
        XFree(data);
    }
    c->model.fullscreen = vt_x11_has_state(c->win, a->net_wm_state_fullscreen);
    c->model.sticky = vt_x11_has_state(c->win, a->net_wm_state_sticky);
    c->model.layer = vt_x11_has_state(c->win, a->net_wm_state_above) ? VT_WM_LAYER_ABOVE
                 : vt_x11_has_state(c->win, a->net_wm_state_below) ? VT_WM_LAYER_BELOW
                 : VT_WM_LAYER_NORMAL;
    unsigned long op = 0xffffffff;
    if (vt_x11_get_cardinal_property(c->win, a->net_wm_window_opacity, &op))
        c->opacity = op;
    else c->opacity = 0xffffffff;
}

static void _read_wm_hints(_client_t *c, Display *dpy) {
    XWMHints *h = XGetWMHints(dpy, c->win);
    if (h) {
        if (h->flags & InputHint) c->input_hint = h->input;
        if (h->flags & XUrgencyHint) c->model.urgent = true;
        XFree(h);
    }
}

static void _read_transient(_client_t *c, Display *dpy) {
    Window t = None;
    if (XGetTransientForHint(dpy, c->win, &t)) c->transient_for = t;
}

/* ------------------------------------------------------- EWMH updates */
static void _set_cardinal_list(Window win, Atom prop, const unsigned long *vals,
                               int n) {
    XChangeProperty(vt_x11_display(), win, prop, vt_x11_atoms()->cardinal, 32,
                    PropModeReplace, (const unsigned char *)vals, n);
}

static void _update_client_list(vt_wm_x11_t *e) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    unsigned long ids[256];
    size_t n = 0;
    for (size_t i = 0; i < e->clients.size && n < 256; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        ids[n++] = c->win;
    }
    _set_cardinal_list(e->root, a->net_client_list, ids, (int)n);
    n = 0;
    for (size_t i = 0; i < e->stacking.size && n < 256; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->stacking, i);
        ids[n++] = c->win;
    }
    _set_cardinal_list(e->root, a->net_client_list_stacking, ids, (int)n);
}

static void _update_desktop_props(vt_wm_x11_t *e) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    unsigned long nd = (unsigned long)e->n_desktops;
    _set_cardinal_list(e->root, a->net_number_of_desktops, &nd, 1);
    unsigned long cur = (unsigned long)e->cur_desktop;
    _set_cardinal_list(e->root, a->net_current_desktop, &cur, 1);
    unsigned long vp[2] = { 0, 0 };
    _set_cardinal_list(e->root, a->net_desktop_viewport, vp, 2);
    unsigned long wa[4] = { (unsigned long)e->workarea.x, (unsigned long)e->workarea.y,
                            (unsigned long)e->workarea.w, (unsigned long)e->workarea.h };
    _set_cardinal_list(e->root, a->net_workarea, wa, 4);
    XTextProperty tp;
    char **names = vt_malloc(sizeof(char *) * (size_t)(e->n_desktops > 0 ? e->n_desktops : 1));
    for (int i = 0; i < e->n_desktops; i++)
        names[i] = (i < (int)e->wm->workspaces.size)
            ? *(char **)vt_vec_at(&e->wm->workspaces, i)
            : (char *)"Workspace";
    if (Xutf8TextListToTextProperty(e->dpy, names, e->n_desktops,
                                    XUTF8StringStyle, &tp) == Success) {
        XSetTextProperty(e->dpy, e->root, &tp, a->net_desktop_names);
        XFree(tp.value);
    }
    vt_free(names);
}

static void _set_wm_state(vt_wm_x11_t *e, _client_t *c, int state) {
    unsigned long data[2] = { (unsigned long)state, None };
    XChangeProperty(e->dpy, c->win, vt_x11_atoms()->wm_state,
                    vt_x11_atoms()->cardinal, 32, PropModeReplace,
                    (const unsigned char *)data, 2);
}

static void _set_net_wm_desktop(vt_wm_x11_t *e, _client_t *c) {
    unsigned long v = c->model.sticky ? 0xfffffffful
                                      : (unsigned long)c->model.workspace;
    _set_cardinal_list(c->win, vt_x11_atoms()->net_wm_desktop, &v, 1);
}

static void _set_allowed_actions(vt_wm_x11_t *e, _client_t *c) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    Atom acts[9];
    int n = 0;
    acts[n++] = a->net_wm_action_move;
    acts[n++] = a->net_wm_action_resize;
    if (!c->is_dock && !c->is_desktop) {
        acts[n++] = a->net_wm_action_minimize;
        acts[n++] = a->net_wm_action_maximize_horz;
        acts[n++] = a->net_wm_action_maximize_vert;
        acts[n++] = a->net_wm_action_fullscreen;
        acts[n++] = a->net_wm_action_above;
        acts[n++] = a->net_wm_action_below;
    }
    acts[n++] = a->net_wm_action_close;
    XChangeProperty(e->dpy, c->win, a->net_wm_allowed_actions, a->atom_atom, 32,
                    PropModeReplace, (const unsigned char *)acts, n);
}

static void _set_state_atoms(vt_wm_x11_t *e, _client_t *c) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    Atom list[16];
    int n = 0;
    if (c->model.maximized) {
        list[n++] = a->net_wm_state_maximized_vert;
        list[n++] = a->net_wm_state_maximized_horz;
    }
    if (c->model.fullscreen) list[n++] = a->net_wm_state_fullscreen;
    if (c->model.sticky) list[n++] = a->net_wm_state_sticky;
    if (c->model.layer == VT_WM_LAYER_ABOVE) list[n++] = a->net_wm_state_above;
    if (c->model.layer == VT_WM_LAYER_BELOW) list[n++] = a->net_wm_state_below;
    if (c->model.minimized) list[n++] = a->net_wm_state_hidden;
    if (c->is_dock || c->is_desktop) list[n++] = a->net_wm_state_skip_taskbar;
    if (n == 0) XDeleteProperty(e->dpy, c->win, a->net_wm_state);
    else XChangeProperty(e->dpy, c->win, a->net_wm_state, a->atom_atom, 32,
                         PropModeReplace, (const unsigned char *)list, n);
}

static void _send_configure(vt_wm_x11_t *e, _client_t *c);
static void _close(vt_wm_x11_t *e, _client_t *c);
static void _maximize(vt_wm_x11_t *e, _client_t *c, bool on);
static void _minimize(vt_wm_x11_t *e, _client_t *c, bool on);
static void _focus(vt_wm_x11_t *e, _client_t *c);
static void _raise(vt_wm_x11_t *e, _client_t *c);
static void _op_start(vt_wm_x11_t *e, _client_t *c, int mode, int edge,
                      int px, int py);
/* ------------------------------------------------------ decorations */
#define _FR_BORDER 2
#define _FR_TITLE  26
#define _FR_BTN_W  26
#define _FR_BTN_GAP 2

static void _set_frame_extents(vt_wm_x11_t *e, _client_t *c) {
    unsigned long fe[4];
    if (c->framed) {
        fe[0] = (unsigned long)_FR_BORDER;                 /* left   */
        fe[1] = (unsigned long)_FR_BORDER;                 /* right  */
        fe[2] = (unsigned long)(c->fr_title + _FR_BORDER);  /* top    */
        fe[3] = (unsigned long)_FR_BORDER;                 /* bottom */
    } else {
        memset(fe, 0, sizeof(fe));
    }
    _set_cardinal_list(c->win, vt_x11_atoms()->net_frame_extents, fe, 4);
}

static bool _framed(const _client_t *c) {
    return c && c->framed && c->frame != None;
}

static Window _fr_win(const _client_t *c) {
    return _framed(c) ? c->frame : c->win;
}

/* frame geometry derived from the (client-space) model geometry */
static void _frame_geom(const _client_t *c, int *fx, int *fy, int *fw,
                        int *fh) {
    *fx = c->model.x - _FR_BORDER;
    *fy = c->model.y - (c->fr_title + _FR_BORDER);
    *fw = c->model.w + 2 * _FR_BORDER;
    *fh = c->model.h + c->fr_title + 2 * _FR_BORDER;
}

#if defined(VT_HAVE_XFT)
/* ---- per-codepoint title font fallback ---------------------------
 * Same idea as the panel's: when the title face lacks a glyph (Russian
 * window titles are the common case), lazily open a fontconfig match
 * that covers the codepoint and draw that run with it. */
static int _wm_utf8_cp(const char *s, FcChar32 *out) {
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

static XftFont *_wm_font_for_cp(vt_wm_x11_t *e, FcChar32 cp) {
    if (XftCharExists(e->dpy, e->tfont, cp)) return e->tfont;
    /* small fixed cache of fallback faces */
    static XftFont *fb[4];
    static FcChar32 fb_cp[4];
    static int n_fb;
    for (int i = 0; i < n_fb; i++)
        if (fb_cp[i] == cp) return fb[i] ? fb[i] : e->tfont;
    FcPattern *pat = FcNameParse((const FcChar8 *)"sans:bold");
    if (!pat) return e->tfont;
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcPattern *mat = FcFontMatch(NULL, pat, &res);
    XftFont *f = NULL;
    if (mat) {
        f = XftFontOpenPattern(e->dpy, mat);
        if (f && !XftCharExists(e->dpy, f, cp)) {
            XftFontClose(e->dpy, f);
            f = NULL;
        } else if (!f) {
            FcPatternDestroy(mat);
        }
    }
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    if (n_fb < 4) {
        fb[n_fb] = f;
        fb_cp[n_fb] = cp;
        n_fb++;
        return f ? f : e->tfont;
    }
    /* cache full: do not leak a per-call face */
    if (f) XftFontClose(e->dpy, f);
    return e->tfont;
}

static void _fill(Display *dpy, XRenderPictFormat *fmt, Drawable d, int x,
                  int y, int w, int h, unsigned long argb) {
    Picture pic = XRenderCreatePicture(dpy, d, fmt, 0, NULL);
    XRenderColor c = {
        .red   = (unsigned short)(((argb >> 16) & 0xff) * 0x101),
        .green = (unsigned short)(((argb >> 8)  & 0xff) * 0x101),
        .blue  = (unsigned short)(((argb)       & 0xff) * 0x101),
        .alpha = (unsigned short)(((argb >> 24) & 0xff) * 0x101),
    };
    XRenderFillRectangle(dpy, PictOpSrc, pic, &c, (short)x, (short)y,
                          (unsigned)w, (unsigned)h);
    XRenderFreePicture(dpy, pic);
}
#endif

/* Paint the frame: border, title bar, title text, window buttons.
 * Colors follow the Vantage dark palette; the active window gets the
 * accent border and bright text, inactive ones dim down. */
static void _frame_paint(vt_wm_x11_t *e, _client_t *c) {
    if (!_framed(c)) return;
    Display *dpy = e->dpy;
    Window fw = c->frame;
    int fx, fy, fw_, fh_;
    _frame_geom(c, &fx, &fy, &fw_, &fh_);
    if (c->fr_title <= 0) {
        /* fullscreen: solid edge, no chrome */
#if defined(VT_HAVE_XFT)
        XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy,
            DefaultVisual(dpy, DefaultScreen(dpy)));
        _fill(dpy, fmt, fw, 0, 0, fw_, fh_, 0xff000000);
#endif
        return;
    }

#if defined(VT_HAVE_XFT)
    XRenderPictFormat *fmt = XRenderFindVisualFormat(
        dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    bool active = c->model.focused;
    unsigned long border = active ? 0xff4f9adc : 0xff26282e;
    unsigned long bar    = active ? 0xff2b2f36 : 0xff1a1c22;
    unsigned long fg     = active ? 0xffeceef0 : 0xff909399;

    int tw = fw_, th = c->fr_title + _FR_BORDER;
    _fill(dpy, fmt, fw, 0, 0, tw, th, bar);                       /* title bar  */
    _fill(dpy, fmt, fw, 0, th, _FR_BORDER, fh_ - th, border);     /* left edge  */
    _fill(dpy, fmt, fw, tw - _FR_BORDER, th, _FR_BORDER, fh_ - th, border);
    _fill(dpy, fmt, fw, 0, fh_ - _FR_BORDER, tw, _FR_BORDER, border);
    _fill(dpy, fmt, fw, _FR_BORDER, th, tw - 2 * _FR_BORDER, fh_ - th - _FR_BORDER,
          0xff000000);   /* behind the client (occluded, keeps X happy) */

    /* buttons: [min][max][close] at the right end of the title bar */
    int by = _FR_BORDER;
    int btn_y = by + (c->fr_title - 8) / 2;     /* glyph vertical center */
    int bx = tw - _FR_BORDER - _FR_BTN_W;
    struct { char kind; unsigned long col; } btns[3] = {
        { 'x', 0xffe05a5a },   /* close    */
        { 'm', 0xff4f9adc },   /* maximize */
        { 'n', 0xff7ac860 },   /* minimize */
    };
    for (int i = 0; i < 3; i++) {
        int x0 = bx - i * (_FR_BTN_W + _FR_BTN_GAP);
        /* subtle button well */
        _fill(dpy, fmt, fw, x0 + 3, by + 3, _FR_BTN_W - 6, c->fr_title - 6,
              (bar & 0xffffff00u) | 0x33);
        unsigned long g = btns[i].col;
        switch (btns[i].kind) {
        case 'x':                     /* × */
            _fill(dpy, fmt, fw, x0 + 10, btn_y,     2, 2, g);
            _fill(dpy, fmt, fw, x0 + 14, btn_y,     2, 2, g);
            _fill(dpy, fmt, fw, x0 + 12, btn_y + 2, 2, 2, g);
            _fill(dpy, fmt, fw, x0 + 10, btn_y + 4, 2, 2, g);
            _fill(dpy, fmt, fw, x0 + 14, btn_y + 4, 2, 2, g);
            break;
        case 'm':                     /* ▢ */
            _fill(dpy, fmt, fw, x0 + 10, btn_y, 6, 6, g);
            _fill(dpy, fmt, fw, x0 + 11, btn_y + 1, 4, 4, bar | 0xff000000u);
            break;
        case 'n':                     /* – */
        default:
            _fill(dpy, fmt, fw, x0 + 10, btn_y + 3, 6, 2, g);
            break;
        }
    }

    /* title text */
    const char *title = c->model.title ? c->model.title : "";
    if (*title && e->tfont) {
        XftDraw *d = XftDrawCreate(dpy, fw,
                                   DefaultVisual(dpy, DefaultScreen(dpy)),
                                   DefaultColormap(dpy, DefaultScreen(dpy)));
        XftColor c8;
        XRenderColor rc = {
            .red   = (unsigned short)(((fg >> 16) & 0xff) * 0x101),
            .green = (unsigned short)(((fg >> 8)  & 0xff) * 0x101),
            .blue  = (unsigned short)(((fg)       & 0xff) * 0x101),
            .alpha = 0xffff,
        };
        XftColorAllocValue(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                           DefaultColormap(dpy, DefaultScreen(dpy)), &rc, &c8);
        int max_w = bx - 3 * (_FR_BTN_W + _FR_BTN_GAP) - 10 - 8;
        XGlyphInfo gi;
        XftTextExtentsUtf8(dpy, e->tfont, (const FcChar8 *)title,
                           (int)strlen(title), &gi);
        char shown[128];
        snprintf(shown, sizeof(shown), "%s", title);
        if (gi.xOff > max_w && max_w > 16) {
            for (;;) {
                size_t l = strlen(shown);
                if (l < 2 || gi.xOff <= max_w) break;
                shown[l - 1] = 0;
                snprintf(shown + strlen(shown),
                         sizeof(shown) - strlen(shown), "%s",
                         "\xe2\x80\xa6");   /* ellipsis */
                XftTextExtentsUtf8(dpy, e->tfont, (const FcChar8 *)shown,
                                   (int)strlen(shown), &gi);
            }
        }
        
        /* per-codepoint font fallback so titles in any script render
         * (a single sans face lacks Cyrillic/CJK on many systems) */
        {
            int tx = 8;
            int ty = _FR_BORDER + (c->fr_title + _FR_BORDER + 8) / 2 -
                     e->tfont->height / 2 + e->tfont->ascent - 2;
            const char *pp = shown;
            while (*pp) {
                FcChar32 cp;
                int n = _wm_utf8_cp(pp, &cp);
                XftFont *rf = _wm_font_for_cp(e, cp);
                const char *run = pp;
                int runlen = n;
                pp += n;
                while (*pp) {
                    FcChar32 cp2;
                    int n2 = _wm_utf8_cp(pp, &cp2);
                    if (_wm_font_for_cp(e, cp2) != rf) break;
                    runlen += n2;
                    pp += n2;
                }
                XftDrawStringUtf8(d, &c8, rf, tx, ty,
                                  (const FcChar8 *)run, runlen);
                XGlyphInfo gi2;
                XftTextExtentsUtf8(dpy, rf, (const FcChar8 *)run, runlen,
                                   &gi2);
                tx += gi2.xOff;
            }
        }
        XftColorFree(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                     DefaultColormap(dpy, DefaultScreen(dpy)), &c8);
        XftDrawDestroy(d);
    }
#else
    (void)e;
#endif /* VT_HAVE_XFT */
}

/* Which decoration hotspot is at frame-local (x,y)? */
typedef enum {
    _FR_HIT_NONE = 0, _FR_HIT_TITLE, _FR_HIT_CLOSE, _FR_HIT_MAX,
    _FR_HIT_MIN, _FR_HIT_EDGE_L, _FR_HIT_EDGE_R, _FR_HIT_EDGE_T,
    _FR_HIT_EDGE_B, _FR_HIT_CORNER_TL, _FR_HIT_CORNER_TR,
    _FR_HIT_CORNER_BL, _FR_HIT_CORNER_BR,
} _fr_hit_t;

static _fr_hit_t _frame_hit(_client_t *c, int x, int y, int fw_, int fh_) {
    if (!_framed(c)) return _FR_HIT_NONE;
    if (c->fr_title <= 0) {         /* fullscreen: edges only */
        int m = _FR_BORDER + 4;
        bool l = x < m, r = x > fw_ - m, t = y < m, b = y > fh_ - m;
        if (l && t) return _FR_HIT_CORNER_TL;
        if (r && t) return _FR_HIT_CORNER_TR;
        if (l && b) return _FR_HIT_CORNER_BL;
        if (r && b) return _FR_HIT_CORNER_BR;
        if (l) return _FR_HIT_EDGE_L;
        if (r) return _FR_HIT_EDGE_R;
        if (t) return _FR_HIT_EDGE_T;
        if (b) return _FR_HIT_EDGE_B;
        return _FR_HIT_NONE;
    }
    int th = c->fr_title + _FR_BORDER;
    if (y <= th) {
        /* title row (top border included) */
        int bx = fw_ - _FR_BORDER - _FR_BTN_W;
        if (x >= bx) return _FR_HIT_CLOSE;
        if (x >= bx - (_FR_BTN_W + _FR_BTN_GAP)) return _FR_HIT_MAX;
        if (x >= bx - 2 * (_FR_BTN_W + _FR_BTN_GAP)) return _FR_HIT_MIN;
        return _FR_HIT_TITLE;
    }
    if (y >= fh_ - _FR_BORDER - 4) {
        bool l = x < _FR_BORDER + 8, r = x > fw_ - _FR_BORDER - 8;
        if (l) return _FR_HIT_CORNER_BL;
        if (r) return _FR_HIT_CORNER_BR;
        return _FR_HIT_EDGE_B;
    }
    if (x <= _FR_BORDER + 4) {
        if (y < th + 8) return _FR_HIT_CORNER_TL;
        return _FR_HIT_EDGE_L;
    }
    if (x >= fw_ - _FR_BORDER - 4) {
        if (y < th + 8) return _FR_HIT_CORNER_TR;
        return _FR_HIT_EDGE_R;
    }
    return _FR_HIT_NONE;
}

/* Create the frame and reparent the client into it. */
static void _frame_create(vt_wm_x11_t *e, _client_t *c) {
    Display *dpy = e->dpy;
    c->fr_title = _FR_TITLE;
    int fx, fy, fw_, fh_;
    _frame_geom(c, &fx, &fy, &fw_, &fh_);
    XSetWindowAttributes wa = {
        .override_redirect = False,
        .background_pixel = BlackPixel(dpy, DefaultScreen(dpy)),
        .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      ButtonMotionMask | SubstructureNotifyMask |
                      EnterWindowMask,
    };
    c->frame = XCreateWindow(dpy, e->root, fx, fy, (unsigned)fw_,
                             (unsigned)fh_, 0, CopyFromParent, InputOutput,
                             CopyFromParent, CWOverrideRedirect | CWBackPixel |
                             CWEventMask, &wa);
    c->framed = true;
    if (e->cur_default_set) XDefineCursor(dpy, c->frame, e->cur_default);

    /* reparent: guard against the reparent-unmap being read as a withdraw */
    c->expect_unmap = true;
    XReparentWindow(dpy, c->win, c->frame, _FR_BORDER,
                    c->fr_title + _FR_BORDER);
    XMapWindow(dpy, c->win);
    /* keep the alt-drag grabs working on the client window itself */
    unsigned int mods[] = { 0, LockMask, Mod2Mask, Mod5Mask };
    for (size_t i = 0; i < VT_ARRAY_SIZE(mods); i++) {
        XGrabButton(dpy, Button1, Mod1Mask | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
        XGrabButton(dpy, Button3, Mod1Mask | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
        XGrabButton(dpy, Button1, mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
    }
}

/* Destroy the frame, restoring the client to the root. */
static void _frame_destroy(vt_wm_x11_t *e, _client_t *c, bool destroyed) {
    if (!_framed(c)) return;
    Display *dpy = e->dpy;
    if (!destroyed) {
        XUnmapWindow(dpy, c->frame);
        c->expect_unmap = true;
        XReparentWindow(dpy, c->win, e->root, c->model.x, c->model.y);
        XMapWindow(dpy, c->win);
    }
    XDestroyWindow(dpy, c->frame);
    c->frame = None;
    c->framed = false;
}

/* Find the client whose frame is `w` (frame events). */
static _client_t *_find_frame(vt_wm_x11_t *e, Window w) {
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (_framed(c) && c->frame == w) return c;
    }
    return NULL;
}

/* Button press on the decoration frame: buttons, title drag, edges. */
static void _frame_button(vt_wm_x11_t *e, _client_t *c, XButtonEvent *be) {
    int fw_, fh_, fx, fy;
    _frame_geom(c, &fx, &fy, &fw_, &fh_);
    _fr_hit_t hit = _frame_hit(c, be->x, be->y, fw_, fh_);

    /* any press on the frame focuses the window first */
    if (!c->model.focused) _focus(e, c);

    if (be->button == Button1) {
        switch (hit) {
        case _FR_HIT_CLOSE:
            _close(e, c);
            return;
        case _FR_HIT_MAX:
            _maximize(e, c, !c->model.maximized);
            return;
        case _FR_HIT_MIN:
            _minimize(e, c, true);
            return;
        case _FR_HIT_TITLE: {
            /* double-click toggles maximize */
            if (c->last_title_click &&
                be->time - c->last_title_click < 400) {
                _maximize(e, c, !c->model.maximized);
                c->last_title_click = 0;
                return;
            }
            c->last_title_click = be->time;
            _raise(e, c);
            _op_start(e, c, 0, 0, be->x_root, be->y_root);
            return;
        }
        case _FR_HIT_EDGE_L:   _op_start(e, c, 1, 4, be->x_root, be->y_root); return;
        case _FR_HIT_EDGE_R:   _op_start(e, c, 1, 1, be->x_root, be->y_root); return;
        case _FR_HIT_EDGE_T:   _op_start(e, c, 1, 8, be->x_root, be->y_root); return;
        case _FR_HIT_EDGE_B:   _op_start(e, c, 1, 2, be->x_root, be->y_root); return;
        case _FR_HIT_CORNER_TL: _op_start(e, c, 1, 4 | 8, be->x_root, be->y_root); return;
        case _FR_HIT_CORNER_TR: _op_start(e, c, 1, 1 | 8, be->x_root, be->y_root); return;
        case _FR_HIT_CORNER_BL: _op_start(e, c, 1, 4 | 2, be->x_root, be->y_root); return;
        case _FR_HIT_CORNER_BR: _op_start(e, c, 1, 1 | 2, be->x_root, be->y_root); return;
        default:
            _raise(e, c);
            return;
        }
    } else if (be->button == Button3) {
        if (hit == _FR_HIT_TITLE || hit == _FR_HIT_NONE)
            _op_start(e, c, 1, 1 | 2, be->x_root, be->y_root);
        else
            _op_start(e, c, 1, 1 | 2, be->x_root, be->y_root);
    } else if (be->button == Button2) {
        _minimize(e, c, true);
    }
}

/* -------------------------------------------------------- focus/stack */
static void _restack(vt_wm_x11_t *e) {
    /* Stacking vector convention: BOTTOM-FIRST (last element = topmost).
     * Sorting by layer ascending keeps desktop at the bottom, docks and
     * fullscreen windows above normal ones. Raise in vector order so the
     * last element ends up on top. */
    vt_vec_sort(&e->stacking, _cmp_layer_asc);
    for (size_t i = 0; i < e->stacking.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->stacking, i);
        XRaiseWindow(e->dpy, _fr_win(c));
    }
    _update_client_list(e);
}

static void _raise(vt_wm_x11_t *e, _client_t *c) {
    for (size_t i = 0; i < e->stacking.size; i++) {
        _client_t **pp = vt_vec_at(&e->stacking, i);
        if (*pp == c) { vt_vec_remove(&e->stacking, i); break; }
    }
    vt_vec_push(&e->stacking, &c);
    XRaiseWindow(e->dpy, _fr_win(c));
    _restack(e);
}

static void _focus(vt_wm_x11_t *e, _client_t *c) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    _client_t *prev = NULL;
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *p = *(_client_t **)vt_vec_at(&e->clients, i);
        if (p->model.focused) prev = p;
        p->model.focused = (p == c);
    }
    if (prev && prev != c) _frame_paint(e, prev);   /* dim the old frame */
    if (c) {
        if (c->input_hint || !c->take_focus)
            XSetInputFocus(e->dpy, c->win, RevertToPointerRoot, CurrentTime);
        if (c->take_focus) {
            XEvent msg = { .type = ClientMessage };
            msg.xclient.window = c->win;
            msg.xclient.message_type = a->wm_protocols;
            msg.xclient.format = 32;
            msg.xclient.data.l[0] = (long)a->wm_take_focus;
            msg.xclient.data.l[1] = CurrentTime;
            XSendEvent(e->dpy, c->win, False, NoEventMask, &msg);
        }
        /* EWMH: _NET_ACTIVE_WINDOW is of type WINDOW */
        Window w = c->win;
        XChangeProperty(e->dpy, e->root, a->net_active_window, XA_WINDOW, 32,
                        PropModeReplace, (const unsigned char *)&w, 1);
        _raise(e, c);
        _frame_paint(e, c);       /* brighten the new frame */
        for (size_t i = 0; i < e->clients.size; i++) {
            _client_t **pp = vt_vec_at(&e->clients, i);
            if (*pp == c) { vt_vec_remove(&e->clients, i); break; }
        }
        vt_vec_push(&e->clients, &c);
        _emit_win(e, c, VT_WM_EVENT_FOCUS);
    } else {
        XSetInputFocus(e->dpy, e->root, RevertToNone, CurrentTime);
        unsigned long none = 0;
        _set_cardinal_list(e->root, a->net_active_window, &none, 1);
    }
}

static void _focus_top_on_desktop(vt_wm_x11_t *e) {
    _client_t *best = NULL;
    for (size_t i = e->stacking.size; i > 0; i--) {
        /* last element = topmost */
        _client_t *c = *(_client_t **)vt_vec_at(&e->stacking, i - 1);
        if (c->model.minimized || c->is_dock || c->is_desktop) continue;
        if (!c->model.sticky && c->model.workspace != e->cur_desktop) continue;
        best = c;
        break;
    }
    _focus(e, best);
}

/* ----------------------------------------------------------- workarea */
static void _apply_configure(vt_wm_x11_t *e, _client_t *c, int x, int y,
                             int w, int h);
static void _push_model(vt_wm_x11_t *e, _client_t *c);

static void _update_workarea(vt_wm_x11_t *e) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    Atom strut = XInternAtom(e->dpy, "_NET_WM_STRUT", False);
    Atom strut_p = XInternAtom(e->dpy, "_NET_WM_STRUT_PARTIAL", False);
    _rect_t wa = { 0, 0, DisplayWidth(e->dpy, DefaultScreen(e->dpy)),
                         DisplayHeight(e->dpy, DefaultScreen(e->dpy)) };
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (!c->is_dock) continue;
        unsigned long *vals = NULL;
        unsigned long n = 0;
        if (vt_x11_get_window_property(c->win, strut_p, a->cardinal,
                                       (unsigned char **)&vals, &n) && vals) {
            if (n >= 12) {
                if (vals[2] > 0) { wa.y = (int)vals[2]; wa.h -= (int)vals[2]; }
                if (vals[3] > 0) { wa.h -= (int)vals[3]; }
                if (vals[0] > 0) { wa.x = (int)vals[0]; wa.w -= (int)vals[0]; }
                if (vals[1] > 0) { wa.w -= (int)vals[1]; }
            }
            XFree(vals);
            continue;
        }
        if (vt_x11_get_window_property(c->win, strut, a->cardinal,
                                       (unsigned char **)&vals, &n) && vals) {
            if (n >= 4) {
                if (vals[2] > 0) { wa.y = (int)vals[2]; wa.h -= (int)vals[2]; }
                if (vals[3] > 0) { wa.h -= (int)vals[3]; }
                if (vals[0] > 0) { wa.x = (int)vals[0]; wa.w -= (int)vals[0]; }
                if (vals[1] > 0) { wa.w -= (int)vals[1]; }
            }
            XFree(vals);
        }
    }
    if (wa.w < 50) wa.w = 50;
    if (wa.h < 50) wa.h = 50;
    _rect_t old = e->workarea;
    e->workarea = wa;
    _update_desktop_props(e);
    /* Workarea changed (a panel just docked, or the strut arrived
     * late): nudge fully-visible windows that now violate the new
     * workarea back inside it — the classic "window too high, title
     * bar hidden behind the panel" case at session start. Windows the
     * user placed are only shifted as far as needed, never resized. */
    if (old.w == 0 && old.h == 0) return;              /* first pass */
    int dy = wa.y - old.y;
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (!c || c->is_dock || c->is_desktop) continue;
        if (c->model.maximized || c->model.fullscreen) continue;
        int ny = c->model.y;
        if (dy > 0 && c->model.y < wa.y)
            ny = wa.y;                                  /* below a top panel */
        else if (dy < 0 && c->model.y >= old.y)
            ny = c->model.y + dy;                       /* panel removed */
        if (ny != c->model.y) {
            c->model.y = ny;
            _apply_configure(e, c, c->model.x, c->model.y,
                             c->model.w, c->model.h);
            _push_model(e, c);
        }
    }
}

static _rect_t _output_for_window(vt_wm_x11_t *e, _client_t *c) {
    int cx = c->model.x + c->model.w / 2;
    int cy = c->model.y + c->model.h / 2;
    _rect_t o = e->workarea;
    if (e->wm->backend) {
        size_t n = vt_backend_output_count(e->wm->backend);
        for (size_t i = 0; i < n; i++) {
            const vt_output_t *out = vt_backend_output_at(e->wm->backend, i);
            if (!out || !out->enabled) continue;
            if (cx >= out->x && cx < out->x + out->w &&
                cy >= out->y && cy < out->y + out->h) {
                o.x = out->x; o.y = out->y; o.w = out->w; o.h = out->h;
                return o;
            }
        }
    }
    return o;
}

/* ------------------------------------------------------------ manage */
static void _apply_configure(vt_wm_x11_t *e, _client_t *c, int x, int y,
                             int w, int h) {
    if (c->has_size_hints) {
        if (c->min_w > 0 && w < c->min_w) w = (int)c->min_w;
        if (c->min_h > 0 && h < c->min_h) h = (int)c->min_h;
        if (c->max_w > 0 && c->max_w < 100000 && w > c->max_w) w = (int)c->max_w;
        if (c->max_h > 0 && c->max_h < 100000 && h > c->max_h) h = (int)c->max_h;
        if (c->inc_w > 1 && c->base_w >= 0 && w > c->base_w)
            w = (int)(c->base_w + ((w - c->base_w) / c->inc_w) * c->inc_w);
        if (c->inc_h > 1 && c->base_h >= 0 && h > c->base_h)
            h = (int)(c->base_h + ((h - c->base_h) / c->inc_h) * c->inc_h);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    c->model.x = x; c->model.y = y;
    c->model.w = w; c->model.h = h;
    if (_framed(c)) {
        int fx, fy, fw_, fh_;
        _frame_geom(c, &fx, &fy, &fw_, &fh_);
        XMoveResizeWindow(e->dpy, c->frame, fx, fy, (unsigned)fw_,
                          (unsigned)fh_);
        XMoveResizeWindow(e->dpy, c->win, _FR_BORDER,
                          c->fr_title + _FR_BORDER, (unsigned)w, (unsigned)h);
        _frame_paint(e, c);
        /* tell the client its root-relative geometry (ICCCM 4.2.3) */
        _send_configure(e, c);
    } else {
        XWindowChanges wc = { .x = x, .y = y, .width = w, .height = h,
                              .border_width = 0, .sibling = None,
                              .stack_mode = Above };
        XConfigureWindow(e->dpy, c->win,
                         CWX | CWY | CWWidth | CWHeight | CWBorderWidth |
                         CWStackMode, &wc);
    }
}

static void _manage(vt_wm_x11_t *e, Window w) {
    if (_find(e, w)) return;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(e->dpy, w, &wa)) return;
    if (wa.override_redirect) return;

    const vt_x11_atoms_t *a = vt_x11_atoms();
    _client_t *c = vt_malloc0(sizeof(*c));
    c->win = w;
    c->model.id = (uint32_t)w;
    c->input_hint = true;
    c->model.x = wa.x; c->model.y = wa.y;
    c->model.w = wa.width; c->model.h = wa.height;
    c->model.workspace = e->cur_desktop;
    c->model.layer = VT_WM_LAYER_NORMAL;
    c->model.mapped = (wa.map_state == IsViewable);

    XAddToSaveSet(e->dpy, w);
    _read_class(c, e->dpy);
    _read_size_hints(c, e->dpy);
    _read_protocols(c, e->dpy);
    _read_type_and_state(c);
    _read_wm_hints(c, e->dpy);
    _read_transient(c, e->dpy);
    c->model.title = _get_title(c, e->dpy);
    unsigned long desktop = (unsigned long)e->cur_desktop;
    if (vt_x11_get_cardinal_property(w, a->net_wm_desktop, &desktop)) {
        if (desktop == 0xfffffffful) c->model.sticky = true;
        else if (desktop < (unsigned long)e->n_desktops)
            c->model.workspace = (int)desktop;
    }
    if (c->is_desktop || c->model.sticky) c->model.workspace = e->cur_desktop;

    bool has_pos = false;
    {
        XSizeHints sh;
        long supplied = 0;
        if (XGetWMNormalHints(e->dpy, c->win, &sh, &supplied) &&
            (sh.flags & (USPosition | PPosition)))
            has_pos = true;
    }
    if (c->transient_for && !has_pos) {
        _client_t *parent = _find(e, c->transient_for);
        if (parent) {
            c->model.x = parent->model.x + (parent->model.w - c->model.w) / 2;
            c->model.y = parent->model.y + (parent->model.h - c->model.h) / 2;
        }
    } else if (!c->is_dock && !c->is_desktop && !has_pos) {
        /* smart placement: cascade near the pointer's output */
        Window rr, cr; int px = 0, py = 0, rx = 0, ry = 0;
        unsigned int mask = 0;
        _rect_t out = e->workarea;
        if (XQueryPointer(e->dpy, e->root, &rr, &cr, &rx, &ry, &px, &py, &mask)) {
            if (e->wm->backend) {
                size_t n = vt_backend_output_count(e->wm->backend);
                for (size_t i = 0; i < n; i++) {
                    const vt_output_t *o = vt_backend_output_at(e->wm->backend, i);
                    if (o && rx >= o->x && rx < o->x + o->w &&
                        ry >= o->y && ry < o->y + o->h) {
                        out.x = o->x; out.y = o->y; out.w = o->w; out.h = o->h;
                        break;
                    }
                }
            }
        }
        static int cascade = 0;
        c->model.x = out.x + 40 + (cascade % 8) * 28;
        c->model.y = out.y + 40 + (cascade % 8) * 28;
        cascade++;
        if (c->model.x + c->model.w > out.x + out.w)
            c->model.x = out.x + (out.w - c->model.w) / 2;
        if (c->model.y + c->model.h > out.y + out.h)
            c->model.y = out.y + (out.h - c->model.h) / 2;
        if (c->model.x < out.x) c->model.x = out.x;
        if (c->model.y < out.y) c->model.y = out.y;
    }

    _apply_configure(e, c, c->model.x, c->model.y, c->model.w, c->model.h);

    _set_wm_state(e, c, NormalState);
    _set_net_wm_desktop(e, c);
    _set_allowed_actions(e, c);

    /* decorations: docks/desktops stay undecorated; MOTIF-decorating
     * windows (CSD apps: GTK headerbars, Chromium, Firefox with the
     * system titlebar off) manage their own chrome and must not be
     * double-decorated */
    if (!c->is_dock && !c->is_desktop && !_motif_undecorated(c, e->dpy))
        _frame_create(e, c);
    _set_frame_extents(e, c);
    _set_state_atoms(e, c);

    /* grab alt-drag move/resize AND plain Button1 (click-to-focus) on
     * the client, ignoring lock modifiers. The plain grab makes the WM
     * see every click first; ReplayPointer then delivers it to the
     * client — the classic click-to-focus mechanism. */
    unsigned int mods[] = { 0, LockMask, Mod2Mask, Mod5Mask, LockMask | Mod2Mask,
                            LockMask | Mod5Mask, Mod2Mask | Mod5Mask,
                            LockMask | Mod2Mask | Mod5Mask };
    for (size_t i = 0; i < VT_ARRAY_SIZE(mods); i++) {
        XGrabButton(e->dpy, Button1, Mod1Mask | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
        XGrabButton(e->dpy, Button3, Mod1Mask | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
        XGrabButton(e->dpy, Button1, mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeSync, GrabModeSync, None, None);
    }
    XSelectInput(e->dpy, c->win, EnterWindowMask | FocusChangeMask |
                 PropertyChangeMask | StructureNotifyMask);

    vt_vec_push(&e->clients, &c);
    vt_vec_push(&e->stacking, &c);
    _push_model(e, c);
    _restack(e);
    if (c->is_dock || c->is_desktop) _update_workarea(e);

    XMapWindow(e->dpy, w);
    if (_framed(c)) {
        XMapWindow(e->dpy, c->frame);
        _frame_paint(e, c);
    }

    if (!c->is_dock && !c->is_desktop &&
        (c->model.sticky || c->model.workspace == e->cur_desktop))
        _focus(e, c);
    vt_logi("wm: manage 0x%lx '%s' %dx%d+%d+%d ws=%d", (unsigned long)w,
            c->model.title ? c->model.title : "?", c->model.w, c->model.h,
            c->model.x, c->model.y, c->model.workspace);
    _emit_win(e, c, VT_WM_EVENT_OPEN);
}

static void _unmanage(vt_wm_x11_t *e, Window w, bool destroyed) {
    _client_t *c = _find(e, w);
    if (!c) return;
    _emit_win(e, c, VT_WM_EVENT_CLOSE);
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t **pp = vt_vec_at(&e->clients, i);
        if (*pp == c) { vt_vec_remove(&e->clients, i); break; }
    }
    for (size_t i = 0; i < e->stacking.size; i++) {
        _client_t **pp = vt_vec_at(&e->stacking, i);
        if (*pp == c) { vt_vec_remove(&e->stacking, i); break; }
    }
    _remove_model(e, c);
    if (!destroyed) {
        XRemoveFromSaveSet(e->dpy, w);
        XUngrabButton(e->dpy, AnyButton, AnyModifier, w);
    }
    _frame_destroy(e, c, destroyed);
    bool was_dock = c->is_dock;
    bool was_focused = c->model.focused;
    _client_free(c);
    _update_client_list(e);
    if (was_dock) _update_workarea(e);
    if (was_focused) _focus_top_on_desktop(e);
    vt_logi("wm: unmanage 0x%lx", (unsigned long)w);
}

/* -------------------------------------------------- state transitions */
static void _maximize(vt_wm_x11_t *e, _client_t *c, bool on) {
    if (on == c->model.maximized) return;
    if (on) {
        _rect_t out = _output_for_window(e, c);
        c->model.prev_x = c->model.x; c->model.prev_y = c->model.y;
        c->model.prev_w = c->model.w; c->model.prev_h = c->model.h;
        c->model.maximized = true;
        _apply_configure(e, c, out.x, out.y, out.w, out.h);
    } else {
        c->model.maximized = false;
        _apply_configure(e, c, c->model.prev_x, c->model.prev_y,
                         c->model.prev_w, c->model.prev_h);
    }
    _set_state_atoms(e, c);
    _emit_win(e, c, VT_WM_EVENT_STATE);
}

static void _fullscreen(vt_wm_x11_t *e, _client_t *c, bool on) {
    if (on == c->model.fullscreen) return;
    if (on) {
        _rect_t out = _output_for_window(e, c);
        c->model.prev_x = c->model.x; c->model.prev_y = c->model.y;
        c->model.prev_w = c->model.w; c->model.prev_h = c->model.h;
        c->model.fullscreen = true;
        c->fr_title = 0;          /* chrome off while fullscreen */
        _apply_configure(e, c, out.x, out.y, out.w, out.h);
        _raise(e, c);
    } else {
        c->model.fullscreen = false;
        c->fr_title = _FR_TITLE;
        _apply_configure(e, c, c->model.prev_x, c->model.prev_y,
                         c->model.prev_w, c->model.prev_h);
    }
    _set_state_atoms(e, c);
    _set_frame_extents(e, c);
    _emit_win(e, c, VT_WM_EVENT_STATE);
}

static void _minimize(vt_wm_x11_t *e, _client_t *c, bool on) {
    if (on == c->model.minimized) return;
    c->model.minimized = on;
    if (on) {
        XUnmapWindow(e->dpy, _fr_win(c));
        c->model.mapped = false;
        _set_wm_state(e, c, IconicState);
        if (c->model.focused) { c->model.focused = false; _focus_top_on_desktop(e); }
    } else {
        XMapWindow(e->dpy, _fr_win(c));
        if (_framed(c)) XMapWindow(e->dpy, c->win);
        c->model.mapped = true;
        _set_wm_state(e, c, NormalState);
        _focus(e, c);
    }
    _set_state_atoms(e, c);
    _emit_win(e, c, VT_WM_EVENT_STATE);
}

static void _close(vt_wm_x11_t *e, _client_t *c) {
    if (c->delete_window) {
        const vt_x11_atoms_t *a = vt_x11_atoms();
        XEvent msg = { .type = ClientMessage };
        msg.xclient.window = c->win;
        msg.xclient.message_type = a->wm_protocols;
        msg.xclient.format = 32;
        msg.xclient.data.l[0] = (long)a->wm_delete_window;
        msg.xclient.data.l[1] = CurrentTime;
        XSendEvent(e->dpy, c->win, False, NoEventMask, &msg);
    } else {
        XKillClient(e->dpy, c->win);
    }
    _emit_win(e, c, VT_WM_EVENT_CLOSE_REQUEST);
}

static void _tile(vt_wm_x11_t *e, _client_t *c, vt_wm_tile_t t) {
    _rect_t out = _output_for_window(e, c);
    if (c->model.maximized) _maximize(e, c, false);
    if (c->model.fullscreen) _fullscreen(e, c, false);
    switch (t) {
    case VT_WM_TILE_LEFT:
        _apply_configure(e, c, out.x, out.y, out.w / 2, out.h); break;
    case VT_WM_TILE_RIGHT:
        _apply_configure(e, c, out.x + out.w / 2, out.y, out.w / 2, out.h); break;
    case VT_WM_TILE_TOP:
        _apply_configure(e, c, out.x, out.y, out.w, out.h / 2); break;
    case VT_WM_TILE_BOTTOM:
        _apply_configure(e, c, out.x, out.y + out.h / 2, out.w, out.h / 2); break;
    case VT_WM_TILE_MAX:
        _maximize(e, c, true); break;
    case VT_WM_TILE_FULLSCREEN:
        _fullscreen(e, c, true); break;
    default: break;
    }
    c->model.tile = t;
    _emit_win(e, c, VT_WM_EVENT_GEOMETRY);
}

static void _set_desktop(vt_wm_x11_t *e, int d) {
    if (d < 0 || d >= e->n_desktops || d == e->cur_desktop) return;
    e->cur_desktop = d;
    e->wm->cur_ws = d;
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (c->is_dock || c->is_desktop || c->model.sticky) continue;
        if (c->model.workspace == d && !c->model.minimized) {
            XMapWindow(e->dpy, _fr_win(c));
            if (_framed(c)) XMapWindow(e->dpy, c->win);
            c->model.mapped = true;
            _set_wm_state(e, c, NormalState);
        } else if (c->model.workspace != d && !c->model.minimized) {
            XUnmapWindow(e->dpy, _fr_win(c));
            c->model.mapped = false;
            _set_wm_state(e, c, IconicState);
        }
    }
    _update_desktop_props(e);
    _focus_top_on_desktop(e);
    _emit_desktop(e);
    vt_logi("wm: desktop -> %d", d + 1);
}

static void _move_to_desktop(vt_wm_x11_t *e, _client_t *c, int d) {
    if (d < 0 || d >= e->n_desktops) return;
    if (d == e->cur_desktop) {
        if (c->model.workspace != e->cur_desktop) {
            c->model.workspace = d;
            _set_net_wm_desktop(e, c);
            XMapWindow(e->dpy, _fr_win(c));
            if (_framed(c)) XMapWindow(e->dpy, c->win);
            c->model.mapped = true;
            _set_wm_state(e, c, NormalState);
            _focus(e, c);
        }
        return;
    }
    if (!c->model.sticky && c->model.workspace == e->cur_desktop &&
        !c->model.minimized) {
        XUnmapWindow(e->dpy, _fr_win(c));
        c->model.mapped = false;
        _set_wm_state(e, c, IconicState);
    }
    c->model.workspace = d;
    _set_net_wm_desktop(e, c);
    _emit_win(e, c, VT_WM_EVENT_STATE);
}

/* ------------------------------------------------ interactive move */
static void _op_start(vt_wm_x11_t *e, _client_t *c, int mode, int edge,
                      int px, int py) {
    if (c->model.fullscreen || c->is_dock || c->is_desktop) return;
    e->in_op = true;
    e->op_mode = mode;
    e->op_edge = edge;
    e->op_client = c;
    e->op_start_x = px; e->op_start_y = py;
    e->op_win_x = c->model.x; e->op_win_y = c->model.y;
    e->op_win_w = c->model.w; e->op_win_h = c->model.h;
    XGrabPointer(e->dpy, e->root, True,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None,
                 mode == 0 ? e->cur_move : e->cur_resize, CurrentTime);
}

static void _op_motion(vt_wm_x11_t *e, int px, int py) {
    if (!e->in_op || !e->op_client) return;
    _client_t *c = e->op_client;
    int dx = px - e->op_start_x, dy = py - e->op_start_y;
    if (e->op_mode == 0) {
        _apply_configure(e, c, e->op_win_x + dx, e->op_win_y + dy,
                         c->model.w, c->model.h);
    } else {
        int x = e->op_win_x, y = e->op_win_y;
        int w = e->op_win_w, h = e->op_win_h;
        if (e->op_edge & 1) { w = e->op_win_w + dx; }
        if (e->op_edge & 2) { h = e->op_win_h + dy; }
        if (e->op_edge & 4) { x = e->op_win_x + dx; w = e->op_win_w - dx; }
        if (e->op_edge & 8) { y = e->op_win_y + dy; h = e->op_win_h - dy; }
        _apply_configure(e, c, x, y, w, h);
    }
}

static void _op_end(vt_wm_x11_t *e) {
    if (!e->in_op) return;
    _client_t *c = e->op_client;
    XUngrabPointer(e->dpy, CurrentTime);
    e->in_op = false;
    e->op_client = NULL;
    if (c && e->snap_enabled && e->op_mode == 0) {
        _rect_t out = _output_for_window(e, c);
        int px = c->model.x + c->model.w / 2;
        int py = c->model.y + c->model.h / 2;
        int m = 8;
        if (py < out.y + m) _tile(e, c, VT_WM_TILE_TOP);
        else if (py > out.y + out.h - m) _tile(e, c, VT_WM_TILE_BOTTOM);
        else if (px < out.x + m) _tile(e, c, VT_WM_TILE_LEFT);
        else if (px > out.x + out.w - m) _tile(e, c, VT_WM_TILE_RIGHT);
    }
    if (c) _emit_win(e, c, VT_WM_EVENT_GEOMETRY);
}

/* ------------------------------------------------------------ hotkeys */
static bool _parse_combo(Display *dpy, const char *combo, int *keycode,
                         unsigned int *mods) {
    unsigned int m = 0;
    KeySym ks = 0;
    char *copy = vt_strdup(combo);
    char *save = NULL;
    for (char *tok = strtok_r(copy, "+", &save); tok;
         tok = strtok_r(NULL, "+", &save)) {
        if (vt_strcaseeq(tok, "alt")) m |= Mod1Mask;
        else if (vt_strcaseeq(tok, "super") || vt_strcaseeq(tok, "meta"))
            m |= Mod4Mask;
        else if (vt_strcaseeq(tok, "ctrl") || vt_strcaseeq(tok, "control"))
            m |= ControlMask;
        else if (vt_strcaseeq(tok, "shift")) m |= ShiftMask;
        else ks = XStringToKeysym(tok);
    }
    vt_free(copy);
    if (ks == 0 || ks == NoSymbol) return false;
    *keycode = XKeysymToKeycode(dpy, ks);
    *mods = m;
    return *keycode != 0;
}

static void _grab_key(vt_wm_x11_t *e, int keycode, unsigned int mods) {
    unsigned int variants[] = { 0, LockMask, Mod2Mask, Mod5Mask,
                                LockMask | Mod2Mask, LockMask | Mod5Mask,
                                Mod2Mask | Mod5Mask,
                                LockMask | Mod2Mask | Mod5Mask };
    for (size_t i = 0; i < VT_ARRAY_SIZE(variants); i++)
        XGrabKey(e->dpy, keycode, mods | variants[i], e->root, False,
                 GrabModeAsync, GrabModeAsync);
}

static void _grab_all(vt_wm_x11_t *e) {
    for (size_t i = 0; i < e->grabs.size; i++) {
        _grab_t *g = vt_vec_at(&e->grabs, i);
        if (g->keycode) _grab_key(e, g->keycode, g->mods);
    }
}

/* ------------------------------------------------------- event handle */
static void _send_configure(vt_wm_x11_t *e, _client_t *c) {
    XConfigureEvent ce = { 0 };
    ce.type = ConfigureNotify;
    ce.event = c->win;
    ce.window = c->win;
    ce.x = c->model.x; ce.y = c->model.y;
    ce.width = c->model.w; ce.height = c->model.h;
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(e->dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static void _handle_client_message(vt_wm_x11_t *e, XClientMessageEvent *cm) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    if (cm->window == e->root) {
        if (cm->message_type == a->net_current_desktop)
            _set_desktop(e, (int)cm->data.l[0]);
        else if (cm->message_type == a->net_active_window) {
            /* EWMH root form: the window to activate travels in l[2] */
            _client_t *c = _find(e, (Window)cm->data.l[2]);
            if (c) {
                if (c->model.minimized) _minimize(e, c, false);
                _focus(e, c);
            }
        }
        return;
    }
    _client_t *c = _find(e, cm->window);
    if (!c) return;
    if (cm->message_type == a->net_active_window) {
        if (c->model.minimized) _minimize(e, c, false);
        _focus(e, c);
    } else if (cm->message_type == a->net_close_window) {
        _close(e, c);
    } else if (cm->message_type == a->net_wm_state) {
        long action = cm->data.l[0];
        for (int i = 1; i <= 2; i++) {
            Atom at = (Atom)cm->data.l[i];
            if (!at) continue;
            bool cur = (at == a->net_wm_state_maximized_vert ||
                        at == a->net_wm_state_maximized_horz) ? c->model.maximized
                       : at == a->net_wm_state_fullscreen ? c->model.fullscreen
                       : at == a->net_wm_state_sticky ? c->model.sticky
                       : at == a->net_wm_state_hidden ? c->model.minimized
                       : at == a->net_wm_state_above
                         ? c->model.layer == VT_WM_LAYER_ABOVE
                       : at == a->net_wm_state_below
                         ? c->model.layer == VT_WM_LAYER_BELOW
                       : false;
            bool on = action == 1 ? true : action == 2 ? !cur : false;
            if (at == a->net_wm_state_maximized_vert ||
                at == a->net_wm_state_maximized_horz)
                _maximize(e, c, on);
            else if (at == a->net_wm_state_fullscreen)
                _fullscreen(e, c, on);
            else if (at == a->net_wm_state_sticky) {
                c->model.sticky = on;
                _set_state_atoms(e, c);
                _set_net_wm_desktop(e, c);
            } else if (at == a->net_wm_state_above) {
                c->model.layer = on ? VT_WM_LAYER_ABOVE : VT_WM_LAYER_NORMAL;
                _set_state_atoms(e, c);
                _restack(e);
            } else if (at == a->net_wm_state_below) {
                c->model.layer = on ? VT_WM_LAYER_BELOW : VT_WM_LAYER_NORMAL;
                _set_state_atoms(e, c);
                _restack(e);
            } else if (at == a->net_wm_state_hidden)
                _minimize(e, c, on);
            else if (at == a->net_wm_state_demands_attention)
                c->model.urgent = on;
        }
    } else if (cm->message_type == a->net_wm_desktop) {
        long d = cm->data.l[0];
        if (d == 0xffffffffL || d == -1L) {
            c->model.sticky = true;
            _set_net_wm_desktop(e, c);
        } else {
            _move_to_desktop(e, c, (int)d);
        }
    } else if (cm->message_type == a->net_wm_moveresize) {
        long dir = cm->data.l[2];
        if (dir == 8) {
            _op_start(e, c, 0, 0, (int)cm->data.l[0], (int)cm->data.l[1]);
        } else if (dir >= 0 && dir <= 7) {
            int edge = 0;
            if (dir == 1 || dir == 3 || dir == 5 || dir == 7) edge |= 1;
            if (dir == 2 || dir == 3 || dir == 6 || dir == 7) edge |= 2;
            if (dir == 4 || dir == 5 || dir == 6 || dir == 7) edge |= 4 | 8;
            _op_start(e, c, 1, edge, (int)cm->data.l[0], (int)cm->data.l[1]);
        }
    } else if (cm->message_type == a->net_restack_window) {
        Window sibling = (Window)cm->data.l[1];
        long detail = cm->data.l[2];
        if (sibling) {
            XWindowChanges wc = { .sibling = sibling,
                                  .stack_mode = detail == 0 ? Below : Above };
            XConfigureWindow(e->dpy, c->win, CWSibling | CWStackMode, &wc);
        }
        _restack(e);
    } else if (cm->message_type == a->wm_change_state) {
        if (cm->data.l[0] == IconicState) _minimize(e, c, true);
        else if (cm->data.l[0] == NormalState) _minimize(e, c, false);
    } else if (cm->message_type == a->net_request_frame_extents) {
        _set_frame_extents(e, c);
    }
}

static void _handle_property(vt_wm_x11_t *e, XPropertyEvent *pe) {
    const vt_x11_atoms_t *a = vt_x11_atoms();
    _client_t *c = _find(e, pe->window);
    if (!c) return;
    if (pe->atom == XA_WM_NAME || pe->atom == a->net_wm_name ||
        pe->atom == a->net_wm_visible_name) {
        char *t = _get_title(c, e->dpy);
        if (t) {
            vt_free(c->model.title);
            c->model.title = t;
            _emit_win(e, c, VT_WM_EVENT_TITLE);
        }
    } else if (pe->atom == a->motif_wm_hints) {
        /* the app turned its own decorations on/off (browser CSD
         * toggles do exactly this) — follow it immediately */
        _apply_motif(e, c);
    } else if (pe->atom == XA_WM_NORMAL_HINTS) {
        c->has_size_hints = false;
        c->min_w = c->min_h = c->max_w = c->max_h = 0;
        c->base_w = c->base_h = c->inc_w = c->inc_h = 0;
        _read_size_hints(c, e->dpy);
    } else if (pe->atom == XA_WM_HINTS) {
        _read_wm_hints(c, e->dpy);
    } else if (pe->atom == XA_WM_TRANSIENT_FOR) {
        _read_transient(c, e->dpy);
    } else if (pe->atom == a->net_wm_state ||
               pe->atom == a->net_wm_window_type) {
        bool fs = c->model.fullscreen;
        bool st = c->model.sticky;
        bool was_dock = c->is_dock, was_desktop = c->is_desktop;
        _read_type_and_state(c);
        if (fs != c->model.fullscreen) _fullscreen(e, c, c->model.fullscreen);
        else if (st != c->model.sticky) _set_net_wm_desktop(e, c);
        else _set_state_atoms(e, c);
        if (was_dock != c->is_dock || was_desktop != c->is_desktop)
            _update_workarea(e);
    } else if (pe->atom == a->net_wm_window_opacity) {
        unsigned long op = 0xffffffff;
        if (vt_x11_get_cardinal_property(c->win, a->net_wm_window_opacity, &op))
            c->opacity = op;
    } else if (pe->atom == XInternAtom(e->dpy, "_NET_WM_STRUT", False) ||
               pe->atom == XInternAtom(e->dpy, "_NET_WM_STRUT_PARTIAL", False)) {
        if (c->is_dock) _update_workarea(e);
    }
}

static void _on_backend_event(void *ud, void *event) {
    vt_wm_x11_t *e = ud;
    XEvent *ev = event;

    switch (ev->type) {
    case MapRequest: {
        XMapRequestEvent *mr = &ev->xmaprequest;
        _client_t *c = _find(e, mr->window);
        if (c) {
            if (c->model.minimized) _minimize(e, c, false);
            else {
                XMapWindow(e->dpy, mr->window);
                if (_framed(c)) XMapWindow(e->dpy, c->frame);
                _focus(e, c);
            }
        } else {
            _manage(e, mr->window);
        }
        break;
    }
    case ConfigureRequest: {
        XConfigureRequestEvent *cr = &ev->xconfigurerequest;
        _client_t *c = _find(e, cr->window);
        if (c) {
            /* client coordinates are relative to its parent — the frame */
            int x = (cr->value_mask & CWX)
                        ? cr->x - _FR_BORDER : c->model.x;
            int y = (cr->value_mask & CWY)
                        ? cr->y - (c->fr_title + _FR_BORDER) : c->model.y;
            int w = (cr->value_mask & CWWidth) ? cr->width : c->model.w;
            int h = (cr->value_mask & CWHeight) ? cr->height : c->model.h;
            if (c->model.maximized || c->model.fullscreen) {
                _send_configure(e, c);
            } else {
                _apply_configure(e, c, x, y, w, h);
                _emit_win(e, c, VT_WM_EVENT_GEOMETRY);
            }
        } else {
            XWindowChanges wc = { .x = cr->x, .y = cr->y,
                                  .width = cr->width, .height = cr->height,
                                  .border_width = cr->border_width,
                                  .sibling = cr->above,
                                  .stack_mode = cr->detail };
            XConfigureWindow(e->dpy, cr->window,
                             (unsigned int)cr->value_mask, &wc);
        }
        break;
    }
    case MapNotify: {
        _client_t *c = _find(e, ev->xmap.window);
        if (c) c->model.mapped = true;
        break;
    }
    case UnmapNotify: {
        XUnmapEvent *ue = &ev->xunmap;
        _client_t *c = _find(e, ue->window);
        if (!c) break;
        c->model.mapped = false;
        /* Top-level unmaps arrive on the root (unframed clients) or on
         * the frame (framed clients: the WM selected
         * SubstructureNotifyMask there). Minimize/workspace switches
         * unmap the FRAME, so a client unmap means a real withdrawal —
         * except the synthetic one the reparent generates, which the
         * expect_unmap guard absorbs. */
        if (ue->event != e->root && ue->event != c->frame) break;
        if (c->expect_unmap) { c->expect_unmap = false; break; }
        if (!ue->send_event) {
            _set_wm_state(e, c, WithdrawnState);
            _unmanage(e, ue->window, false);
        }
        break;
    }
    case DestroyNotify: {
        XDestroyWindowEvent *de = &ev->xdestroywindow;
        if (_find(e, de->window)) _unmanage(e, de->window, true);
        break;
    }
    case ClientMessage:
        _handle_client_message(e, &ev->xclient);
        break;
    case PropertyNotify:
        _handle_property(e, &ev->xproperty);
        break;
    case EnterNotify: {
        if (!e->sloppy_focus) break;
        XEnterWindowEvent *ee = &ev->xcrossing;
        if (ee->mode != NotifyNormal || ee->detail == NotifyInferior) break;
        /* Restacking windows under a stationary pointer synthesizes
         * EnterNotify events; focusing from those re-restacks and loops
         * forever. Only honor crossings that involve actual pointer
         * movement (root coordinates changed since the last one). */
        static int _last_root_x = -1, _last_root_y = -1;
        if (ee->x_root == _last_root_x && ee->y_root == _last_root_y)
            break;
        _last_root_x = ee->x_root;
        _last_root_y = ee->y_root;
        _client_t *c = _find(e, ee->window);
        if (c && !c->model.minimized && !c->is_dock && !c->is_desktop &&
            !c->model.focused &&
            (c->model.sticky || c->model.workspace == e->cur_desktop))
            _focus(e, c);
        break;
    }
    case ButtonPress: {
        XButtonEvent *be = &ev->xbutton;
        _client_t *c = _find(e, be->window);
        if (!c) c = _find_frame(e, be->window);
        if (c && _framed(c) && be->window == c->frame) {
            _frame_button(e, c, be);
            break;
        }
        if (c && (be->state & Mod1Mask) && !c->model.fullscreen) {
            _raise(e, c);
            if (be->button == Button1)
                _op_start(e, c, 0, 0, be->x_root, be->y_root);
            else if (be->button == Button3)
                _op_start(e, c, 1, 1 | 2, be->x_root, be->y_root);
            XAllowEvents(e->dpy, AsyncPointer, CurrentTime);
        } else if (c && be->button == Button1) {
            if (!c->model.focused) _focus(e, c);
            else _raise(e, c);
            XAllowEvents(e->dpy, ReplayPointer, CurrentTime);
        } else {
            XAllowEvents(e->dpy, AsyncPointer, CurrentTime);
        }
        break;
    }
    case Expose: {
        if (ev->xexpose.count > 0) break;   /* only the last of a batch */
        _client_t *c = _find_frame(e, ev->xexpose.window);
        if (c) _frame_paint(e, c);
        break;
    }
    case MotionNotify: {
        if (e->in_op) {
            XEvent last = *ev;
            while (XCheckTypedWindowEvent(e->dpy, e->root, MotionNotify, ev))
                last = *ev;
            _op_motion(e, last.xmotion.x_root, last.xmotion.y_root);
        }
        break;
    }
    case ButtonRelease: {
        if (e->in_op) _op_end(e);
        break;
    }
    case KeyPress: {
        XKeyEvent *ke = &ev->xkey;
        unsigned int state = ke->state & (ShiftMask | ControlMask |
                                          Mod1Mask | Mod4Mask);
        for (size_t i = 0; i < e->grabs.size; i++) {
            _grab_t *g = vt_vec_at(&e->grabs, i);
            if ((int)ke->keycode == g->keycode && state == g->mods) {
                vt_wm_shortcut_handle(e->wm, g->combo);
                break;
            }
        }
        break;
    }
    case FocusIn: {
        _client_t *c = _find(e, ev->xfocus.window);
        if (!c) c = _find_frame(e, ev->xfocus.window);
        if (c && !c->model.focused) { c->model.focused = true; _frame_paint(e, c); }
        break;
    }
    case FocusOut: {
        _client_t *c = _find(e, ev->xfocus.window);
        if (!c) c = _find_frame(e, ev->xfocus.window);
        if (c) { c->model.focused = false; _frame_paint(e, c); }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------- engine setup */
struct vt_wm_x11 *vt_wm_x11_new_impl(vt_wm_t *wm) {
    vt_wm_x11_t *e = vt_malloc0(sizeof(*e));
    e->wm = wm;
    e->dpy = vt_x11_display();
    e->root = vt_x11_root();
    e->n_desktops = (int)(wm->workspaces.size > 0 ? wm->workspaces.size : 4);
    e->cur_desktop = 0;
    e->sloppy_focus = false;      /* default: click-to-focus */
    e->snap_enabled = true;
    vt_vec_init(&e->clients, sizeof(_client_t *), 8);
    vt_vec_init(&e->stacking, sizeof(_client_t *), 8);
    vt_vec_init(&e->grabs, sizeof(_grab_t), 8);
    return (struct vt_wm_x11 *)e;
}

static bool _detect_other_wm(vt_wm_x11_t *e) {
    /* Selecting SubstructureRedirect on the root fails with BadAccess if
     * another window manager is running. Install a trapping handler for
     * the probe, then restore the tolerant handler. */
    Display *dpy = e->dpy;
    _probe_bad_access = 0;
    XSetErrorHandler(_probe_err);
    XSelectInput(dpy, e->root, SubstructureRedirectMask | SubstructureNotifyMask
                                 | FocusChangeMask | PropertyChangeMask
                                 | ButtonPressMask | ButtonReleaseMask
                                 | KeyPressMask | EnterWindowMask
                                 | PointerMotionMask | StructureNotifyMask);
    XSync(dpy, False);
    XSetErrorHandler(_x_err);
    return _probe_bad_access != 0;
}

/* ------------------------------------------------------ root cursor */

/* The classic 16x16 arrow as hard-coded bits — the last-resort cursor
 * that needs no cursor font and no Xcursor theme, so the pointer is
 * visible even on a bare Xorg/XLibre with nothing else installed. */
static const char *const _arrow_rows[] = {
    "X...............",
    "XX..............",
    "XOX.............",
    "XOOX............",
    "XOOOX...........",
    "XOOOOX..........",
    "XOOOOOX.........",
    "XOOOOOOX........",
    "XOOOOOOOX.......",
    "XOOOOXXXX.......",
    "XOOXOX..........",
    "XOX.XOX.........",
    "XX...XOX........",
    "X.....XOX.......",
    "......XOX.......",
    ".......X........",
};

static Cursor _cursor_default_init(vt_wm_x11_t *e, bool *set_out) {
    Display *dpy = e->dpy;
    *set_out = false;
    if (!dpy) return None;

    /* 1. Xcursor themed arrow (XCURSOR_THEME / XCURSOR_SIZE aware,
     *    'default' theme fallback) */
#if defined(VT_HAVE_XCURSOR)
    {
        const char *theme = getenv("XCURSOR_THEME");
        const char *szs = getenv("XCURSOR_SIZE");
        int size = szs && *szs ? atoi(szs) : 24;
        if (size <= 0 || size > 128) size = 24;
        const char *t = (theme && *theme) ? theme : "default";
        Cursor c = XcursorLibraryLoadCursor(dpy, "left_ptr");
        if (c == None)
            c = XcursorLibraryLoadCursor(dpy, "arrow");
        if (c != None) {
            *set_out = true;
            vt_logi("wm-x11: root cursor: Xcursor theme '%s' "
                    "(left_ptr)", t);
            (void)size;
            return c;
        }
        vt_logi("wm-x11: root cursor: Xcursor found no 'left_ptr' image "
                "in theme '%s' — trying the cursor font", t);
    }
#endif

    /* 2. classic cursor-font arrow (built into the X server) */
    {
        Cursor c = XCreateFontCursor(dpy, XC_left_ptr);
        if (c != None) {
            *set_out = true;
            vt_logi("wm-x11: root cursor: cursor-font left_ptr");
            return c;
        }
        vt_logw("wm-x11: root cursor: XCreateFontCursor failed — "
                "falling back to the hard-coded arrow");
    }

    /* 3. hand-coded bitmap arrow (works with nothing but core X11) */
    {
        const int n = 16;
        unsigned char bits[(n * n) / 8];
        unsigned char mask[(n * n) / 8];
        memset(bits, 0, sizeof(bits));
        memset(mask, 0, sizeof(mask));
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                char ch = _arrow_rows[y][x];
                if (ch == 'X' || ch == 'O') {
                    int bit = y * n + x;
                    mask[bit / 8] |= (unsigned char)(1u << (bit % 8));
                    if (ch == 'X')
                        bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
                }
            }
        }
        Pixmap src = XCreateBitmapFromData(dpy, e->root,
                                           (const char *)bits, n, n);
        Pixmap msk = XCreateBitmapFromData(dpy, e->root,
                                           (const char *)mask, n, n);
        if (src != None && msk != None) {
            XColor fg = { .red = 0, .green = 0, .blue = 0 };
            XColor bg = { .red = 0xffff, .green = 0xffff, .blue = 0xffff };
            Cursor c = XCreatePixmapCursor(dpy, src, msk, &fg, &bg, 0, 0);
            XFreePixmap(dpy, src);
            XFreePixmap(dpy, msk);
            if (c != None) {
                *set_out = true;
                vt_logi("wm-x11: root cursor: hard-coded 16x16 arrow "
                        "(no font, no theme)");
                return c;
            }
        }
        vt_logw("wm-x11: root cursor: could not create any cursor — the "
                "pointer may be invisible");
    }
    return None;
}

int vt_wm_x11_start(struct vt_wm_x11 *eng) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    if (!e || !e->dpy) return VT_ERR_INVAL;
    Display *dpy = e->dpy;
    const vt_x11_atoms_t *a = vt_x11_atoms();

    if (_detect_other_wm(e)) {
        vt_loge("wm-x11: another window manager is already running on %s",
                DisplayString(dpy));
        return VT_ERR_BUSY;
    }

    e->wmwin = XCreateSimpleWindow(dpy, e->root, -100, -100, 1, 1, 0, 0, 0);
    XSetWindowAttributes sa = { .override_redirect = True };
    XChangeWindowAttributes(dpy, e->wmwin, CWOverrideRedirect, &sa);
    vt_x11_set_net_wm_check(e->root, e->wmwin);

    Atom supported[] = {
        a->net_supported, a->net_client_list, a->net_client_list_stacking,
        a->net_active_window, a->net_current_desktop, a->net_number_of_desktops,
        a->net_desktop_names, a->net_desktop_viewport, a->net_workarea,
        a->net_wm_name, a->net_wm_icon_name, a->net_wm_visible_name,
        a->net_wm_icon, a->net_wm_state, a->net_wm_state_sticky,
        a->net_wm_state_maximized_vert, a->net_wm_state_maximized_horz,
        a->net_wm_state_fullscreen, a->net_wm_state_hidden,
        a->net_wm_state_above, a->net_wm_state_below,
        a->net_wm_state_demands_attention, a->net_wm_state_skip_taskbar,
        a->net_wm_state_skip_pager, a->net_wm_window_type,
        a->net_wm_window_type_normal, a->net_wm_window_type_dock,
        a->net_wm_window_type_desktop, a->net_wm_window_type_toolbar,
        a->net_wm_window_type_menu, a->net_wm_window_type_splash,
        a->net_wm_window_type_dialog, a->net_wm_window_type_utility,
        a->net_wm_window_type_notification, a->net_wm_allowed_actions,
        a->net_wm_action_move, a->net_wm_action_resize,
        a->net_wm_action_minimize, a->net_wm_action_maximize_horz,
        a->net_wm_action_maximize_vert, a->net_wm_action_fullscreen,
        a->net_wm_action_close, a->net_wm_action_above,
        a->net_wm_action_below, a->net_wm_desktop, a->net_wm_pid,
        a->net_wm_window_opacity, a->net_close_window, a->net_wm_moveresize,
        a->net_restack_window, a->net_request_frame_extents,
        a->net_frame_extents, a->net_supporting_wm_check,
        a->net_fullscreen_monitors, a->wm_protocols, a->wm_delete_window,
        a->wm_take_focus, a->wm_state, a->wm_change_state,
    };
    vt_x11_set_net_supported(e->root, supported, VT_ARRAY_SIZE(supported));

    e->cur_move = XCreateFontCursor(dpy, XC_fleur);
    e->cur_resize = XCreateFontCursor(dpy, XC_bottom_right_corner);
    e->cur_default = _cursor_default_init(e, &e->cur_default_set);
    if (e->cur_default_set) {
        /* The root window has no cursor of its own — without an explicit
         * XDefineCursor the server falls back to the parent's, which on
         * a bare Xorg/XLibre with no cursor theme loaded can render as
         * an invisible pointer. Pin a real arrow on the root AND on our
         * WM check window so the pointer is always visible. Frames get
         * the same cursor when they are created. */
        XDefineCursor(dpy, e->root, e->cur_default);
        XDefineCursor(dpy, e->wmwin, e->cur_default);
    }
#if defined(VT_HAVE_XFT)
    e->tfont = XftFontOpenName(dpy, DefaultScreen(dpy), "sans-9:bold");
    if (!e->tfont)
        e->tfont = XftFontOpenName(dpy, DefaultScreen(dpy), "sans-9");
    vt_logi("wm-x11: decoration font %s", e->tfont ? "loaded" : "unavailable");
#endif

    /* register shortcuts as XGrabs */
    for (size_t i = 0; i < e->wm->shortcuts.size; i++) {
        vt_wm_shortcut_t *sc = vt_vec_at(&e->wm->shortcuts, i);
        _grab_t g = { .combo = vt_strdup(sc->combo), .keycode = 0, .mods = 0 };
        if (_parse_combo(dpy, sc->combo, &g.keycode, &g.mods)) {
            vt_vec_push(&e->grabs, &g);
        } else {
            vt_logw("wm-x11: cannot parse shortcut '%s'", sc->combo);
            vt_free(g.combo);
        }
    }
    _grab_all(e);

    /* hook into backend event stream */
    if (e->wm->backend)
        e->sink_id = vt_backend_add_event_sink(e->wm->backend,
                                               _on_backend_event, e);

    _update_workarea(e);
    _update_desktop_props(e);

    /* scan and adopt already-mapped windows */
    Window r, parent, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, e->root, &r, &parent, &children, &nchildren)) {
        for (unsigned i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (children[i] == e->wmwin) continue;
            if (!XGetWindowAttributes(dpy, children[i], &wa)) continue;
            if (wa.override_redirect || wa.map_state != IsViewable) continue;
            _manage(e, children[i]);
        }
        if (children) XFree(children);
    }

    XSync(dpy, False);
    vt_logi("wm-x11: started (%d desktops, workarea %dx%d+%d+%d)",
            e->n_desktops, e->workarea.w, e->workarea.h,
            e->workarea.x, e->workarea.y);
    return VT_OK;
}

void vt_wm_x11_stop(struct vt_wm_x11 *eng) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    if (!e) return;
#if defined(VT_HAVE_XFT)
    if (e->tfont) { XftFontClose(e->dpy, e->tfont); e->tfont = NULL; }
#endif
    if (e->cur_default_set) {
        /* hand the pointer back to the server default (theme unload) */
        XUndefineCursor(e->dpy, e->root);
        XUndefineCursor(e->dpy, e->wmwin);
        XFreeCursor(e->dpy, e->cur_default);
        e->cur_default_set = false;
    }
    if (e->wm->backend && e->sink_id > 0)
        vt_backend_remove_event_sink(e->wm->backend, e->sink_id);
}

void vt_wm_x11_free(struct vt_wm_x11 *eng) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    if (!e) return;
    /* wm->windows holds pointers to models EMBEDDED in the clients about
     * to be freed — clear it first (the models die with the clients). */
    vt_vec_clear(&e->wm->windows);
    for (size_t i = 0; i < e->clients.size; i++) {
        _client_t *c = *(_client_t **)vt_vec_at(&e->clients, i);
        if (_framed(c)) _frame_destroy(e, c, true);
        _client_free(c);
    }
    vt_vec_fini(&e->clients);
    vt_vec_fini(&e->stacking);
    for (size_t i = 0; i < e->grabs.size; i++) {
        _grab_t *g = vt_vec_at(&e->grabs, i);
        vt_free(g->combo);
    }
    vt_vec_fini(&e->grabs);
    vt_free(e);
}

/* -------------------------------------------- engine ops (public API) */
static vt_wm_x11_t *_E(vt_wm_t *wm) {
    if (!wm || !wm->engine) return NULL;
    return (vt_wm_x11_t *)wm->engine;
}

struct vt_wm_x11 *vt_wm_x11_from(vt_wm_t *wm) {
    return (struct vt_wm_x11 *)_E(wm);
}

void vt_wm_x11_focus_id(struct vt_wm_x11 *eng, uint32_t id) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) { if (c->model.minimized) _minimize(e, c, false); _focus(e, c); }
}
void vt_wm_x11_close_id(struct vt_wm_x11 *eng, uint32_t id) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _close(e, c);
}
void vt_wm_x11_minimize_id(struct vt_wm_x11 *eng, uint32_t id, bool on) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _minimize(e, c, on);
}
void vt_wm_x11_maximize_id(struct vt_wm_x11 *eng, uint32_t id, bool on) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _maximize(e, c, on);
}
void vt_wm_x11_fullscreen_id(struct vt_wm_x11 *eng, uint32_t id, bool on) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _fullscreen(e, c, on);
}
void vt_wm_x11_move_id(struct vt_wm_x11 *eng, uint32_t id, int x, int y) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _apply_configure(e, c, x, y, c->model.w, c->model.h);
}
void vt_wm_x11_resize_id(struct vt_wm_x11 *eng, uint32_t id, int w, int h) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _apply_configure(e, c, c->model.x, c->model.y, w, h);
}
void vt_wm_x11_tile_id(struct vt_wm_x11 *eng, uint32_t id, vt_wm_tile_t t) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _tile(e, c, t);
}
void vt_wm_x11_desktop(struct vt_wm_x11 *eng, int d) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    if (e) _set_desktop(e, d);
}
void vt_wm_x11_move_to_desktop_id(struct vt_wm_x11 *eng, uint32_t id, int d) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    if (c) _move_to_desktop(e, c, d);
}
bool vt_wm_x11_is_dock(struct vt_wm_x11 *eng, uint32_t id) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    return c ? (c->is_dock || c->is_desktop) : false;
}

void vt_wm_x11_set_focus_mode(struct vt_wm_x11 *eng, bool sloppy) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    if (!e) return;
    e->sloppy_focus = sloppy;
    vt_logi("wm-x11: focus mode: %s", sloppy ? "sloppy (follows pointer)"
                                             : "click-to-focus");
}
unsigned long vt_wm_x11_opacity(struct vt_wm_x11 *eng, uint32_t id) {
    vt_wm_x11_t *e = (vt_wm_x11_t *)eng;
    _client_t *c = e ? _find(e, (Window)id) : NULL;
    return c ? c->opacity : 0xffffffff;
}

#endif /* VT_HAVE_X11 */
