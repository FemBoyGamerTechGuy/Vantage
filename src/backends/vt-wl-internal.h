/*
 * vt-wl-internal.h — shared types between the Wayland backend core and
 * its protocol modules (layer-shell, Xwayland).
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * vt-backend-wayland.c owns the surface lifetime and the main loop;
 * vt-wl-layer.c implements zwlr_layer_shell_v1 (docked panel surfaces,
 * exclusive zones → the workarea) and vt-wl-xwayland.c implements
 * xwayland_shell_v1 + the Xwayland window management. This header is
 * the narrow seam between them: no public API, build-internal only.
 */
#ifndef VT_WL_INTERNAL_H
#define VT_WL_INTERNAL_H

#include <vantage/vt-backend.h>
#include <vantage/vt-seat.h>
#include <vantage/vt-kms.h>
#include <vantage/vt-wallpaper.h>

#include <wayland-server.h>
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "wlr-layer-shell-protocol.h"
#include "xwayland-shell-protocol.h"

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------ surfaces */

typedef struct _wl_surf {
    struct wl_resource *res;
    struct wl_resource *buf_res;      /* current wl_buffer */
    uint32_t *pixels;                 /* OUR copy of the last commit */
    uint32_t *own;                    /* backing store for pixels */
    size_t   own_cap;                 /* capacity in uint32 units */
    int32_t  w, h;
    /* Committed BUFFER dimensions — the only safe bounds for reading
     * ->pixels. During a pending interactive resize (and any configure
     * the client has not acked with a matching buffer yet) w/h are
     * already the NEW configured size while pixels is still the OLD,
     * smaller buffer: blitting w*h would read past the allocation
     * (heap-buffer-overflow, caught by ASan in the SSD edge-resize
     * harness probe). Paint loops use min(buf, configured). */
    int32_t  buf_w, buf_h;
    int32_t  stride;                  /* row stride in uint32 units */
    int32_t  dx, dy;                  /* attach offset */
    int      x, y;                    /* composited position */
    int      ws;                      /* workspace (all if sticky-ish) */
    bool     mapped;
    bool     minimized;               /* set_minimized: hidden but alive */
    bool     has_pending_xdg;         /* xdg toplevel exists */
    bool     out_entered;             /* wl_surface.enter sent for the
                                       current mapping (clients like
                                       Xwayland's sw path wait for it
                                       before they attach a buffer) */
    bool     attach_pending;         /* wl_surface.attach called since
                                       the last commit (a bare commit
                                       with NO attach keeps the current
                                       buffer — GDK sync commits) */
    struct _xdg_toplevel *toplevel;
    struct _xdg_popup    *popup;      /* xdg_popup role */
    struct wl_resource *xdg_res;     /* the xdg_surface resource (NULL
                                       while the surface has no xdg role;
                                       needed to deliver the configure
                                       batch after layer get_popup) */
    struct wl_list link;              /* stacking (head = bottom) */
    struct wl_list frame_cbs;         /* pending wl_callback */
    /* xdg_surface.set_window_geometry: the window rect inside the
     * buffer (CSD shadows live in the buffer but outside the geometry).
     * SET, never accumulated — accumulating made windows drift across
     * the screen on every resize. */
    int32_t  win_gx, win_gy, win_gw, win_gh;
    bool     have_win_geo;
    int32_t  last_gx, last_gy;        /* previous geometry offset (anchor) */
    /* wl_pointer.set_cursor duties */
    bool     is_cursor;
    int      hotspot_x, hotspot_y;
    /* wl_subsurface duties: children are painted relative to this
     * surface, in their own stacking order (place_above/below) */
    struct _wl_surf *parent;
    struct wl_list  subs;             /* child subsurfaces */
    struct wl_list  sub_link;
    /* server-side decoration requested via xdg-decoration (SSD):
     * drawn by the compositor around this surface */
    bool     ssd;
    struct wl_resource *decor_res;   /* zxdg_toplevel_decoration_v1 */
    /* zwlr_layer_surface_v1 role (docked shell surfaces, e.g. panels) */
    struct _layer_surf *layer;
    /* xwayland_shell_v1 association (X11 windows via Xwayland) */
    struct _xwl_win *xwl;
} _wl_surf_t;

