/*
 * vt-backend.h — Display backend abstraction
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Vantage has two display backends:
 *   - Wayland (native compositor on libwayland-server)
 *   - X11 (client of the running X server, via libX11/libxcb)
 *
 * The X11 backend talks to the X11 protocol only. Which X server
 * implementation is underneath — Xorg, XLibre, or anything else that
 * speaks X11 — is an informational detail, queried at runtime for
 * diagnostics (vt_backend_server_implementation()), never a separate
 * Vantage backend.
 *
 * The backend is selected at session startup (vantage-session
 * --wayland | --x11, or automatic detection) and the compositor and WM
 * use the same vt_backend_t interface regardless of which is in use.
 */
#ifndef VANTAGE_BACKEND_H
#define VANTAGE_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_BACKEND_AUTO = 0,
    VT_BACKEND_WAYLAND,
    VT_BACKEND_X11,
    VT_BACKEND_HEADLESS,
    VT_BACKEND_INVALID,
} vt_backend_kind_t;

typedef struct vt_output {
    char      *name;
    int        id;
    int        x, y, w, h;          /* logical geometry */
    int        phys_w_mm, phys_h_mm;
    int        refresh_hz;
    int        subpixel;
    int        scale;                /* 1=normal, 2=HiDPI */
    bool       primary;
    bool       connected;
    bool       enabled;
} vt_output_t;

typedef struct vt_input_dev {
    char      *name;
    char      *syspath;
    int        type;     /* keyboard, pointer, touch */
    int        id;
    bool       active;
} vt_input_dev_t;

/* Wayland backend window lifecycle events, emitted through the
 * backend event-sink mechanism so the WM layer can mirror xdg_shell
 * windows into its backend-agnostic model (vantage-remote list,
 * window-close IPC etc.). */
typedef enum {
    VT_BACKEND_WL_EVENT_NONE = 0,
    VT_BACKEND_WL_EVENT_WIN_MAP,
    VT_BACKEND_WL_EVENT_WIN_UNMAP,
    VT_BACKEND_WL_EVENT_WIN_TITLE,
    VT_BACKEND_WL_EVENT_WIN_STATE,
    VT_BACKEND_WL_EVENT_WIN_FOCUS,
    VT_BACKEND_WL_EVENT_WIN_GEOMETRY,
    VT_BACKEND_WL_EVENT_WORKSPACE,   /* count/cur carried in the event */
} vt_backend_wl_event_kind_t;

typedef struct {
    vt_backend_wl_event_kind_t kind;
    uint64_t    window_id;   /* compositor-assigned, stable */
    const char *title;       /* may be NULL/"" */
    const char *app_id;      /* may be NULL/"" */
    int         x, y, w, h;
    bool        focused, maximized, fullscreen, minimized;
} vt_backend_wl_event_t;

typedef struct vt_backend {
    vt_backend_kind_t kind;
    void  *priv;
    vt_vec_t outputs;       /* vt_output_t */
    vt_vec_t inputs;        /* vt_input_dev_t */
    vt_vec_t sinks;         /* event sinks: _vt_event_sink_t */
    int      (*init)(struct vt_backend *self);
    void     (*fini)(struct vt_backend *self);
    int      (*dispatch)(struct vt_backend *self, int timeout_ms);
    int      (*fd)(struct vt_backend *self);
    size_t   (*output_count)(struct vt_backend *self);
    const vt_output_t *(*output_at)(struct vt_backend *self, size_t i);
    int      (*output_apply)(struct vt_backend *self, size_t idx, const vt_output_t *cfg);
    bool     (*supports_compositing)(struct vt_backend *self);
    bool     (*can_swap_buffers)(struct vt_backend *self);
    void     (*set_user_data)(struct vt_backend *self, void *ud);
    void    *(*get_user_data)(struct vt_backend *self);
    /* Ask the backend to close a native window (xdg_toplevel close on
     * Wayland; X11 goes through its WM engine instead). Optional. */
    int      (*close_window)(struct vt_backend *self, uint64_t window_id);
    /* Optional: compositor-side hotkey dispatch, set by the WM host.
     * Returns true when the combo was consumed (do not forward the key
     * to the focused client). */
    bool     (*hotkey)(struct vt_backend *self, const char *combo);
} vt_backend_t;

/* Native event sink: receives backend-native events (XEvent* on X11,
 * wl_event loop dispatch on Wayland). Lets the WM/compositor layers hook
 * into the backend's event stream without owning the connection. */
typedef void (*vt_backend_event_fn)(void *ud, void *event);

typedef struct {
    int                  id;
    vt_backend_event_fn  fn;
    void                *ud;
} vt_backend_sink_t;

vt_backend_t *vt_backend_new(vt_backend_kind_t preferred);
void          vt_backend_free(vt_backend_t *b);

/* Informational: the display-server implementation the backend is
 * talking to. For the X11 backend this is the server vendor
 * ("Xorg", "XLibre", or the raw vendor string); for the Wayland
 * backend it is "Vantage" (Vantage *is* the compositor). NULL when
 * unknown. This never selects code paths — Xorg and XLibre share the
 * identical X11 code. */
const char   *vt_backend_server_implementation(const vt_backend_t *b);
int           vt_backend_init(vt_backend_t *b);
int           vt_backend_dispatch(vt_backend_t *b, int timeout_ms);
int           vt_backend_fd(vt_backend_t *b);
size_t        vt_backend_output_count(const vt_backend_t *b);
const vt_output_t *vt_backend_output_at(const vt_backend_t *b, size_t i);
int           vt_backend_output_apply(vt_backend_t *b, size_t i,
                                        const vt_output_t *cfg);
const char   *vt_backend_name(const vt_backend_t *b);
const char   *vt_backend_kind_str(vt_backend_kind_t k);
vt_backend_kind_t vt_backend_kind_from_str(const char *s);

/* Event sink registration. Returns sink id >= 0, or -VT_ERR_INVAL. */
int           vt_backend_add_event_sink(vt_backend_t *b,
                                        vt_backend_event_fn fn, void *ud);
int           vt_backend_remove_event_sink(vt_backend_t *b, int sink_id);
void          vt_backend_emit_event(vt_backend_t *b, void *event);

/* Native handle accessor (Display* for X11 backends). NULL otherwise. */
void         *vt_backend_native(const vt_backend_t *b);

#ifdef __cplusplus
}
#endif
#endif
