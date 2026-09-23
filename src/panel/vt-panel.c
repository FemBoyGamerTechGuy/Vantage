/*
 * vt-panel.c — Vantage panel + applet registry
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The panel is a thin shell hosting applets. Applets implement the
 * vt_panel_plugin_api_t interface and may be:
 *   - built-in (linked into the panel binary)
 *   - runtime-loaded (dlopen) — see vt_panel_applet_register()
 *
 * Applets are intentionally tiny. Heavy logic lives elsewhere.
 */

#define VT_LOG_DOMAIN "panel"
#include <vantage/vt-panel.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

vt_panel_t *vt_panel_new(vt_renderer_t *r) {
    vt_panel_t *p = vt_malloc0(sizeof(*p));
    p->pos = VT_PANEL_POS_TOP;
    p->height = 32;
    p->visible = true;
    p->renderer = r;
    vt_vec_init(&p->applets, sizeof(vt_panel_applet_t), 8);
    return p;
}
void vt_panel_free(vt_panel_t *p) {
    if (!p) return;
    for (size_t i = 0; i < p->applets.size; i++) {
        vt_panel_applet_t *a = vt_vec_at(&p->applets, i);
        if (a->inst && a->api && a->api->fini) a->api->fini(a->inst);
    }
    vt_vec_fini(&p->applets);
    vt_free(p);
}
int vt_panel_start(vt_panel_t *p) {
    if (!p) return VT_ERR_INVAL;
    p->running = true;
    vt_logi("panel: started, %zu applets", p->applets.size);
    return VT_OK;
}
void vt_panel_stop(vt_panel_t *p) {
    if (!p) return;
    p->running = false;
}

static const vt_panel_plugin_api_t *_lookup_api(vt_panel_applet_kind_t k) {
    switch (k) {
    case VT_PANEL_APPLET_LAUNCHER:  return &vt_panel_applet_launcher;
    case VT_PANEL_APPLET_TASKLIST:  return &vt_panel_applet_tasklist;
    case VT_PANEL_APPLET_CLOCK:     return &vt_panel_applet_clock;
    case VT_PANEL_APPLET_WORKSPACES:return &vt_panel_applet_workspaces;
    case VT_PANEL_APPLET_TRAY:      return &vt_panel_applet_tray;
    case VT_PANEL_APPLET_VOLUME:    return &vt_panel_applet_volume;
    case VT_PANEL_APPLET_NETWORK:   return &vt_panel_applet_network;
    case VT_PANEL_APPLET_BATTERY:   return &vt_panel_applet_battery;
    default: return NULL;
    }
}
int vt_panel_add_applet(vt_panel_t *p, vt_panel_applet_kind_t kind) {
    if (!p) return VT_ERR_INVAL;
    const vt_panel_plugin_api_t *api = _lookup_api(kind);
    if (!api) return VT_ERR_NOTSUPP;
    vt_panel_applet_t a = { .kind = kind, .api = api };
    a.inst = api->init ? api->init(p) : NULL;
    if (!vt_vec_push(&p->applets, &a)) return VT_ERR_NOMEM;
    return (int)p->applets.size - 1;
}
int vt_panel_remove_applet(vt_panel_t *p, int idx) {
    if (!p || idx < 0 || (size_t)idx >= p->applets.size) return VT_ERR_INVAL;
    vt_panel_applet_t *a = vt_vec_at(&p->applets, idx);
    if (a->inst && a->api && a->api->fini) a->api->fini(a->inst);
    vt_vec_remove(&p->applets, idx);
    return VT_OK;
}

void vt_panel_render(vt_panel_t *p) {
    if (!p || !p->renderer) return;
    int w = 1920, h = p->height;
    if (p->pos == VT_PANEL_POS_LEFT || p->pos == VT_PANEL_POS_RIGHT)
        { h = 1080; w = p->height; }
    vt_renderer_begin(p->renderer, w, h);
    vt_color_t bg = {0.05f, 0.05f, 0.07f, 0.95f};
    vt_rect_t r = {0, 0, w, h};
    vt_renderer_fill_rect(p->renderer, r, bg);
    int x = 4;
    for (size_t i = 0; i < p->applets.size; i++) {
        vt_panel_applet_t *a = vt_vec_at(&p->applets, i);
        size_t pref = (a->api && a->api->preferred_size) ?
                       a->api->preferred_size(a->inst) : 60;
        a->area = (vt_rect_t){x, 0, (int)pref, h};
        if (a->api && a->api->render) a->api->render(a->inst, p->renderer, a->area);
        x += (int)pref + 4;
    }
    vt_renderer_end(p->renderer);
    vt_renderer_present(p->renderer);
}

void vt_panel_set_pos(vt_panel_t *p, vt_panel_pos_t pos) {
    if (p) p->pos = pos;
}
void vt_panel_set_height(vt_panel_t *p, int h) {
    if (p && h > 0) p->height = h;
}

/* Applet registry for runtime-loaded plugins */
static vt_vec_t _registry;
static pthread_mutex_t _reg_lock = PTHREAD_MUTEX_INITIALIZER;
int vt_panel_applet_register(const vt_panel_plugin_api_t *api) {
    if (!api) return -1;
    pthread_mutex_lock(&_reg_lock);
    if (_registry.elem_sz == 0)
        vt_vec_init(&_registry, sizeof(vt_panel_plugin_api_t *), 8);
    const vt_panel_plugin_api_t *p = api;
    vt_vec_push(&_registry, &p);
    int id = (int)_registry.size - 1;
    pthread_mutex_unlock(&_reg_lock);
    return id;
}
