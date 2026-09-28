/*
 * vt-wl-layer.c — zwlr_layer_shell_unstable_v1 (v4) server side
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Docked shell surfaces (panels, docks, notification overlays). This
 * is what lets the Vantage panel be a REAL Wayland client — the same
 * gtk4-layer-shell path every independent panel (waybar, nwg-panel,
 * wf-panel) uses — instead of code inside the compositor.
 *
 * Protocol shape:
 *   zwlr_layer_shell_v1.get_layer_surface(surface, output, layer, ns)
 *     → zwlr_layer_surface_v1 with set_size / set_anchor / set_margin /
 *       set_exclusive_zone / set_keyboard_mode / get_popup / set_layer
 *       requests and configure/closed events.
 *
 * Semantics implemented here:
 *   - anchoring on any edge combination + margins
 *   - anchored opposite edges → stretched across the output
 *   - exclusive zones shrink the WORKAREA: maximized/fullscreen
 *     windows and new-window centering all respect them
 *   - stacking layers: background/bottom below toplevels,
 *     top/overlay above them, popups above everything
 *   - keyboard interactivity: none / exclusive (focus pinned, used by
 *     lock screens) / on-demand (click-to-focus, used by panels)
 *   - get_popup: xdg_popup attached to a layer surface (GTK menus on
 *     layer-shell windows need exactly this)
 *
 * The XML is vendored from wlr-protocols (MIT, copyright Drew DeVault
 * 2017, notice kept in protocols/wlr-layer-shell-unstable-v1.xml).
 */

#define VT_LOG_DOMAIN "backend-wayland"
#include "vt-wl-internal.h"

#if defined(VT_HAVE_WAYLAND)

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ state */

typedef struct _layer_surf {
    struct wl_resource *res;          /* zwlr_layer_surface_v1 */
    _wl_surf_t *surf;
    uint32_t layer;                   /* enum zwlr_layer_shell_v1_layer */
    uint32_t anchors;                 /* bit set of edges */
    int32_t margins[4];               /* left right top bottom */
    int32_t excl_zone;                /* -1 = auto (window size) */
    uint32_t kbd_mode;                /* none / exclusive / on_demand */
    /* configure handshake */
    bool configured;                  /* configure sent at least once */
    bool acked;                       /* client acked the last configure */
    bool ever_acked;                  /* any configure acked — the first
                                       * commit is valid once this is set
                                       * (gtk4-layer-shell may issue a
                                       * set_size BETWEEN our configure and
                                       * the client's first commit, which
                                       * re-arms `acked`; dropping the
                                       * commit over that starves the
                                       * surface forever) */
    uint32_t conf_serial;             /* serial of the pending configure */
    int32_t conf_w, conf_h;           /* size we told the client */
    bool closed;
    bool size_mismatch;               /* edge trigger for reconfigures */
    char *namespace;
} _layer_surf_t;

/* anchor bits (match the protocol enum values) */
#define _A_TOP    1
#define _A_BOTTOM 2
#define _A_LEFT   4
#define _A_RIGHT  8

static void _layer_send_configure(_layer_surf_t *ls) {
    _wl_state_t *st = _wls;
    if (!st || !ls || !ls->res) return;
    /* size: anchored opposite edges → fill the output on that axis;
     * otherwise the client's requested size (0 → its natural size) */
    int32_t w = ls->conf_w, h = ls->conf_h;
    bool fill_w = (ls->anchors & (_A_LEFT | _A_RIGHT)) ==
                  (_A_LEFT | _A_RIGHT);
    bool fill_h = (ls->anchors & (_A_TOP | _A_BOTTOM)) ==
                  (_A_TOP | _A_BOTTOM);
    if (fill_w) {
        w = st->out_w - ls->margins[0] - ls->margins[1];
        if (w < 1) w = 1;
    }
    if (fill_h) {
        h = st->out_h - ls->margins[2] - ls->margins[3];
        if (h < 1) h = 1;
    }
    ls->conf_serial = ++st->serial;
    zwlr_layer_surface_v1_send_configure(ls->res, ls->conf_serial,
                                         (uint32_t)w, (uint32_t)h);
    ls->conf_w = w;
    ls->conf_h = h;
    ls->configured = true;
    ls->acked = false;
}

