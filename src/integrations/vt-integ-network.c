/*
 * vt-integ-network.c — Network state polling (NM > ConnMan > /proc fallback)
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Vantage probes the network backend at runtime. The simplest fallback
 * reads /proc/net/wireless (signal) and /proc/net/route (default route)
 * to determine whether the system is online — works on every Linux
 * distribution without any deps.
 */

#define VT_LOG_DOMAIN "integ-net"
#include <vantage/vt-integrations.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(VT_HAVE_NM)
#include <NetworkManager.h>
#endif

struct vt_net {
    vt_net_backend_t backend;
    char *ssid;
    char *iface;
    int   signal;
    bool  online;
};

vt_net_t *vt_net_new(void) {
    vt_net_t *n = vt_malloc0(sizeof(*n));
    return n;
}
void vt_net_free(vt_net_t *n) {
    if (!n) return;
    vt_free(n->ssid); vt_free(n->iface);
    vt_free(n);
}

vt_net_backend_t vt_net_detect(void) {
#if defined(VT_HAVE_NM)
    return VT_NET_BACKEND_NM;
#endif
    return VT_NET_BACKEND_PROC;
}
const char *vt_net_backend_str(vt_net_backend_t b) {
    switch (b) {
    case VT_NET_BACKEND_NM:      return "NetworkManager";
    case VT_NET_BACKEND_CONNMAN: return "ConnMan";
    case VT_NET_BACKEND_PROC:    return "/proc/net";
    default:                       return "none";
    }
}

int vt_net_init(vt_net_t *n) {
    if (!n) return VT_ERR_INVAL;
    n->backend = vt_net_detect();
    return VT_OK;
}

static bool _proc_net_online(void) {
    /* Online iff we have a default route */
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) return false;
    char line[512];
    bool online = false;
    /* skip header */
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return false; }
    while (fgets(line, sizeof(line), fp)) {
        char iface[64]; uint32_t dst;
        if (sscanf(line, "%63s %x", iface, &dst) >= 2 && dst == 0) {
            online = true; break;
        }
    }
    fclose(fp);
    return online;
}

bool vt_net_online(vt_net_t *n) {
    if (!n) return false;
    if (n->backend == VT_NET_BACKEND_PROC) return _proc_net_online();
    return n->online;
}
int vt_net_get_signal(vt_net_t *n) {
    if (!n) return -1;
    if (n->backend == VT_NET_BACKEND_PROC) {
        /* parse /proc/net/wireless */
        FILE *fp = fopen("/proc/net/wireless", "r");
        if (!fp) return -1;
        char line[512];
        int sig = -1;
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return -1; }
        char iface[64]; double status, link, level, noise, qual;
        if (sscanf(line, "%63[^:]: %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
                   iface, &status, &link, &level, &noise,
                   &qual, &qual, &qual, &qual, &qual, &qual) >= 3) {
            sig = (int)link;
        }
        fclose(fp);
        return sig;
    }
    return n->signal;
}
const char *vt_net_get_ssid(vt_net_t *n) {
    if (!n) return NULL;
    return n->ssid ? n->ssid : "unknown";
}
const char *vt_net_get_iface(vt_net_t *n) {
    if (!n) return NULL;
    if (n->backend == VT_NET_BACKEND_PROC) {
        /* default-route iface */
        FILE *fp = fopen("/proc/net/route", "r");
        if (!fp) return NULL;
        static char iface[64];
        char line[512];
        if (fgets(line, sizeof(line), fp) && fgets(line, sizeof(line), fp)) {
            uint32_t dst;
            if (sscanf(line, "%63s %x", iface, &dst) >= 2 && dst == 0) {
                fclose(fp);
                return iface;
            }
        }
        fclose(fp);
        return NULL;
    }
    return n->iface ? n->iface : NULL;
}
