/*
 * vantage-panel.c — Standalone panel binary
 */

#define VT_LOG_DOMAIN "panel"
#include <vantage/vt-core.h>
#include <vantage/vt-panel.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-config.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);
    vt_log_set_level(VT_LOG_INFO);

    vt_renderer_t *r = vt_renderer_new(VT_RENDERER_AUTO);
    vt_panel_t *p = vt_panel_new(r);
    vt_panel_add_applet(p, VT_PANEL_APPLET_LAUNCHER);
    vt_panel_add_applet(p, VT_PANEL_APPLET_CLOCK);
    vt_panel_add_applet(p, VT_PANEL_APPLET_VOLUME);
    vt_panel_add_applet(p, VT_PANEL_APPLET_NETWORK);
    vt_panel_add_applet(p, VT_PANEL_APPLET_BATTERY);
    vt_panel_start(p);
    while (!_stop) {
        vt_panel_render(p);
        vt_time_sleep_ms(16);
    }
    vt_panel_free(p);
    vt_renderer_free(r);
    return 0;
}