typedef struct _xdg_toplevel {
    struct wl_resource *res;
    _wl_surf_t *surf;
    uint64_t    id;                   /* stable window id for the WM */
    bool maximized, fullscreen, resizing, activated, minimized;
    char *title;
    char *app_id;
    int32_t min_w, min_h, max_w, max_h;   /* xdg size hints; 0 = unset */
} _xdg_toplevel_t;

/* xdg_positioner state (popup placement) — parsed for real, so GTK
 * menus/combo boxes appear where the app asked instead of being
 * dumped centered on the screen at a fixed 320x200. */
typedef struct {
    int32_t  ax, ay, aw, ah;         /* anchor rect (parent coords) */
    int32_t  size_w, size_h;
    uint32_t anchor, gravity;
    int32_t  off_x, off_y;
    uint32_t constraint;
    bool     has_size;
} _xdg_pos_t;

typedef struct _xdg_popup {
    struct wl_resource *res;
    _wl_surf_t *surf;
    _wl_surf_t *parent;              /* anchor parent (surface coords) */
    int32_t rel_x, rel_y;            /* position relative to the parent */
    bool grabbed;                    /* popup grab active (menus) */
    _xdg_pos_t pos;                  /* positioner copy: layer-shell
                                        popups re-place when their parent
                                        is attached later (get_popup) */
} _xdg_popup_t;


typedef struct _cb_node {
    struct wl_list link;
    struct wl_resource *cb;
} _cb_node_t;

/* per-client pointer/keyboard resources */
typedef struct _ptr_res {
    struct wl_list link;
    struct wl_resource *res;          /* wl_pointer resource */
} _ptr_res_t;

typedef struct _kbd_res {
    struct wl_list link;
    struct wl_resource *res;          /* wl_keyboard resource */
} _kbd_res_t;

