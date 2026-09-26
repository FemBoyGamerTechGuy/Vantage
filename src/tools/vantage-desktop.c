/*
 * vantage-desktop.c — Standalone desktop binary
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Owns the wallpaper surface, desktop icons and the root context menu.
 * Started by vantage-session; also works standalone on any EWMH WM.
 */

#define VT_LOG_DOMAIN "desktop"
#include <vantage/vt-core.h>
#include <vantage/vt-desktop.h>
#include <vantage/vt-renderer.h>
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
    signal(SIGPIPE, SIG_IGN);
    vt_log_set_level(VT_LOG_INFO);

    vt_desktop_t *d = vt_desktop_new(NULL);
    if (vt_desktop_start(d) != VT_OK) {
        vt_loge("desktop: failed to start");
        vt_desktop_free(d);
        return 1;
    }

    while (!_stop)
        vt_desktop_step(d, 100);

    vt_logi("desktop: shutting down");
    vt_desktop_free(d);
    return 0;
}