/* placement from anchors + margins (called on map and on relayout) */
static void _layer_place(_layer_surf_t *ls) {
    _wl_state_t *st = _wls;
    _wl_surf_t *s = ls->surf;
    if (!st || !s) return;
    bool top = ls->anchors & _A_TOP, bot = ls->anchors & _A_BOTTOM;
    bool left = ls->anchors & _A_LEFT, right = ls->anchors & _A_RIGHT;
    if (top && !bot)       s->y = ls->margins[2];
    else if (bot && !top)  s->y = st->out_h - s->h - ls->margins[3];
    else if (top && bot)   s->y = ls->margins[2];
    else                   s->y = (st->out_h - s->h) / 2;
    if (left && !right)    s->x = ls->margins[0];
    else if (right && !left) s->x = st->out_w - s->w - ls->margins[1];
    else if (left && right) s->x = ls->margins[0];
    else                   s->x = (st->out_w - s->w) / 2;
    if (s->x < 0) s->x = 0;
    if (s->y < 0) s->y = 0;
}

/* effective exclusive zone of a layer surface */
static int _layer_excl(_layer_surf_t *ls) {
    if (!ls || ls->closed || !ls->surf || !ls->surf->mapped) return 0;
    if (ls->excl_zone > 0) return ls->excl_zone;
    if (ls->excl_zone == 0) return 0;
    /* -1 = auto: window size + margin on the anchored axis */
    _wl_surf_t *s = ls->surf;
    if ((ls->anchors & (_A_TOP | _A_BOTTOM)) == (_A_TOP | _A_BOTTOM))
        return s->h + ls->margins[2] + ls->margins[3];
    if (ls->anchors & _A_TOP)  return s->h + ls->margins[2];
    if (ls->anchors & _A_BOTTOM) return s->h + ls->margins[3];
    if ((ls->anchors & (_A_LEFT | _A_RIGHT)) == (_A_LEFT | _A_RIGHT))
        return s->w + ls->margins[0] + ls->margins[1];
    if (ls->anchors & _A_LEFT)  return s->w + ls->margins[0];
    if (ls->anchors & _A_RIGHT) return s->w + ls->margins[1];
    return 0;
}

void _layer_detach(_wl_surf_t *s) {
    if (s && s->layer) {
        s->layer->surf = NULL;
        s->layer = NULL;
    }
}

