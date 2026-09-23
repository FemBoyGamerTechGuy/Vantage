/*
 * vt-backend-x11.c — X11 (Xorg / XLibre) display backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * One Xlib Display connection per process, owned by this backend. RandR
 * provides multi-monitor geometry and mode info; XInput2 lists devices.
 * The WM and compositor layers hook into the event stream through the
 * backend event-sink mechanism and share the same connection.
 */

#define VT_LOG_DOMAIN "backend-x11"
#include <vantage/vt-backend.h>
#include <vantage/vt-x11.h>

#if defined(VT_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#if defined(VT_HAVE_XRANDR)
#include <X11/extensions/Xrandr.h>
#endif
#if defined(VT_HAVE_XINPUT)
#include <X11/extensions/XInput2.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

typedef struct {
    Display      *dpy;
    int           screen;
    Window        root;
    bool          randr_present;
    int           randr_event_base;
    int           randr_error_base;
    bool          have_xinput;
} _x11_state_t;

static _x11_state_t *_st = NULL;

Display *vt_x11_display(void) { return _st ? _st->dpy : NULL; }
Window   vt_x11_root(void)    { return _st ? _st->root : None; }
int      vt_x11_screen(void)  { return _st ? _st->screen : 0; }

/* ------------------------------------------------------------ atoms */
static vt_x11_atoms_t _atoms;
static bool _atoms_done = false;

static Atom _a(const char *name) { return XInternAtom(_st->dpy, name, False); }