typedef struct {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_event_source *src;
    struct wl_event_source *seat_src;   /* seat fd source */
    struct wl_event_source *drm_src;   /* drm fd source */
    struct wl_event_source *li_src;    /* libinput fd source */
    char *socket_name;
    struct wl_global *compositor_g;
    struct wl_global *shm_g;
    struct wl_global *seat_g;
    struct wl_global *output_g;
    struct wl_global *xdg_g;
    struct wl_global *subcomp_g;
    struct wl_global *ddm_g;
    struct wl_listener client_created;
    struct wl_list surfaces;          /* bottom→top */
    vt_backend_t *backend_self;       /* for event emission */
    void *user_data;                  /* wm host pointer */
    /* output */
    int out_w, out_h;
    uint32_t *fb;                     /* output framebuffer (XRGB) */
    bool dirty;
    int clients;
    uint64_t frame_count;
    bool headless;                    /* honest marker: no KMS */

    /* workspace state */
    int ws_count, ws_cur;

    /* real session path */
    vt_seat_t *seat;
    vt_kms_t  *kms;
    struct wl_event_source *vt_switch_src;

    /* input */
#if defined(VT_HAVE_LIBINPUT)
    struct libinput *li;
    struct udev *udev;
#endif
    int cursor_x, cursor_y;
    bool buttons[16];                  /* pressed buttons (0-indexed) */
    uint32_t serial;                   /* wayland serial counter */
    /* keyboard */
#if defined(VT_HAVE_XKBCOMMON)
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_km;
    struct xkb_state   *xkb_st;
    int  keymap_fd;
    size_t keymap_size;
#endif
    struct wl_list ptr_reses;          /* _ptr_res_t */
    struct wl_list kbd_reses;          /* _kbd_res_t */
    _wl_surf_t *ptr_focus;             /* surface under cursor */
    _wl_surf_t *kbd_focus;             /* focused surface */
    _xdg_toplevel_t *focused_toplevel;
    uint64_t next_win_id;

    /* clipboard */
    struct wl_list data_devs;        /* _data_dev_t */
    struct _data_src *selection;
    /* desktop background: the SAME wallpaper engine the X11 desktop
     * uses, rendered once into an ARGB buffer (config [wallpaper]) */
    vt_wallpaper_t *wall;
    uint32_t *bg_pix;
    /* xdg-decoration protocol */
    struct wl_global *decor_g;
    /* wlr layer-shell protocol */
    struct wl_global *layer_shell_g;
    /* xwayland support */
    struct wl_global *xwl_shell_g;
    bool xwl_enabled;                 /* Xwayland spawned + managed */
    /* cursor sprite */
    uint32_t cursor_img[64 * 64];
    int cur_img_w, cur_img_h, cur_img_hx, cur_img_hy;
    bool cur_client_set;               /* client provided a cursor */
    _wl_surf_t *cursor_surf;
    /* shape sets: 0=default arrow, 1=E/W resize, 2=N/S, 3=NW/SE, 4=NE/SW.
     * The ACTIVE image stays in cursor_img (the blit/KMS paths do not
     * change); switching shapes copies the set in and re-applies. */
    int cur_shape;                     /* current shape index 0..4 */
    uint32_t cur_shape_img[5][64 * 64];
    int cur_shape_w[5], cur_shape_h[5], cur_shape_hx[5], cur_shape_hy[5];
    uint32_t mods_depressed;           /* current keyboard modifiers */

    /* interactive move/resize (xdg toplevel requests + Super+drag) */
    bool op_active;                    /* interactive op in progress */
    bool op_resize;
    uint8_t op_edges;                  /* grabbed edge bits: 1=E 2=S 4=W 8=N
                                          (same encoding as the X11 WM) */
    _wl_surf_t *op_surf;
    int op_grab_x, op_grab_y;
    int op_start_x, op_start_y;        /* surface origin at grab (edge math) */
    int op_start_w, op_start_h;
    uint64_t op_last_geo_us;           /* geometry-event throttle stamp */
} _wl_state_t;

extern _wl_state_t *_wls;

/* SSD frame metrics + geometry (xdg-decoration server mode) */
#define _WL_SSD_BORDER 2
#define _WL_SSD_TITLE  26
#define _WL_SSD_BTN    22
/* interactive edge-grab margin: the visible 2px border plus a few
 * pixels of the client edge (invisible resize borders overlapping
 * the content, as every mainstream WM does). Enough to grab reliably,
 * thin enough not to eat scrollbar arrows. */
#define _WL_SSD_RESIZE_MARGIN 8

/* geometry broadcast rate during interactive move/resize: the pager
 * must track the drag CONTINUOUSLY (~30 fps), not catch up on release
 * (the reported "pager visibly lags behind the window"). */
#define _WL_GEO_EVENT_INTERVAL_US 33000

/* ------------------------------------------------- backend-core exports */
/* Defined in vt-backend-wayland.c; used by the protocol modules. */

void _emit_win(_wl_state_t *st, vt_backend_wl_event_kind_t kind,
               _xdg_toplevel_t *t);
void _toplevel_configure(struct wl_resource *res, int32_t w, int32_t h,
                         uint32_t state);
void _kbd_enter_focus(_wl_state_t *st, _wl_surf_t *s);
void _popup_done(_wl_state_t *st, _wl_surf_t *s);
void _ssd_frame_geom(const _wl_surf_t *s, int *fx, int *fy,
                     int *fw, int *fh);
void _ssd_paint(_wl_state_t *st, _wl_surf_t *s);
void _paint_background(_wl_state_t *st);
void _focus_top_on_ws(_wl_state_t *st);
void _pointer_focus_update(_wl_state_t *st, bool force);