bool _layer_is_bottom(const _wl_surf_t *s) {
    return s && s->layer &&
        (s->layer->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
         s->layer->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
}

bool _layer_is_top(const _wl_surf_t *s) {
    return s && s->layer &&
        (s->layer->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
         s->layer->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

bool _layer_wants_kbd(const _wl_surf_t *s) {
    return s && s->layer &&
           (s->layer->kbd_mode ==
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
            s->layer->kbd_mode ==
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
}

bool _layer_kbd_exclusive(const _wl_surf_t *s) {
    return s && s->layer &&
           s->layer->kbd_mode ==
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
           s->layer->surf && s->layer->surf->mapped;
}

void _layer_workarea(_wl_state_t *st, int *wx, int *wy, int *ww, int *wh) {
    int top = 0, bottom = 0, left = 0, right = 0;
    if (st) {
        _wl_surf_t *s;
        wl_list_for_each(s, &st->surfaces, link) {
            if (!s->layer || !s->mapped) continue;
            _layer_surf_t *ls = s->layer;
            int e = _layer_excl(ls);
            if (e <= 0) continue;
            /* only TOP/OVERLAY layers reserve space by default
             * (background/bottom panels do not push windows away) */
            if (ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
                ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY)
                continue;
            /* STRETCH anchors do not reserve space: a full-width top
             * panel anchors LEFT|RIGHT|TOP — the LEFT/RIGHT anchors
             * only stretch it across the screen. Counting them as side
             * reservations shrank the workarea by the panel height on
             * the left and right (maximized windows got bogus margins
             * and everything the workarea feeds — placement clamps,
             * maximize, tiling — was skewed). An edge reserves space
             * only when the surface does NOT span the opposite axis. */
            bool spans_h = (ls->anchors & _A_LEFT) && (ls->anchors & _A_RIGHT);
            bool spans_v = (ls->anchors & _A_TOP) && (ls->anchors & _A_BOTTOM);
            if (!spans_v) {
                if (ls->anchors & _A_TOP)    top = (e > top) ? e : top;
                if (ls->anchors & _A_BOTTOM) bottom = (e > bottom) ? e : bottom;
            }
            if (!spans_h) {
                if (ls->anchors & _A_LEFT)  left = (e > left) ? e : left;
                if (ls->anchors & _A_RIGHT) right = (e > right) ? e : right;
            }
        }
    }
    if (wx) *wx = left;
    if (wy) *wy = top;
    if (ww) *ww = (st ? st->out_w : 1024) - left - right;
    if (wh) *wh = (st ? st->out_h : 768) - top - bottom;
}

/* -------------------------------------------------------- requests */

static void _ls_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (ls && ls->surf) ls->surf->layer = NULL;
    wl_resource_destroy(res);
}

static void _ls_res_destroy(struct wl_resource *res) {
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    if (ls->surf) {
        ls->surf->layer = NULL;
        /* unmapping a layer surface frees the space it reserved; the
         * generic surface destroy may already have removed it (client
         * death) — mapped was cleared there too */
        if (ls->surf->mapped) {
            ls->surf->mapped = false;
            wl_list_remove(&ls->surf->link);
        }
        if (_wls) _wls->dirty = true;
    }
    if (ls->namespace) vt_free(ls->namespace);
    vt_free(ls);
}

static void _ls_set_size(struct wl_client *cli, struct wl_resource *res,
                         uint32_t w, uint32_t h) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    ls->conf_w = (int32_t)w;
    ls->conf_h = (int32_t)h;
    /* state changed before the first commit: the pending configure
     * must reflect it (the initial configure went out before the client
     * could say how big it wants to be) */
    if (ls->surf && !ls->surf->mapped)
        _layer_send_configure(ls);
}

static void _ls_set_anchor(struct wl_client *cli, struct wl_resource *res,
                           uint32_t anchor) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    ls->anchors = anchor & 0xF;
    if (ls->surf && !ls->surf->mapped)
        _layer_send_configure(ls);
}

static void _ls_set_margin(struct wl_client *cli, struct wl_resource *res,
                           int32_t top, int32_t right, int32_t bottom,
                           int32_t left) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    ls->margins[0] = left;
    ls->margins[1] = right;
    ls->margins[2] = top;
    ls->margins[3] = bottom;
    if (ls->surf && !ls->surf->mapped)
        _layer_send_configure(ls);
}

static void _ls_set_exclusive_zone(struct wl_client *cli,
                                   struct wl_resource *res,
                                   int32_t zone) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    ls->excl_zone = zone;
    if (_wls) _wls->dirty = true;
}

static void _ls_set_kbd_mode(struct wl_client *cli, struct wl_resource *res,
                             uint32_t mode) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    if (mode > ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND)
        mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    ls->kbd_mode = mode;
}

static void _ls_set_layer(struct wl_client *cli, struct wl_resource *res,
                          uint32_t layer) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(res,
            ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
            "invalid layer %u", layer);
        return;
    }
    ls->layer = layer;
    if (ls->surf && ls->surf->mapped && _wls) {
        _stack_resort(_wls);
        _wls->dirty = true;
    }
}