const vt_x11_atoms_t *vt_x11_atoms(void) {
    if (_atoms_done || !_st) return &_atoms;
    _atoms.net_supported           = _a("_NET_SUPPORTED");
    _atoms.net_client_list         = _a("_NET_CLIENT_LIST");
    _atoms.net_client_list_stacking= _a("_NET_CLIENT_LIST_STACKING");
    _atoms.net_active_window       = _a("_NET_ACTIVE_WINDOW");
    _atoms.net_current_desktop     = _a("_NET_CURRENT_DESKTOP");
    _atoms.net_number_of_desktops  = _a("_NET_NUMBER_OF_DESKTOPS");
    _atoms.net_desktop_names       = _a("_NET_DESKTOP_NAMES");
    _atoms.net_desktop_viewport    = _a("_NET_DESKTOP_VIEWPORT");
    _atoms.net_workarea            = _a("_NET_WORKAREA");
    _atoms.net_wm_name             = _a("_NET_WM_NAME");
    _atoms.net_wm_icon_name        = _a("_NET_WM_ICON_NAME");
    _atoms.net_wm_visible_name     = _a("_NET_WM_VISIBLE_NAME");
    _atoms.net_wm_icon             = _a("_NET_WM_ICON");
    _atoms.net_wm_state            = _a("_NET_WM_STATE");
    _atoms.net_wm_state_modal      = _a("_NET_WM_STATE_MODAL");
    _atoms.net_wm_state_sticky     = _a("_NET_WM_STATE_STICKY");
    _atoms.net_wm_state_maximized_vert = _a("_NET_WM_STATE_MAXIMIZED_VERT");
    _atoms.net_wm_state_maximized_horz = _a("_NET_WM_STATE_MAXIMIZED_HORZ");
    _atoms.net_wm_state_fullscreen = _a("_NET_WM_STATE_FULLSCREEN");
    _atoms.net_wm_state_hidden     = _a("_NET_WM_STATE_HIDDEN");
    _atoms.net_wm_state_above      = _a("_NET_WM_STATE_ABOVE");
    _atoms.net_wm_state_below      = _a("_NET_WM_STATE_BELOW");
    _atoms.net_wm_state_demands_attention = _a("_NET_WM_STATE_DEMANDS_ATTENTION");
    _atoms.net_wm_state_skip_taskbar = _a("_NET_WM_STATE_SKIP_TASKBAR");
    _atoms.net_wm_state_skip_pager  = _a("_NET_WM_STATE_SKIP_PAGER");
    _atoms.net_wm_window_type      = _a("_NET_WM_WINDOW_TYPE");
    _atoms.net_wm_window_type_normal = _a("_NET_WM_WINDOW_TYPE_NORMAL");
    _atoms.net_wm_window_type_dock  = _a("_NET_WM_WINDOW_TYPE_DOCK");
    _atoms.net_wm_window_type_desktop = _a("_NET_WM_WINDOW_TYPE_DESKTOP");
    _atoms.net_wm_window_type_toolbar = _a("_NET_WM_WINDOW_TYPE_TOOLBAR");
    _atoms.net_wm_window_type_menu = _a("_NET_WM_WINDOW_TYPE_MENU");
    _atoms.net_wm_window_type_splash = _a("_NET_WM_WINDOW_TYPE_SPLASH");
    _atoms.net_wm_window_type_dialog = _a("_NET_WM_WINDOW_TYPE_DIALOG");
    _atoms.net_wm_window_type_utility = _a("_NET_WM_WINDOW_TYPE_UTILITY");
    _atoms.net_wm_window_type_notification = _a("_NET_WM_WINDOW_TYPE_NOTIFICATION");
    _atoms.net_wm_allowed_actions = _a("_NET_WM_ALLOWED_ACTIONS");
    _atoms.net_wm_action_move     = _a("_NET_WM_ACTION_MOVE");
    _atoms.net_wm_action_resize   = _a("_NET_WM_ACTION_RESIZE");
    _atoms.net_wm_action_minimize = _a("_NET_WM_ACTION_MINIMIZE");
    _atoms.net_wm_action_maximize_horz = _a("_NET_WM_ACTION_MAXIMIZE_HORZ");
    _atoms.net_wm_action_maximize_vert = _a("_NET_WM_ACTION_MAXIMIZE_VERT");
    _atoms.net_wm_action_fullscreen = _a("_NET_WM_ACTION_FULLSCREEN");
    _atoms.net_wm_action_close    = _a("_NET_WM_ACTION_CLOSE");
    _atoms.net_wm_action_above    = _a("_NET_WM_ACTION_ABOVE");
    _atoms.net_wm_action_below    = _a("_NET_WM_ACTION_BELOW");
    _atoms.net_wm_desktop         = _a("_NET_WM_DESKTOP");
    _atoms.net_wm_pid             = _a("_NET_WM_PID");
    _atoms.net_wm_window_opacity  = _a("_NET_WM_WINDOW_OPACITY");
    _atoms.net_wm_cm              = _a("_NET_WM_CM_S0");
    _atoms.net_close_window       = _a("_NET_CLOSE_WINDOW");
    _atoms.net_wm_moveresize      = _a("_NET_WM_MOVERESIZE");
    _atoms.net_restack_window     = _a("_NET_RESTACK_WINDOW");
    _atoms.net_request_frame_extents = _a("_NET_REQUEST_FRAME_EXTENTS");
    _atoms.net_frame_extents      = _a("_NET_FRAME_EXTENTS");
    _atoms.net_system_tray        = _a("_NET_SYSTEM_TRAY_S0");
    _atoms.net_supporting_wm_check = _a("_NET_SUPPORTING_WM_CHECK");
    _atoms.net_fullscreen_monitors = _a("_NET_FULLSCREEN_MONITORS");
    _atoms.wm_protocols           = _a("WM_PROTOCOLS");
    _atoms.wm_delete_window       = _a("WM_DELETE_WINDOW");
    _atoms.wm_take_focus          = _a("WM_TAKE_FOCUS");
    _atoms.wm_state               = _a("WM_STATE");
    _atoms.wm_change_state        = _a("WM_CHANGE_STATE");
    _atoms.wm_client_leader       = _a("WM_CLIENT_LEADER");
    _atoms.utf8_string            = _a("UTF8_STRING");
    _atoms.cardinal               = _a("CARDINAL");
    _atoms.atom_atom              = _a("ATOM");
    _atoms.string                 = _a("STRING");
    _atoms.pixmap_atom            = _a("PIXMAP");
    _atoms.wm_class_atom          = _a("WM_CLASS");
    _atoms_done = true;
    return &_atoms;
}

