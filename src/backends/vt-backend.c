/*
 * vt-backend.c — Display backend factory
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Selects and instantiates the appropriate backend based on user config
 * and what is available at runtime. The backend implementations live in
 * vt-backend-wayland.c (native Wayland compositor) and vt-backend-x11.c
 * (client of the running X server, Xorg or XLibre alike).
 *
 * Automatic selection depends only on the display transports present in
 * the environment ($DISPLAY / $WAYLAND_DISPLAY), never on desktop-
 * environment variables. Backend structs are heap-allocated by their
 * constructors so that each process owns exactly one live instance
 * with mutable state.
 */

#define VT_LOG_DOMAIN "backend"
#include <vantage/vt-backend.h>
#include <vantage/vt-x11.h>
#include <string.h>
#include <stdlib.h>

#if !defined(VT_HAVE_X11)
/* defined by the no-X11 stub in vt-backend-x11.c */
const char *vt_x11_server_name(void);
#endif


vt_backend_t *_vt_backend_wayland_new(void);
vt_backend_t *_vt_backend_x11_new(void);

static const char *const _names[] = {
    [VT_BACKEND_AUTO]    = "auto",
    [VT_BACKEND_WAYLAND] = "wayland",
    [VT_BACKEND_X11]     = "x11",
    [VT_BACKEND_HEADLESS]= "headless",
    [VT_BACKEND_INVALID] = "invalid",
};

vt_backend_kind_t vt_backend_kind_from_str(const char *s) {
    if (!s) return VT_BACKEND_AUTO;
    for (int i = 0; i < (int)VT_ARRAY_SIZE(_names); i++)
        if (vt_strcaseeq(_names[i], s)) return (vt_backend_kind_t)i;
    /* legacy spellings: one X11 code path, whatever server runs it */
    if (vt_strcaseeq("xorg", s) || vt_strcaseeq("xlibre", s) ||
        vt_strcaseeq("x11l", s))
        return VT_BACKEND_X11;
    return VT_BACKEND_AUTO;
}
const char *vt_backend_kind_str(vt_backend_kind_t k) {
    return (k >= 0 && k < (int)VT_ARRAY_SIZE(_names)) ? _names[k] : "invalid";
}
const char *vt_backend_name(const vt_backend_t *b) {
    return b ? vt_backend_kind_str(b->kind) : "null";
}

vt_backend_t *vt_backend_new(vt_backend_kind_t preferred) {
    /* Probe order: the requested backend first; for AUTO, prefer the X11
     * server that is already running ($DISPLAY) and otherwise start the
     * native Wayland compositor. Selection never inspects
     * XDG_CURRENT_DESKTOP and friends. */
    vt_backend_t *(*candidates[2])(void) = { NULL, NULL };
    int n = 0;
    switch (preferred) {
    case VT_BACKEND_WAYLAND:
        candidates[n++] = _vt_backend_wayland_new;
        break;
    case VT_BACKEND_X11:
        candidates[n++] = _vt_backend_x11_new;
        break;
    default: {
        const char *dpy = getenv("DISPLAY");
        if (dpy && *dpy) {
            candidates[n++] = _vt_backend_x11_new;
            candidates[n++] = _vt_backend_wayland_new;
        } else {
            candidates[n++] = _vt_backend_wayland_new;
            candidates[n++] = _vt_backend_x11_new;
        }
        break;
    }
    }
    for (int i = 0; i < n; i++) {
        if (!candidates[i]) continue;
        vt_backend_t *b = candidates[i]();
        if (!b) continue;
        if (b->init && b->init(b) == 0)
            return b;
        if (b->fini) b->fini(b);
        vt_free(b);
    }
    /* Fallback: headless — just so the session can still start. */
    vt_backend_t *hb = vt_malloc0(sizeof(*hb));
    hb->kind = VT_BACKEND_HEADLESS;
    vt_vec_init(&hb->outputs, sizeof(vt_output_t), 1);
    vt_vec_init(&hb->inputs, sizeof(vt_input_dev_t), 2);
    vt_vec_init(&hb->sinks, sizeof(vt_backend_sink_t), 2);
    vt_output_t o = { .name = vt_strdup("headless"), .id = 0,
                      .w = 1024, .h = 768, .refresh_hz = 60, .scale = 1,
                      .connected = true, .enabled = true, .primary = true };
    vt_vec_push(&hb->outputs, &o);
    vt_logw("backend: no display backend available; running headless");
    return hb;
}