static void _ls_get_popup(struct wl_client *cli, struct wl_resource *res,
                          struct wl_resource *popup) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    _xdg_popup_t *p = popup ? wl_resource_get_user_data(popup) : NULL;
    if (!ls || !p || !ls->surf) {
        wl_resource_post_error(res,
            ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
            "get_popup requires a live popup");
        return;
    }
    /* attach the popup to this layer surface (the gtk4-layer-shell
     * path: xdg_surface.get_popup(NULL parent) + layer get_popup) and
     * re-place it with its stored positioner, now that the parent is
     * known */
    p->parent = ls->surf;
    int32_t rx, ry, rw, rh;
    _popup_place(&p->pos, ls->surf, _wls ? _wls->out_w : 1024,
                 _wls ? _wls->out_h : 768, &rx, &ry, &rw, &rh);
    p->rel_x = rx;
    p->rel_y = ry;
    if (p->res)
        xdg_popup_send_configure(p->res, rx, ry, rw, rh);
    /* xdg-shell: a popup configure batch MUST end with an
     * xdg_surface.configure carrying a serial the client acks before
     * committing. Without it GTK never sees a "commit-ready" serial
     * after the popup configure, decides the popup is broken and
     * destroys it — the start menu then remap-loops forever and never
     * renders. The NULL-parent get_popup path already sent one
     * configure, but the position was unknown then; this is the real
     * one, sent to the POPUP's own xdg_surface. */
    if (p->surf && p->surf->xdg_res && _wls)
        xdg_surface_send_configure(p->surf->xdg_res, ++_wls->serial);
}

static void _ls_ack_configure(struct wl_client *cli,
                              struct wl_resource *res,
                              uint32_t serial) {
    (void)cli;
    _layer_surf_t *ls = wl_resource_get_user_data(res);
    if (!ls) return;
    if (serial == ls->conf_serial)
        ls->acked = true;
    ls->ever_acked = true;
}

static const struct zwlr_layer_surface_v1_interface _ls_impl = {
    .destroy            = _ls_destroy,
    .ack_configure      = _ls_ack_configure,
    .set_size           = _ls_set_size,
    .set_anchor         = _ls_set_anchor,
    .set_exclusive_zone = _ls_set_exclusive_zone,
    .set_margin         = _ls_set_margin,
    .set_keyboard_interactivity = _ls_set_kbd_mode,
    .get_popup          = _ls_get_popup,
    .set_layer          = _ls_set_layer,
};

/* ----------------------------------------------- shell global binding */

static void _shell_get_layer_surface(struct wl_client *cli,
                                     struct wl_resource *res,
                                     uint32_t id,
                                     struct wl_resource *surface,
                                     struct wl_resource *output,
                                     uint32_t layer,
                                     const char *namespace) {
    (void)output;   /* single-output compositor: the output is implied */
    _wl_surf_t *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (!s) {
        wl_resource_post_error(res,
            ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
            "get_layer_surface on a dead surface");
        return;
    }
    if (s->toplevel || s->popup || s->layer) {
        wl_resource_post_error(res,
            ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
            "surface already has a role");
        return;
    }
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(res,
            ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
            "invalid layer %u", layer);
        return;
    }
    _layer_surf_t *ls = vt_malloc0(sizeof(*ls));
    if (!ls) { wl_client_post_no_memory(cli); return; }
    ls->surf = s;
    ls->layer = layer;
    ls->excl_zone = 0;
    ls->kbd_mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    struct wl_resource *lres = wl_resource_create(
        cli, &zwlr_layer_surface_v1_interface,
        wl_resource_get_version(res), id);
    if (!lres) { vt_free(ls); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(lres, &_ls_impl, ls, _ls_res_destroy);
    ls->res = lres;
    ls->namespace = namespace ? vt_strdup(namespace) : NULL;
    s->layer = ls;
    /* first configure goes out immediately (spec allows configure
     * before the first commit; gtk4-layer-shell expects this) */
    _layer_send_configure(ls);
}

