/*
 * vt-wl-protocols.c — staging protocols the real world expects
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Four protocols desktop toolkits probe for (foot prints a warning for
 * each missing one; GTK/Qt degrade silently):
 *
 *   wp_cursor_shape_manager_v1   server-side cursors: the client asks
 *                                for a NAMED shape and the compositor
 *                                renders it from the Xcursor theme.
 *                                Kills the per-app fallback-cursor
 *                                inconsistency (each toolkit loading
 *                                whatever icon it can find) and the
 *                                oversized/colored "aura" cursors that
 *                                produced.
 *   xdg_activation_v1           focus/urgency tokens: apps can request
 *                                activation ("focus my window"), and
 *                                foot's bell.urgent stops warning.
 *   wp_fractional_scale_manager_v1
 *                                per-surface preferred scale. We run
 *                                integer scale 1 → preferred_scale(120);
 *                                clients stop warning and render at the
 *                                exact scale we declared.
 *   xdg_toplevel_icon_manager_v1
 *                                taskbar icons v2: names/buffers are
 *                                accepted (and could later feed the
 *                                panel tasklist); nothing renders from
 *                                them yet.
 *
 * text-input/IME is deliberately NOT implemented: an honest global that
 * swallows keys would break dead-key compose in terminals; without an
 * input-method framework there is nothing useful to relay. foot keeps
 * its own compose handling instead — that warning is a documented
 * limitation, not a bug.
 */
#include "vt-wl-internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cursor-shape-protocol.h"
#include "xdg-activation-protocol.h"
#include "fractional-scale-protocol.h"
#include "xdg-toplevel-icon-protocol.h"
#include "viewporter-protocol.h"

/* ------------------------------------------------------ cursor-shape */