/* ---------------------------------------------------- property helpers */
char *vt_x11_get_utf8_property(Window w, Atom prop) {
    if (!_st) return NULL;
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    if (XGetWindowProperty(_st->dpy, w, prop, 0, 64 * 1024, False,
                           _atoms.utf8_string, &actual, &fmt, &n, &bytes,
                           &data) != Success)
        return NULL;
    if (!data) return NULL;
    char *out = vt_strndup((const char *)data, n);
    XFree(data);
    return out;
}

bool vt_x11_set_utf8_property(Window w, Atom prop, const char *val) {
    if (!_st || !val) return false;
    XChangeProperty(_st->dpy, w, prop, _atoms.utf8_string, 8,
                    PropModeReplace, (const unsigned char *)val,
                    (int)strlen(val));
    return true;
}

bool vt_x11_get_cardinal_property(Window w, Atom prop, unsigned long *out) {
    if (!_st || !out) return false;
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    if (XGetWindowProperty(_st->dpy, w, prop, 0, 4, False,
                           _atoms.cardinal, &actual, &fmt, &n, &bytes,
                           &data) != Success)
        return false;
    if (!data || n < 1) { if (data) XFree(data); return false; }
    *out = *(unsigned long *)(void *)data;
    XFree(data);
    return true;
}

bool vt_x11_get_window_property(Window w, Atom prop, Atom type,
                                unsigned char **out_data,
                                unsigned long *out_nitems) {
    if (!_st || !out_data || !out_nitems) return false;
    Atom actual;
    int fmt;
    unsigned long bytes;
    *out_data = NULL;
    if (XGetWindowProperty(_st->dpy, w, prop, 0, 64 * 1024, False, type,
                           &actual, &fmt, out_nitems, &bytes,
                           out_data) != Success)
        return false;
    return *out_data != NULL;
}

bool vt_x11_has_state(Window w, Atom state) {
    unsigned char *data = NULL;
    unsigned long n = 0;
    if (!vt_x11_get_window_property(w, _atoms.net_wm_state, _atoms.atom_atom,
                                    &data, &n))
        return false;
    bool found = false;
    Atom *atoms = (Atom *)(void *)data;
    for (unsigned long i = 0; i < n; i++)
        if (atoms[i] == state) { found = true; break; }
    XFree(data);
    return found;
}

bool vt_x11_motif_decorations_off(Window w) {
    Atom motif = XInternAtom(_st->dpy, "_MOTIF_WM_HINTS", False);
    Atom actual;
    int fmt;
    unsigned long n, bytes;
    unsigned char *data = NULL;
    if (XGetWindowProperty(_st->dpy, w, motif, 0, 20, False, AnyPropertyType,
                           &actual, &fmt, &n, &bytes, &data) != Success)
        return false;
    if (!data || n < 3) { if (data) XFree(data); return false; }
    /* hints.flags & 2 (decorations) and hints.decorations == 0 → off */
    unsigned long *h = (unsigned long *)(void *)data;
    bool off = (h[0] & 2) && (h[2] == 0);
    XFree(data);
    return off;
}

void vt_x11_set_net_supported(Window root, const Atom *list, size_t n) {
    XChangeProperty(_st->dpy, root, _atoms.net_supported, _atoms.atom_atom, 32,
                    PropModeReplace, (const unsigned char *)list, (int)n);
}

