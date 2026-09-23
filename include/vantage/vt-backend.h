/*
 * vt-backend.h — Display backend abstraction
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vantage supports three native display backends:
 *   - Wayland (native)
 *   - Xorg (native, libxcb/libX11)
 *   - XLibre (native, same X11 code path, modular config)
 *
 * The backend is selected at session startup based on user config and
 * what is available at runtime. The compositor and WM use the same
 * vt_backend_t interface regardless of which is in use.
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
    VT_BACKEND_XORG,
    VT_BACKEND_XLIBRE,
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