static void _cshape_device_destroy(struct wl_client *cli,
                                   struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _cshape_device_set_shape(struct wl_client *cli,
                                     struct wl_resource *res,
                                     uint32_t serial, uint32_t shape) {
    (void)cli; (void)res; (void)serial;
    /* The request is tied to the wl_pointer it was created from; we
     * track one active client cursor at a time (single-seat desktop).
     * The compositor loads and shows the shape; when the pointer
     * leaves the requesting surface the core releases it again. */
    _cursor_client_shape(shape);
}

static const struct wp_cursor_shape_device_v1_interface
    _cshape_device_impl = {
    .destroy = _cshape_device_destroy,
    .set_shape = _cshape_device_set_shape,
};

static void _cshape_mgr_get_pointer(struct wl_client *cli,
                                    struct wl_resource *res, uint32_t id,
                                    struct wl_resource *pointer) {
    (void)pointer;
    struct wl_resource *r = wl_resource_create(
        cli, &wp_cursor_shape_device_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_cshape_device_impl, NULL, NULL);
}

static void _cshape_mgr_get_tablet(struct wl_client *cli,
                                   struct wl_resource *res, uint32_t id,
                                   struct wl_resource *tool) {
    /* we have no tablet input path yet; hand out a stub that at least
     * speaks the interface so v2 clients do not die on bind */
    (void)tool;
    struct wl_resource *r = wl_resource_create(
        cli, &wp_cursor_shape_device_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_cshape_device_impl, NULL, NULL);
}

static void _cshape_mgr_destroy(struct wl_client *cli,
                                struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct wp_cursor_shape_manager_v1_interface
    _cshape_mgr_impl = {
    .destroy = _cshape_mgr_destroy,
    .get_pointer = _cshape_mgr_get_pointer,
    .get_tablet_tool_v2 = _cshape_mgr_get_tablet,
};

static void _bind_cshape_mgr(struct wl_client *cli, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &wp_cursor_shape_manager_v1_interface,
        version > 2 ? 2 : version, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_cshape_mgr_impl, NULL, NULL);
}

/* ------------------------------------------------------- xdg-activation */

typedef struct _act_token {
    struct wl_resource *res;
    _wl_surf_t *surf;               /* set_surface target (weak) */
    char *token;                    /* generated secret string */
    bool committed;
} _act_token_t;

static void _act_token_free(_act_token_t *t) {
    if (!t) return;
    vt_free(t->token);
    vt_free(t);
}

static void _act_token_res_destroy(struct wl_resource *res) {
    _act_token_t *t = wl_resource_get_user_data(res);
    if (!t) return;
    if (t->surf) t->surf->act_token = NULL;   /* detach weak link */
    _act_token_free(t);
}

static void _act_token_destroy(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _act_token_set_serial(struct wl_client *cli,
                                  struct wl_resource *res,
                                  uint32_t serial,
                                  struct wl_resource *seat) {
    (void)cli; (void)res; (void)serial; (void)seat;
}

static void _act_token_set_app_id(struct wl_client *cli,
                                  struct wl_resource *res,
                                  const char *app_id) {
    (void)cli; (void)res; (void)app_id;
}

static void _act_token_set_surface(struct wl_client *cli,
                                   struct wl_resource *res,
                                   struct wl_resource *surface) {
    (void)cli;
    _act_token_t *t = wl_resource_get_user_data(res);
    if (!t) return;
    _wl_surf_t *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (t->surf && t->surf->act_token == t) t->surf->act_token = NULL;
    t->surf = s;
    if (s) s->act_token = t;
}

static void _act_token_commit(struct wl_client *cli,
                              struct wl_resource *res) {
    (void)cli;
    _act_token_t *t = wl_resource_get_user_data(res);
    if (!t || t->committed) return;
    t->committed = true;
    /* a fresh unguessable string: tokens are capability references */
    unsigned char rnd[16] = {0};
    int rf = open("/dev/urandom", O_RDONLY);
    if (rf >= 0) {
        if (read(rf, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
            /* fall back to time+id below */
        }
        close(rf);
    }
    char buf[64];
    if (rnd[0] || rnd[1]) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            buf[2 * i] = hex[rnd[i] >> 4];
            buf[2 * i + 1] = hex[rnd[i] & 15];
        }
        buf[32] = 0;
    } else {
        snprintf(buf, sizeof(buf), "vt%llu-%u",
                 (unsigned long long)vt_time_now_us(),
                 (unsigned)(uintptr_t)t);
    }
    vt_free(t->token);
    t->token = vt_strdup(buf);
    xdg_activation_token_v1_send_done(res, t->token);
}

static const struct xdg_activation_token_v1_interface _act_token_impl = {
    .destroy = _act_token_destroy,
    .set_serial = _act_token_set_serial,
    .set_app_id = _act_token_set_app_id,
    .set_surface = _act_token_set_surface,
    .commit = _act_token_commit,
};

static void _act_get_token(struct wl_client *cli, struct wl_resource *res,
                           uint32_t id) {
    _act_token_t *t = vt_malloc0(sizeof(*t));
    if (!t) { wl_client_post_no_memory(cli); return; }
    struct wl_resource *r = wl_resource_create(
        cli, &xdg_activation_token_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { vt_free(t); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_act_token_impl, t,
                                   _act_token_res_destroy);
    t->res = r;
}

static void _act_activate(struct wl_client *cli, struct wl_resource *res,
                          const char *token, struct wl_resource *surface) {
    (void)cli; (void)res;
    /* an explicit surface argument (newer draft) wins when given */
    if (surface) {
        _wl_surf_t *s = wl_resource_get_user_data(surface);
        if (s && s->mapped && !s->minimized && s->toplevel)
            _focus_surface(s);
        return;
    }
    if (!token || !*token) return;
    /* find the committed token object with this string (compositor-own
     * tokens only — the string IS the capability) and focus its
     * surface the same way a click would */
    _wl_state_t *st = _wls;
    if (!st) return;
    _wl_surf_t *s;
    wl_list_for_each(s, &st->surfaces, link) {
        if (s->act_token && s->act_token->committed &&
            s->act_token->token && vt_streq(s->act_token->token, token)) {
            if (s->mapped && !s->minimized && s->toplevel)
                _focus_surface(s);
            return;
        }
    }
}

static void _act_mgr_destroy(struct wl_client *cli,
                             struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct xdg_activation_v1_interface _act_impl = {
    .destroy = _act_mgr_destroy,
    .get_activation_token = _act_get_token,
    .activate = _act_activate,
};

static void _bind_act(struct wl_client *cli, void *data,
                      uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &xdg_activation_v1_interface, version, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_act_impl, NULL, NULL);
}

/* ---------------------------------------------------- fractional-scale */

typedef struct _fscale {
    struct wl_resource *res;
    _wl_surf_t *surf;               /* weak */
} _fscale_t;

static void _fscale_res_destroy(struct wl_resource *res) {
    _fscale_t *f = wl_resource_get_user_data(res);
    if (!f) return;
    if (f->surf) f->surf->fscale = NULL;
    vt_free(f);
}

static void _fscale_destroy(struct wl_client *cli,
                            struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct wp_fractional_scale_v1_interface _fscale_impl = {
    .destroy = _fscale_destroy,
};

static void _fscale_mgr_get(struct wl_client *cli, struct wl_resource *res,
                            uint32_t id, struct wl_resource *surface) {
    _wl_surf_t *s = surface ? wl_resource_get_user_data(surface) : NULL;
    _fscale_t *f = vt_malloc0(sizeof(*f));
    if (!f) { wl_client_post_no_memory(cli); return; }
    struct wl_resource *r = wl_resource_create(
        cli, &wp_fractional_scale_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { vt_free(f); wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_fscale_impl, f,
                                   _fscale_res_destroy);
    f->res = r;
    f->surf = s;
    if (s) s->fscale = r;
    /* integer scale 1.0 → 120 in the 24.8 wire format; sent at once
     * (the event is allowed before the surface is configured) */
    wp_fractional_scale_v1_send_preferred_scale(r, 120);
}

static void _fscale_mgr_destroy(struct wl_client *cli,
                                struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct wp_fractional_scale_manager_v1_interface
    _fscale_mgr_impl = {
    .destroy = _fscale_mgr_destroy,
    .get_fractional_scale = _fscale_mgr_get,
};

static void _bind_fscale(struct wl_client *cli, void *data,
                         uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &wp_fractional_scale_manager_v1_interface, version, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_fscale_mgr_impl, NULL, NULL);
}

/* ----------------------------------------------------- toplevel-icon */

static void _ticon_destroy(struct wl_client *cli,
                           struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _ticon_set_name(struct wl_client *cli,
                            struct wl_resource *res, const char *name) {
    /* accepted: a themed icon name for the taskbar; the panel still
     * resolves icons from app_id for now */
    (void)cli; (void)res; (void)name;
}

static void _ticon_add_buffer(struct wl_client *cli,
                              struct wl_resource *res,
                              struct wl_resource *shm_buffer,
                              int32_t scale) {
    (void)cli; (void)res; (void)shm_buffer; (void)scale;
    /* accepted-but-not-rendered (documented): the tasklist keeps
     * using themed icons */
}

static const struct xdg_toplevel_icon_v1_interface _ticon_impl = {
    .destroy = _ticon_destroy,
    .set_name = _ticon_set_name,
    .add_buffer = _ticon_add_buffer,
};

static void _ticon_res_destroy(struct wl_resource *res) {
    (void)res;
}

static void _ticon_mgr_create_icon(struct wl_client *cli,
                                   struct wl_resource *res, uint32_t id) {
    struct wl_resource *r = wl_resource_create(
        cli, &xdg_toplevel_icon_v1_interface,
        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_ticon_impl, NULL,
                                   _ticon_res_destroy);
}

static void _ticon_mgr_set_icon(struct wl_client *cli,
                                struct wl_resource *res,
                                struct wl_resource *toplevel,
                                struct wl_resource *icon) {
    (void)cli; (void)res; (void)toplevel; (void)icon;
}

static void _ticon_mgr_destroy(struct wl_client *cli,
                               struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct xdg_toplevel_icon_manager_v1_interface
    _ticon_mgr_impl = {
    .destroy = _ticon_mgr_destroy,
    .create_icon = _ticon_mgr_create_icon,
    .set_icon = _ticon_mgr_set_icon,
};

static void _bind_ticon_mgr(struct wl_client *cli, void *data,
                            uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &xdg_toplevel_icon_manager_v1_interface, version, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_ticon_mgr_impl, NULL, NULL);
    /* the manager's ready sequence: at least one preferred icon size
     * followed by done — clients (foot) treat the protocol as
     * implemented only after done arrives, and warned otherwise */
    xdg_toplevel_icon_manager_v1_send_icon_size(res, 64);
    xdg_toplevel_icon_manager_v1_send_done(res);
}

/* ------------------------------------------------------------ viewporter */

static void _viewport_destroy(struct wl_client *cli,
                              struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static void _viewport_set_source(struct wl_client *cli,
                                 struct wl_resource *res,
                                 wl_fixed_t x, wl_fixed_t y,
                                 wl_fixed_t width, wl_fixed_t height) {
    /* accepted; identity in practice — the compositor declares a
     * constant scale of 1.0 (preferred_scale 120), so clients never
     * set a scaling viewport, and source-rect cropping is not used by
     * the toolkits we target */
    (void)cli; (void)res; (void)x; (void)y; (void)width; (void)height;
}

static void _viewport_set_destination(struct wl_client *cli,
                                      struct wl_resource *res,
                                      int32_t width, int32_t height) {
    (void)cli; (void)res; (void)width; (void)height;
}

static const struct wp_viewport_interface _viewport_impl = {
    .destroy = _viewport_destroy,
    .set_source = _viewport_set_source,
    .set_destination = _viewport_set_destination,
};

static void _viewport_res_destroy(struct wl_resource *res) {
    (void)res;
}

static void _viewporter_get_viewport(struct wl_client *cli,
                                     struct wl_resource *res, uint32_t id,
                                     struct wl_resource *surface) {
    (void)surface;
    struct wl_resource *r = wl_resource_create(
        cli, &wp_viewport_interface, wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(r, &_viewport_impl, NULL,
                                   _viewport_res_destroy);
}

static void _viewporter_destroy(struct wl_client *cli,
                                struct wl_resource *res) {
    (void)cli;
    wl_resource_destroy(res);
}

static const struct wp_viewporter_interface _viewporter_impl = {
    .destroy = _viewporter_destroy,
    .get_viewport = _viewporter_get_viewport,
};

static void _bind_viewporter(struct wl_client *cli, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *res = wl_resource_create(
        cli, &wp_viewporter_interface, version, id);
    if (!res) { wl_client_post_no_memory(cli); return; }
    wl_resource_set_implementation(res, &_viewporter_impl, NULL, NULL);
}

/* -------------------------------------------------------- lifecycle */

void _proto_globals_create(_wl_state_t *st) {
    st->cshape_g = wl_global_create(st->display,
        &wp_cursor_shape_manager_v1_interface, 1, NULL, _bind_cshape_mgr);
    st->activation_g = wl_global_create(st->display,
        &xdg_activation_v1_interface, 1, NULL, _bind_act);
    st->fscale_g = wl_global_create(st->display,
        &wp_fractional_scale_manager_v1_interface, 1, NULL, _bind_fscale);
    st->ticon_g = wl_global_create(st->display,
        &xdg_toplevel_icon_manager_v1_interface, 1, NULL, _bind_ticon_mgr);
    st->viewporter_g = wl_global_create(st->display,
        &wp_viewporter_interface, 1, NULL, _bind_viewporter);
    vt_logi("wayland: staging protocols: cursor-shape-v1, "
            "xdg-activation-v1, fractional-scale-v1 (viewporter), "
            "xdg-toplevel-icon-v1");
}

void _proto_globals_destroy(_wl_state_t *st) {
    if (st->cshape_g) wl_global_destroy(st->cshape_g);
    if (st->activation_g) wl_global_destroy(st->activation_g);
    if (st->fscale_g) wl_global_destroy(st->fscale_g);
    if (st->ticon_g) wl_global_destroy(st->ticon_g);
    if (st->viewporter_g) wl_global_destroy(st->viewporter_g);
    st->cshape_g = NULL;
    st->activation_g = NULL;
    st->fscale_g = NULL;
    st->ticon_g = NULL;
    st->viewporter_g = NULL;
}

void _proto_surface_destroyed(_wl_surf_t *s) {
    /* per-surface protocol objects hold WEAK surf pointers: detach
     * them here (their own resource destructors free the records) */
    if (s->act_token) {
        s->act_token->surf = NULL;
        s->act_token = NULL;
    }
    if (s->fscale) {
        _fscale_t *f = wl_resource_get_user_data(s->fscale);
        if (f) f->surf = NULL;
        s->fscale = NULL;
    }
}
