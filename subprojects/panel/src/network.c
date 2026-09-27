/*
 * network.c — network indicator (wired/wifi strength from sysfs)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Same data source as the compositor-side applet: /sys/class/net for
 * link state (carrier) + /proc/net/wireless for wifi quality. No
 * NetworkManager dependency — honest data, degrading to "offline".
 */
#include "network.h"
#include "picon.h"

#include <string.h>

static GtkWidget *icon;
static GtkWidget *button;

/* 0 = down, 1 = wired, 2..5 = wifi (bars) */
static int _probe(char *tip, size_t tip_len) {
    /* wifi quality first: /proc/net/wireless lists live interfaces */
    FILE *f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[512];
        int skipped = 0;
        int best = 0;
        char best_if[64] = {0};
        while (fgets(line, sizeof(line), f)) {
            if (skipped < 2) { skipped++; continue; }  /* headers */
            char ifname[64] = {0};
            double status = 0, link = 0, level = 0, noise = 0;
            /* iface: status link level noise ... (leading space) */
            if (sscanf(line, " %63[^:]: %lf %lf %lf %lf",
                       ifname, &status, &link, &level, &noise) >= 3) {
                /* quality ≈ link/70 clamped, like iwconfig's math */
                int q = (int)((link / 70.0) * 100.0 + 0.5);
                if (q < 0) q = 0;
                if (q > 100) q = 100;
                if (q > best) {
                    best = q;
                    snprintf(best_if, sizeof(best_if), "%s", ifname);
                }
            }
        }
        fclose(f);
        if (best > 0) {
            int bars = best > 75 ? 4 : best > 50 ? 3 : best > 25 ? 2 : 1;
            snprintf(tip, tip_len, "Wi-Fi %s: %d%%", best_if, best);
            return 1 + bars;
        }
    }
    /* wired: any interface with carrier */
    const char *dir = "/sys/class/net";
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (!strncmp(name, "lo", 3)) continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s/carrier", dir, name);
            gchar *buf = NULL;
            gsize blen = 0;
            if (!g_file_get_contents(path, &buf, &blen, NULL) || !buf) {
                g_free(buf);
                continue;
            }
            if (buf[0] == '1') {
                snprintf(tip, tip_len, "Wired (%s)", name);
                g_free(buf);
                g_dir_close(d);
                return 1;
            }
            g_free(buf);
        }
        g_dir_close(d);
    }
    snprintf(tip, tip_len, "Network offline");
    return 0;
}

static const char *_icon_name(int state) {
    switch (state) {
    case 1:  return "network-wired-symbolic";
    case 2:  return "network-wireless-signal-weak-symbolic";
    case 3:  return "network-wireless-signal-ok-symbolic";
    case 4:  return "network-wireless-signal-good-symbolic";
    case 5:  return "network-wireless-signal-excellent-symbolic";
    default: return "network-offline-symbolic";
    }
}

static gboolean _tick(gpointer user) {
    (void)user;
    char tip[128];
    int st = _probe(tip, sizeof(tip));
    if (icon) {
        /* themed name when the theme has it, drawn glyph otherwise —
         * never an unchecked set_from_icon_name (GTK 4.18 falls back
         * to image-missing and recurses without bound on themes that
         * do not ship it) */
        vp_icon_image_set(GTK_IMAGE(icon), _icon_name(st), 18,
                          st == 0 ? VP_GLYPH_NET_OFFLINE
                                  : VP_GLYPH_NET_ONLINE);
    }
    if (button)
        gtk_widget_set_tooltip_text(button, tip);
    return G_SOURCE_CONTINUE;
}

GtkWidget *network_new(void) {
    icon = vp_icon_image_new("network-offline-symbolic", 18,
                             VP_GLYPH_NET_OFFLINE);
    button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), icon);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, "Network");
    gtk_widget_set_sensitive(button, FALSE);  /* indicator, not control */
    _tick(NULL);
    g_timeout_add_seconds(4, _tick, NULL);
    return button;
}