void vt_x11_set_net_wm_check(Window root, Window wmwin) {
    XChangeProperty(_st->dpy, root, _atoms.net_supporting_wm_check,
                    _atoms.cardinal, 32, PropModeReplace,
                    (const unsigned char *)&wmwin, 1);
    XChangeProperty(_st->dpy, wmwin, _atoms.net_supporting_wm_check,
                    _atoms.cardinal, 32, PropModeReplace,
                    (const unsigned char *)&wmwin, 1);
    vt_x11_set_utf8_property(wmwin, _atoms.net_wm_name, "Vantage");
}

/* ------------------------------------------------------------ backend */
static void _x11_populate_outputs(vt_backend_t *self) {
    _x11_state_t *st = self->priv;
    Display *dpy = st->dpy;

    for (size_t i = 0; i < self->outputs.size; i++) {
        vt_output_t *o = vt_vec_at(&self->outputs, i);
        vt_free(o->name);
    }
    vt_vec_clear(&self->outputs);

#if defined(VT_HAVE_XRANDR)
    if (st->randr_present) {
        XRRScreenResources *res = XRRGetScreenResources(dpy, st->root);
        if (res) {
            Window primary = XRRGetOutputPrimary(dpy, st->root);
            int idx = 0;
            for (int c = 0; c < res->ncrtc; c++) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[c]);
                if (!ci || ci->width == 0 || ci->width == 0 || ci->noutput == 0) {
                    if (ci) XRRFreeCrtcInfo(ci);
                    continue;
                }
                for (int o = 0; o < ci->noutput; o++) {
                    XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, ci->outputs[o]);
                    if (!oi) continue;
                    if (oi->connection != RR_Connected) { XRRFreeOutputInfo(oi); continue; }
                    vt_output_t vo = {0};
                    vo.id = idx;
                    vo.name = vt_strdup(oi->name ? oi->name : "output");
                    vo.x = ci->x; vo.y = ci->y;
                    vo.w = (int)ci->width; vo.h = (int)ci->height;
                    vo.phys_w_mm = (int)oi->mm_width;
                    vo.phys_h_mm = (int)oi->mm_height;
                    vo.refresh_hz = 0;
                    for (int m = 0; m < res->nmode; m++) {
                        if (res->modes[m].id == ci->mode) {
                            XRRModeInfo *mi = &res->modes[m];
                            vo.refresh_hz = mi->dotClock && mi->hTotal && mi->vTotal
                                ? (int)(mi->dotClock / (unsigned long long)mi->hTotal / mi->vTotal)
                                : 60;
                            break;
                        }
                    }
                    if (vo.refresh_hz == 0) vo.refresh_hz = 60;
                    vo.primary = (ci->outputs[o] == primary);
                    vo.connected = true;
                    vo.enabled = true;
                    vo.scale = 1;
                    vt_vec_push(&self->outputs, &vo);
                    idx++;
                    XRRFreeOutputInfo(oi);
                }
                XRRFreeCrtcInfo(ci);
            }
            XRRFreeScreenResources(res);
        }
    }
#endif
    if (self->outputs.size == 0) {
        /* No RandR or no outputs — one output covering the whole screen. */
        vt_output_t o = {0};
        o.name = vt_strdup("default");
        o.id = 0;
        o.w = DisplayWidth(dpy, st->screen);
        o.h = DisplayHeight(dpy, st->screen);
        o.refresh_hz = 60;
        o.scale = 1;
        o.connected = true;
        o.enabled = true;
        o.primary = true;
        vt_vec_push(&self->outputs, &o);
    }
    /* Mark the first output primary if none was. */
    bool any_primary = false;
    for (size_t i = 0; i < self->outputs.size; i++) {
        vt_output_t *o = vt_vec_at(&self->outputs, i);
        if (o->primary) { any_primary = true; break; }
    }
    if (!any_primary && self->outputs.size > 0) {
        vt_output_t *o = vt_vec_at(&self->outputs, 0);
        o->primary = true;
    }
}

