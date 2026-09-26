/*
 * vantage-panel.c — Standalone panel binary
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Reads the panel section of the Vantage config (position, height,
 * applet list), creates the panel and runs the event loop. Normally
 * started by vantage-session, but works standalone on any EWMH WM
 * (struts and dock hints are honored by compliant managers).
 */

#define VT_LOG_DOMAIN "panel"
#include <vantage/vt-core.h>
#include <vantage/vt-panel.h>
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
    signal(SIGPIPE, SIG_IGN);
    vt_log_set_level(VT_LOG_INFO);
    for (int i = 1; i < argc; i++) {
        if (vt_streq(argv[i], "-v") || vt_streq(argv[i], "--verbose"))
            vt_log_set_level(VT_LOG_DEBUG);
    }

    vt_config_t *cfg = vt_config_new_defaults();
    vt_config_load(cfg, vt_config_default_path());

    vt_panel_t *panel = vt_panel_new(NULL);
    const char *pos = vt_config_get(cfg, "panel", "position", "top");
    if (vt_strcaseeq(pos, "bottom")) panel->pos = VT_PANEL_POS_BOTTOM;
    else if (vt_strcaseeq(pos, "left")) panel->pos = VT_PANEL_POS_LEFT;
    else if (vt_strcaseeq(pos, "right")) panel->pos = VT_PANEL_POS_RIGHT;
    panel->height = (int)vt_config_get_int(cfg, "panel", "height", 32);

    /* default applet set — LEFT: start button + task list;
     * RIGHT (pinned, reverse order): workspaces, volume, clock, username.
     * battery/network/tray remain available via the config. */
    vt_panel_add_applet(panel, VT_PANEL_APPLET_LAUNCHER);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_TASKLIST);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_WORKSPACES);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_VOLUME);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_CLOCK);
    vt_panel_add_applet(panel, VT_PANEL_APPLET_USER);

    if (vt_panel_start(panel) != VT_OK) {
        vt_loge("panel: failed to start");
        vt_panel_free(panel);
        vt_config_free(cfg);
        return 1;
    }

    while (!_stop)
        vt_panel_step(panel, 100);

    vt_logi("panel: shutting down");
    vt_panel_stop(panel);
    vt_panel_free(panel);
    vt_config_free(cfg);
    return 0;
}