static void _shell_destroy(struct wl_client *cli, struct wl_resource *res) {
    (void)cli; (void)res;
}

static const struct zwlr_layer_shell_v1_interface _shell_impl = {
    .get_layer_surface = _shell_get_layer_surface,
    .destroy           = _shell_destroy,
};

static void _bind_layer_shell(struct wl_client *cli, void *data,
                              uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &zwlr_layer_shell_v1_interface,
        version < 4 ? version : 4, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_shell_impl, NULL, NULL);
}

struct wl_global *_layer_shell_global_create(_wl_state_t *st) {
    st->layer_shell_g = wl_global_create(
        st->display, &zwlr_layer_shell_v1_interface, 4, NULL,
        _bind_layer_shell);
    return st->layer_shell_g;
}

void _layer_shell_global_destroy(_wl_state_t *st) {
    if (st && st->layer_shell_g) {
        wl_global_destroy(st->layer_shell_g);
        st->layer_shell_g = NULL;
    }
}

/* ------------------------------------------------- backend-core hooks */

/* Called from _surf_commit for surfaces carrying the layer role.
 * First valid commit (client acked + attached) → place + map. */
bool _layer_commit(_wl_surf_t *s) {
    _layer_surf_t *ls = s->layer;
    _wl_state_t *st = _wls;
    if (!ls || !st) return false;
    if (ls->closed) return true;          /* closed: never maps again */
    if (!ls->configured) _layer_send_configure(ls);
    if (!ls->ever_acked) {
        /* not a single configure acked yet: the client must complete
         * the initial handshake before its first commit counts */
        return true;
    }
    if (!ls->acked && s->mapped) {
        /* size change pending: reconfigure so the client resizes */
        _layer_send_configure(ls);
    }
    if (!s->mapped && s->w > 0 && s->h > 0) {
        _layer_place(ls);
        s->ws = -1;                        /* layer surfaces: all wspaces */
        _stack_insert(s);
        s->mapped = true;
        st->dirty = true;
        /* exclusive keyboard: docked surface grabs all keys (locks) */
        if (_layer_kbd_exclusive(s) && s->res)
            _kbd_enter_focus(st, s);
        vt_logi("wayland: layer surface '%s' mapped %dx%d at +%d+%d "
                "(layer %u, excl %d, kbd %u)",
                ls->namespace ? ls->namespace : "",
                s->w, s->h, s->x, s->y, ls->layer, _layer_excl(ls),
                ls->kbd_mode);
    } else if (s->mapped && s->w > 0) {
        /* resize/anchor refresh */
        _layer_place(ls);
        st->dirty = true;
    }
    /* size convergence: an anchored surface must end up at the
     * configured size — GTK sometimes commits its natural size before
     * applying the configure; nudge it until they agree. Edge-triggered:
     * one configure per mismatch APPEARANCE, so a client that cannot
     * resize (locked size) is not flooded with configures. */
    bool fill_w = (ls->anchors & (_A_LEFT | _A_RIGHT)) == (_A_LEFT | _A_RIGHT);
    bool fill_h = (ls->anchors & (_A_TOP | _A_BOTTOM)) == (_A_TOP | _A_BOTTOM);
    bool mismatch = s->mapped && ((fill_w && s->w != ls->conf_w) ||
                                  (fill_h && s->h != ls->conf_h));
    if (mismatch && !ls->size_mismatch)
        _layer_send_configure(ls);
    ls->size_mismatch = mismatch;
    return true;
}

void _layer_relayout(_wl_surf_t *s) {
    if (s && s->layer) {
        _layer_place(s->layer);
        _layer_send_configure(s->layer);
        if (_wls) _wls->dirty = true;
    }
}

/* popup placement helper re-exported for _ls_get_popup: the positioner
 * math lives in the backend core; we need it here for the layer parent
 * case. Declared in vt-wl-internal.h as _popup_place. */

#endif /* VT_HAVE_WAYLAND */