static int _x11_init(vt_backend_t *self) {
    if (_st) { /* already initialized in this process */
        self->priv = _st;
        _x11_populate_outputs(self);
        return 0;
    }
    _x11_state_t *st = vt_malloc0(sizeof(*st));
    self->priv = st;

    const char *dpy_name = getenv("DISPLAY");
    if (!dpy_name || !*dpy_name) {
        vt_logd("x11: DISPLAY not set");
        vt_free(st);
        self->priv = NULL;
        return -1;
    }
    st->dpy = XOpenDisplay(NULL);
    if (!st->dpy) {
        vt_logd("x11: cannot open display %s", dpy_name);
        vt_free(st);
        self->priv = NULL;
        return -1;
    }
    XSynchronize(st->dpy, False);
    st->screen = DefaultScreen(st->dpy);
    st->root = RootWindow(st->dpy, st->screen);

#if defined(VT_HAVE_XRANDR)
    int major = 0, minor = 0;
    st->randr_present = XRRQueryExtension(st->dpy, &st->randr_event_base,
                                          &st->randr_error_base)
                        && XRRQueryVersion(st->dpy, &major, &minor)
                        && major >= 1;
    if (st->randr_present)
        XRRSelectInput(st->dpy, st->root, RRScreenChangeNotifyMask
                                          | RRCrtcChangeNotifyMask
                                          | RROutputChangeNotifyMask);
#endif

#if defined(VT_HAVE_XINPUT)
    int xi_opcode = 0, xi_event = 0, xi_error = 0;
    st->have_xinput = XQueryExtension(st->dpy, "XInputExtension",
                                      &xi_opcode, &xi_event, &xi_error);
    if (st->have_xinput) {
        int xmaj = 2, xmin = 0;
        if (XIQueryVersion(st->dpy, &xmaj, &xmin) != Success)
            st->have_xinput = false;
        (void)xi_event; (void)xi_error;
    }
#endif

    _st = st;
    vt_x11_atoms(); /* initialize atom cache */

    _x11_populate_outputs(self);

    /* Input device list via XInput2 when available. */
    self->inputs.size = 0;
#if defined(VT_HAVE_XINPUT)
    int ndev = 0;
    XIDeviceInfo *devs = XIQueryDevice(st->dpy, XIAllMasterDevices, &ndev);
    if (devs) {
        for (int i = 0; i < ndev; i++) {
            vt_input_dev_t d = {0};
            d.name = vt_strdup(devs[i].name ? devs[i].name : "device");
            d.id = devs[i].deviceid;
            d.type = (devs[i].use == XIMasterKeyboard) ? 0
                   : (devs[i].use == XIMasterPointer) ? 1 : 2;
            d.active = true;
            vt_vec_push(&self->inputs, &d);
        }
        XIFreeDeviceInfo(devs);
    }
#endif
    if (self->inputs.size == 0) {
        vt_input_dev_t k = { .name = vt_strdup("core keyboard"), .id = 0,
                             .type = 0, .active = true };
        vt_input_dev_t p = { .name = vt_strdup("core pointer"), .id = 1,
                             .type = 1, .active = true };
        vt_vec_push(&self->inputs, &k);
        vt_vec_push(&self->inputs, &p);
    }

    vt_logi("x11: connected to %s (screen %d, %zu output%s, randr=%d)",
            dpy_name, st->screen, self->outputs.size,
            self->outputs.size == 1 ? "" : "s", st->randr_present);
    return 0;
}

static void _x11_fini(vt_backend_t *self) {
    if (!self->priv) return;
    if (self->priv == _st) {
        XCloseDisplay(_st->dpy);
        vt_free(_st);
        _st = NULL;
        _atoms_done = false;
    }
    self->priv = NULL;
}

