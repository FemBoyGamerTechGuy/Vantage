/*
 * vantage-wm.c — Standalone WM binary
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Useful for running the WM alone on an existing X server (e.g. for
 * debugging or for using Vantage WM inside an existing session).
 */

#define VT_LOG_DOMAIN "wm"
#include <vantage/vt-core.h>
#include <vantage/vt-wm.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-config.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t _stop = 0;
static void _on_sig(int sig) { (void)sig; _stop = 1; }

int main(int argc, char **argv) {
    signal(SIGINT, _on_sig);
    signal(SIGTERM, _on_sig);
    vt_log_set_level(VT_LOG_INFO);

    vt_config_t *cfg = vt_config_new_defaults();
    vt_backend_kind_t be_kind = vt_backend_kind_from_str(
        vt_config_get(cfg, "desktop", "backend", "auto"));
    vt_backend_t *backend = vt_backend_new(be_kind);
    vt_logi("wm: backend=%s", vt_backend_name(backend));

    vt_wm_t *wm = vt_wm_new(backend);
    vt_wm_start(wm);

    while (!_stop) {
        vt_wm_step(wm, 50);
    }
    vt_wm_free(wm);
    vt_backend_free(backend);
    vt_config_free(cfg);
    return 0;
}