/* Every window (xdg toplevel or Xwayland) needs a WM-visible id; the
 * Xwayland module creates synthetic toplevels so the taskbar, pager
 * and IPC behave identically for X11 apps. */
_xdg_toplevel_t *_toplevel_new(_wl_state_t *st, _wl_surf_t *s,
                               const char *title, const char *app_id);
void _toplevel_free(_xdg_toplevel_t *t);

/* popup placement math (positioner → relative rect, clamped) */
void _popup_place(const _xdg_pos_t *pos, _wl_surf_t *parent,
                  int out_w, int out_h,
                  int32_t *rx, int32_t *ry, int32_t *rw, int32_t *rh);

/* ------------------------------------------------------ layer-shell API */
/* Implemented in vt-wl-layer.c. */

struct wl_global *_layer_shell_global_create(_wl_state_t *st);
void _layer_shell_global_destroy(_wl_state_t *st);

/* called from _surf_commit: first commit of a layer surface → place,
 * configure and map it. Returns true if handled. */
bool _layer_commit(_wl_surf_t *s);
/* recompute placement after a resize commit */
void _layer_relayout(_wl_surf_t *s);

/* exclusive zones → workarea (called whenever zones change) */
void _layer_workarea(_wl_state_t *st, int *wx, int *wy, int *ww, int *wh);

/* painting: layer surfaces of the TOP/OVERLAY layers draw above
 * toplevels; background/bottom below. Called from _paint(). */

/* stacking: insert a surface at the position its role requires.
 * Order (bottom→top): background/bottom layer surfaces → toplevels
 * → top/overlay layer surfaces → popups. The old code always pushed
 * to the very top, which stacked new windows above docked panels. */
void _stack_insert(_wl_surf_t *s);

/* re-apply stacking order after a layer change (set_layer) */
void _stack_resort(_wl_state_t *st);

/* keyboard: does a layer surface want focus? (mode exclusive/on-demand) */
bool _layer_wants_kbd(const _wl_surf_t *s);
bool _layer_kbd_exclusive(const _wl_surf_t *s);

/* hit-testing: is this surface a docked layer surface? */
static inline bool _surf_is_layer(const _wl_surf_t *s) {
    return s && s->layer != NULL;
}
bool _layer_is_bottom(const _wl_surf_t *s);
bool _layer_is_top(const _wl_surf_t *s);
/* detach a dying surface from its layer role (ownership: the layer
 * resource may outlive the surface) */
void _layer_detach(_wl_surf_t *s);

/* ------------------------------------------------------ Xwayland API */
/* Implemented in vt-wl-xwayland.c. */

bool _xwl_start(_wl_state_t *st);
void _xwl_stop(_wl_state_t *st);
void _xwl_dispatch(void);             /* drain the xcb connection */
int  _xwl_display(void);              /* display number or -1 */
const char *_xwl_auth_file(void);     /* Xauthority path or NULL */

/* events from the backend core into the Xwayland WM */
void _xwl_surface_destroyed(_wl_surf_t *s);   /* surface is dying */
void _xwl_focus_changed(_wl_state_t *st, _wl_surf_t *s);
void _xwl_win_geom(_wl_surf_t *s);
void _xwl_announce_geom(_wl_surf_t *s);   /* first commit: publish real
                                             geometry to the WM model */
void _xwl_move_resize(_wl_surf_t *s, int x, int y, int w, int h);
void _xwl_close(_wl_surf_t *s);
void _xwl_maximize(_wl_surf_t *s, bool on);
void _xwl_fullscreen(_wl_surf_t *s, bool on);
void _xwl_minimize(_wl_surf_t *s, bool on);
void _xwl_set_workspace(_wl_surf_t *s, int ws);
int  _xwl_ws_switch(int ws);
void _xwl_workspace_changed(_wl_state_t *st);

#endif /* VT_WL_INTERNAL_H */