void vt_backend_free(vt_backend_t *b) {
    if (!b) return;
    for (size_t i = 0; i < b->outputs.size; i++) {
        vt_output_t *o = vt_vec_at(&b->outputs, i);
        vt_free(o->name);
    }
    vt_vec_fini(&b->outputs);
    for (size_t i = 0; i < b->inputs.size; i++) {
        vt_input_dev_t *d = vt_vec_at(&b->inputs, i);
        vt_free(d->name);
        vt_free(d->syspath);
    }
    vt_vec_fini(&b->inputs);
    vt_vec_fini(&b->sinks);
    if (b->fini) b->fini(b);
    vt_free(b);
}

int vt_backend_init(vt_backend_t *b) {
    return (b && b->init) ? b->init(b) : 0;
}
int vt_backend_dispatch(vt_backend_t *b, int timeout_ms) {
    return (b && b->dispatch) ? b->dispatch(b, timeout_ms) : 0;
}
int vt_backend_fd(vt_backend_t *b) {
    return (b && b->fd) ? b->fd(b) : -1;
}
size_t vt_backend_output_count(const vt_backend_t *b) {
    return (b && b->output_count) ? b->output_count((vt_backend_t *)b) : 0;
}
const vt_output_t *vt_backend_output_at(const vt_backend_t *b, size_t i) {
    return (b && b->output_at) ? b->output_at((vt_backend_t *)b, i) : NULL;
}
int vt_backend_output_apply(vt_backend_t *b, size_t i, const vt_output_t *cfg) {
    return (b && b->output_apply) ? b->output_apply(b, i, cfg) : 0;
}

int vt_backend_add_event_sink(vt_backend_t *b, vt_backend_event_fn fn, void *ud) {
    if (!b || !fn) return -VT_ERR_INVAL;
    static int next_id = 1;
    vt_backend_sink_t s = { .id = next_id++, .fn = fn, .ud = ud };
    if (!vt_vec_push(&b->sinks, &s)) return -VT_ERR_NOMEM;
    return s.id;
}

int vt_backend_remove_event_sink(vt_backend_t *b, int sink_id) {
    if (!b || sink_id <= 0) return -VT_ERR_INVAL;
    for (size_t i = 0; i < b->sinks.size; i++) {
        vt_backend_sink_t *s = vt_vec_at(&b->sinks, i);
        if (s->id == sink_id) { vt_vec_remove(&b->sinks, i); return VT_OK; }
    }
    return -VT_ERR_NOENT;
}

void vt_backend_emit_event(vt_backend_t *b, void *event) {
    if (!b || !event) return;
    /* Iterate over a snapshot: a sink may remove itself during dispatch. */
    vt_backend_sink_t snap[16];
    size_t n = b->sinks.size < 16 ? b->sinks.size : 16;
    memcpy(snap, b->sinks.data, n * sizeof(vt_backend_sink_t));
    for (size_t i = 0; i < n; i++)
        snap[i].fn(snap[i].ud, event);
}

void *vt_backend_native(const vt_backend_t *b) {
    /* The X11 backend stores the Display* in priv. */
    return b ? b->priv : NULL;
}

const char *vt_backend_server_implementation(const vt_backend_t *b) {
    if (!b) return NULL;
    switch (b->kind) {
    case VT_BACKEND_X11:
        return vt_x11_server_name();
    case VT_BACKEND_WAYLAND:
        return "Vantage";
    default:
        return NULL;
    }
}
