/*
 * vantage-session.c — Session entry point
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the binary registered in the xsessions .desktop file. It
 * starts the chosen backend, renderer, compositor, WM, panel, and
 * desktop, runs autostart, and blocks until logout/shutdown.
 */

#define VT_LOG_DOMAIN "session"
#include <vantage/vt-core.h>
#include <vantage/vt-config.h>
#include <vantage/vt-session.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-compositor.h>
#include <vantage/vt-wm.h>
#include <vantage/vt-panel.h>
#include <vantage/vt-desktop.h>
#include <vantage/vt-wallpaper.h>
#include <vantage/vt-theme.h>
#include <vantage/vt-settings.h>
#include <vantage/vt-integrations.h>

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);

    vt_log_set_level(VT_LOG_INFO);
    vt_logi("session: Vantage " VT_VERSION);

    /* Load config */
    vt_config_t *cfg = vt_config_new_defaults();
    if (vt_config_load(cfg, vt_config_default_path()) == VT_OK)
        vt_logi("session: config loaded from %s", vt_config_default_path());
    else
        vt_logi("session: using defaults (config not yet created)");

    /* Detect & setup power integration */
    vt_power_t *pwr = vt_power_new();
    vt_power_init(pwr);
    (void)pwr;

    /* Start session manager */
    vt_session_t *s = vt_session_new();

    /* Initialize backend (auto-detected) */
    vt_backend_kind_t be_kind = vt_backend_kind_from_str(
        vt_config_get(cfg, "desktop", "backend", "auto"));
    vt_backend_t *backend = vt_backend_new(be_kind);
    vt_logi("session: backend=%s", vt_backend_name(backend));

    /* Initialize renderer (auto) */
    const char *r_str = vt_config_get(cfg, "desktop", "renderer", "auto");
    vt_renderer_kind_t r_kind = (r_str && r_str[0] == 's') ?
                                VT_RENDERER_SW : VT_RENDERER_AUTO;
    vt_renderer_t *renderer = vt_renderer_new(r_kind);
    vt_logi("session: renderer=%s, hw-accel=%s",
            vt_renderer_name(renderer),
            renderer->caps.hw_accel ? "yes" : "no");

    /* Start compositor */
    vt_compositor_t *comp = vt_compositor_new(renderer, backend);
    vt_compositor_start(comp);

    /* Start WM */
    vt_wm_t *wm = vt_wm_new(backend);
    vt_wm_start(wm);

    /* Apply theme */
    vt_theme_t *theme = vt_theme_load_by_name(
        vt_config_get(cfg, "desktop", "theme", "Vantage-Dark"));
    if (!theme) theme = vt_theme_new();
    vt_theme_apply(theme);

    /* Start panel + add default applets */
    vt_panel_t *panel = vt_panel_new(renderer);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_LAUNCHER);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_TASKLIST);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_WORKSPACES);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_TRAY);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_VOLUME);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_NETWORK);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_BATTERY);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_CLOCK);
    vt_panel_start(panel);

    /* Start desktop */
    vt_desktop_t *desk = vt_desktop_new(renderer);
    vt_desktop_start(desk);

    /* Apply settings */
    vt_settings_t *settings = vt_settings_new();
    vt_settings_apply_all(settings);

    /* Wallpaper */
    vt_wallpaper_t *wp = vt_wallpaper_new();
    const char *wp_path = vt_config_get(cfg, "desktop", "wallpaper", "");
    const char *wp_video = vt_config_get(cfg, "desktop", "video-wallpaper", "");
    if (*wp_video) {
        vt_wallpaper_set_kind(wp, VT_WALLPAPER_VIDEO);
        vt_wallpaper_set_path(wp, wp_video);
        vt_wallpaper_set_volume(wp, (float)vt_config_get_double(cfg, "desktop",
                              "video-volume", 0));
        vt_wallpaper_load(wp, NULL);
    } else if (*wp_path) {
        vt_wallpaper_set_kind(wp, VT_WALLPAPER_IMAGE);
        vt_wallpaper_set_path(wp, wp_path);
        vt_wallpaper_load(wp, NULL);
    } else {
        vt_wallpaper_set_kind(wp, VT_WALLPAPER_COLOR);
    }

    /* Start session */
    vt_session_start(s);
    vt_session_autostart_load(s);

    /* Run main loop until shutdown */
    while (!_stop && vt_session_stage(s) != VT_SESSION_STAGE_SHUTDOWN) {
        vt_backend_dispatch(backend, 50);
        vt_wm_step(wm, 0);
        vt_compositor_step(comp, 0);
        /* render panel + wallpaper via compositor once per frame */
        if (renderer && renderer->initialized) {
            vt_panel_render(panel);
            vt_rect_t r = {0, 32, 1920, 1048};
            vt_wallpaper_step(wp, renderer, 1920, 1048);
            vt_wallpaper_render(wp, renderer, r);
        }
    }

    vt_logi("session: shutting down");
    vt_wallpaper_free(wp);
    vt_session_end(s, VT_SESSION_END_LOGOUT);
    vt_settings_free(settings);
    vt_desktop_free(desk);
    vt_panel_free(panel);
    vt_theme_free(theme);
    vt_wm_free(wm);
    vt_compositor_free(comp);
    vt_renderer_free(renderer);
    vt_backend_free(backend);
    vt_power_free(pwr);
    vt_config_free(cfg);
    vt_session_free(s);
    return 0;
}
