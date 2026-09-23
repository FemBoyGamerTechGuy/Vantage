/*
 * vt-panel-applets.c — Built-in panel applets
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Each applet is a tiny implementation of vt_panel_plugin_api_t.
 */

#define VT_LOG_DOMAIN "panel"
#include <vantage/vt-panel.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-integrations.h>
#include <vantage/vt-wm.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define API_VERSION 1

/* launcher ---------------------------------------------------------------- */
static void *vt_pa_launcher_init(vt_panel_t *p) {
    (void)p;
    return (void *)1;
}
static void vt_pa_launcher_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t c = {0.3f, 0.6f, 1.0f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
}
static size_t vt_pa_launcher_size(void *inst) { (void)inst; return 40; }

const vt_panel_plugin_api_t vt_panel_applet_launcher = {
    .init = vt_pa_launcher_init,
    .render = vt_pa_launcher_render,
    .preferred_size = vt_pa_launcher_size,
    .name = "launcher",
    .api_version = API_VERSION,
};

/* tasklist ---------------------------------------------------------------- */
static void *vt_pa_tasklist_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_tasklist_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t c = {0.2f, 0.2f, 0.25f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
}
static size_t vt_pa_tasklist_size(void *inst) { (void)inst; return 220; }

const vt_panel_plugin_api_t vt_panel_applet_tasklist = {
    .init = vt_pa_tasklist_init, .render = vt_pa_tasklist_render,
    .preferred_size = vt_pa_tasklist_size, .name = "tasklist",
    .api_version = API_VERSION,
};

/* clock ------------------------------------------------------------------- */
static void *vt_pa_clock_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_clock_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t bg = {0.1f, 0.1f, 0.15f, 1.0f};
    vt_renderer_fill_rect(r, area, bg);
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    (void)buf;
}
static size_t vt_pa_clock_size(void *inst) { (void)inst; return 80; }

const vt_panel_plugin_api_t vt_panel_applet_clock = {
    .init = vt_pa_clock_init, .render = vt_pa_clock_render,
    .preferred_size = vt_pa_clock_size, .name = "clock",
    .api_version = API_VERSION,
};

/* workspaces -------------------------------------------------------------- */
static void *vt_pa_ws_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_ws_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t c = {0.2f, 0.4f, 0.6f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
}
static size_t vt_pa_ws_size(void *inst) { (void)inst; return 100; }

const vt_panel_plugin_api_t vt_panel_applet_workspaces = {
    .init = vt_pa_ws_init, .render = vt_pa_ws_render,
    .preferred_size = vt_pa_ws_size, .name = "workspaces",
    .api_version = API_VERSION,
};

/* tray -------------------------------------------------------------------- */
static void *vt_pa_tray_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_tray_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_color_t c = {0.15f, 0.15f, 0.2f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
}
static size_t vt_pa_tray_size(void *inst) { (void)inst; return 80; }

const vt_panel_plugin_api_t vt_panel_applet_tray = {
    .init = vt_pa_tray_init, .render = vt_pa_tray_render,
    .preferred_size = vt_pa_tray_size, .name = "tray",
    .api_version = API_VERSION,
};

/* volume ------------------------------------------------------------------ */
static void *vt_pa_vol_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_vol_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_audio_t *a = vt_audio_new();
    if (vt_audio_init(a) == VT_OK) {
        vt_color_t c = {0.0f, 0.6f, 0.4f, 1.0f};
        vt_renderer_fill_rect(r, area, c);
    } else {
        vt_color_t c = {0.3f, 0.3f, 0.3f, 1.0f};
        vt_renderer_fill_rect(r, area, c);
    }
    vt_audio_free(a);
}
static size_t vt_pa_vol_size(void *inst) { (void)inst; return 60; }

const vt_panel_plugin_api_t vt_panel_applet_volume = {
    .init = vt_pa_vol_init, .render = vt_pa_vol_render,
    .preferred_size = vt_pa_vol_size, .name = "volume",
    .api_version = API_VERSION,
};

/* network ----------------------------------------------------------------- */
static void *vt_pa_net_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_net_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_net_t *n = vt_net_new();
    vt_net_init(n);
    vt_color_t c = n && vt_net_online(n) ?
        (vt_color_t){0.2f, 0.8f, 0.2f, 1.0f} :
        (vt_color_t){0.8f, 0.2f, 0.2f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
    vt_net_free(n);
}
static size_t vt_pa_net_size(void *inst) { (void)inst; return 50; }

const vt_panel_plugin_api_t vt_panel_applet_network = {
    .init = vt_pa_net_init, .render = vt_pa_net_render,
    .preferred_size = vt_pa_net_size, .name = "network",
    .api_version = API_VERSION,
};

/* battery ----------------------------------------------------------------- */
static void *vt_pa_bat_init(vt_panel_t *p) { (void)p; return (void *)1; }
static void vt_pa_bat_render(void *inst, vt_renderer_t *r, vt_rect_t area) {
    (void)inst;
    vt_power_t *pwr = vt_power_new();
    vt_power_init(pwr);
    int pct = vt_power_battery_pct(pwr);
    vt_color_t c = pct > 50 ? (vt_color_t){0.2f, 0.7f, 0.2f, 1.0f} :
                   pct > 20 ? (vt_color_t){0.9f, 0.7f, 0.2f, 1.0f} :
                              (vt_color_t){0.9f, 0.2f, 0.2f, 1.0f};
    vt_renderer_fill_rect(r, area, c);
    vt_power_free(pwr);
}
static size_t vt_pa_bat_size(void *inst) { (void)inst; return 50; }

const vt_panel_plugin_api_t vt_panel_applet_battery = {
    .init = vt_pa_bat_init, .render = vt_pa_bat_render,
    .preferred_size = vt_pa_bat_size, .name = "battery",
    .api_version = API_VERSION,
};
