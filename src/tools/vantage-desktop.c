/*
 * vantage-desktop.c — Standalone desktop binary
 */

#define VT_LOG_DOMAIN "desktop"
#include <vantage/vt-core.h>
#include <vantage/vt-desktop.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-wallpaper.h>
#include <vantage/vt-config.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);
    vt_log_set_level(VT_LOG_INFO);

    vt_renderer_t *r = vt_renderer_new(VT_RENDERER_AUTO);
    vt_desktop_t *d = vt_desktop_new(r);
    vt_desktop_start(d);

    while (!_stop) {
        vt_desktop_render(d);
        vt_time_sleep_ms(100);
    }
    vt_desktop_free(d);
    vt_renderer_free(r);
    return 0;
}