static int _x11_dispatch(vt_backend_t *self, int timeout_ms) {
    _x11_state_t *st = self->priv;
    if (!st || !st->dpy) return -1;
    if (timeout_ms > 0 && !XPending(st->dpy)) {
        struct pollfd pfd = { .fd = ConnectionNumber(st->dpy), .events = POLLIN };
        poll(&pfd, 1, timeout_ms);
    }
    int n = 0;
    XEvent ev;
    while (XPending(st->dpy)) {
        XNextEvent(st->dpy, &ev);
        n++;
#if defined(VT_HAVE_XRANDR)
        if (st->randr_present && ev.type == st->randr_event_base + RRScreenChangeNotify) {
            _x11_populate_outputs(self);
        }
#endif
        vt_backend_emit_event(self, &ev);
    }
    XFlush(st->dpy);
    return n;
}

static int _x11_fd(vt_backend_t *self) {
    _x11_state_t *st = self->priv;
    return (st && st->dpy) ? ConnectionNumber(st->dpy) : -1;
}
static size_t _x11_output_count(vt_backend_t *self) {
    return self->outputs.size;
}
static const vt_output_t *_x11_output_at(vt_backend_t *self, size_t i) {
    return i < self->outputs.size ? vt_vec_at(&self->outputs, i) : NULL;
}
static int _x11_output_apply(vt_backend_t *self, size_t i, const vt_output_t *cfg) {
    _x11_state_t *st = self->priv;
    if (!st || i >= self->outputs.size) return -1;
    vt_output_t *o = vt_vec_at(&self->outputs, i);
    o->enabled = cfg->enabled;
    o->scale = cfg->scale;
#if defined(VT_HAVE_XRANDR)
    /* Apply position via RandR: find the crtc that owns this output. */
    XRRScreenResources *res = XRRGetScreenResources(st->dpy, st->root);
    if (!res) return -1;
    int idx = 0, rc = -1;
    for (int c = 0; c < res->ncrtc && rc < 0; c++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(st->dpy, res, res->crtcs[c]);
        if (!ci) continue;
        for (int oo = 0; oo < ci->noutput; oo++) {
            if (idx == (int)i) {
                if (!cfg->enabled) {
                    XRRSetCrtcConfig(st->dpy, res, res->crtcs[c], CurrentTime,
                                     0, 0, None, RR_Rotate_0, NULL, 0);
                } else {
                    XRRSetCrtcConfig(st->dpy, res, res->crtcs[c], CurrentTime,
                                     cfg->x, cfg->y, ci->mode, ci->rotation,
                                     ci->outputs, ci->noutput);
                }
                rc = 0;
                break;
            }
            idx++;
        }
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
    return rc;
#else
    (void)st;
    return 0;
#endif
}
static bool _x11_supports_compositing(vt_backend_t *self) {
    (void)self;
    return true; /* via Composite extension */
}
static bool _x11_can_swap_buffers(vt_backend_t *self) {
    (void)self;
    return true;
}

vt_backend_t *_vt_backend_x11_new(void) {
    vt_backend_t *b = vt_malloc0(sizeof(*b));
    b->kind = VT_BACKEND_XORG;
    b->init = _x11_init;
    b->fini = _x11_fini;
    b->dispatch = _x11_dispatch;
    b->fd = _x11_fd;
    b->output_count = _x11_output_count;
    b->output_at = _x11_output_at;
    b->output_apply = _x11_output_apply;
    b->supports_compositing = _x11_supports_compositing;
    b->can_swap_buffers = _x11_can_swap_buffers;
    vt_vec_init(&b->outputs, sizeof(vt_output_t), 2);
    vt_vec_init(&b->inputs, sizeof(vt_input_dev_t), 2);
    vt_vec_init(&b->sinks, sizeof(vt_backend_sink_t), 2);
    return b;
}

#else /* !VT_HAVE_X11 */

vt_backend_t *_vt_backend_x11_new(void) {
    vt_logw("x11: built without X11 support");
    return NULL;
}

#endif
