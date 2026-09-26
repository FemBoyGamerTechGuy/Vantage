/*
 * vt-integ-power.c — Power/suspend/shutdown/reboot hooks
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Probes: logind (systemd) > elogind > UPower > direct /sys/power/state
 * + reboot(2)/shutdown(2). Works on systems without systemd/elogind.
 *
 * Battery level: reads /sys/class/power_supply/BATxxx/capacity.
 */

#define VT_LOG_DOMAIN "integ-power"
#include <vantage/vt-integrations.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

struct vt_power {
    vt_power_backend_t backend;
};

vt_power_t *vt_power_new(void) { return vt_malloc0(sizeof(vt_power_t)); }
void vt_power_free(vt_power_t *p) { vt_free(p); }

vt_power_backend_t vt_power_detect(void) {
    /* If logind is reachable, prefer it. */
    if (access("/run/systemd/system", F_OK) == 0) return VT_POWER_LOGIND;
    if (access("/run/elogind.pid", F_OK) == 0)    return VT_POWER_LOGIND;
    /* Try /proc/1/comm == "elogind" or "systemd" */
    FILE *fp = fopen("/proc/1/comm", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            char *p = strchr(buf, '\n'); if (p) *p = 0;
            if (vt_streq(buf, "systemd") || vt_streq(buf, "elogind")) {
                fclose(fp); return VT_POWER_LOGIND;
            }
        }
        fclose(fp);
    }
    if (access("/sys/class/power_supply", F_OK) == 0)
        return VT_POWER_IOCTL_ACPI;
    return VT_POWER_NONE;
}

const char *vt_power_backend_str(vt_power_backend_t b) {
    switch (b) {
    case VT_POWER_LOGIND: return "logind";
    case VT_POWER_UPOWER: return "UPower";
    case VT_POWER_IOCTL_ACPI: return "direct-acpi";
    default:                return "none";
    }
}

int vt_power_init(vt_power_t *p) {
    if (!p) return VT_ERR_INVAL;
    p->backend = vt_power_detect();
    vt_logi("power: backend=%s", vt_power_backend_str(p->backend));
    return VT_OK;
}

bool vt_power_can_suspend(vt_power_t *p) {
    (void)p;
    return access("/sys/power/state", W_OK) == 0;
}
bool vt_power_can_hibernate(vt_power_t *p) {
    (void)p;
    if (access("/sys/power/state", W_OK) != 0) return false;
    FILE *fp = fopen("/sys/power/state", "r");
    if (!fp) return false;
    char buf[256] = {0};
    bool ok = false;
    if (fgets(buf, sizeof(buf), fp)) ok = strstr(buf, "disk") != NULL;
    fclose(fp);
    return ok;
}

static int _direct_suspend(void) {
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd < 0) return -1;
    int r = write(fd, "mem", 3) == 3 ? 0 : -1;
    close(fd);
    return r;
}
static int _direct_hibernate(void) {
    int fd = open("/sys/power/state", O_WRONLY);
    if (fd < 0) return -1;
    int r = write(fd, "disk", 4) == 4 ? 0 : -1;
    close(fd);
    return r;
}

int vt_power_suspend(vt_power_t *p) {
    (void)p;
    if (system("systemctl suspend 2>/dev/null") == 0) return 0;
    if (system("loginctl suspend 2>/dev/null") == 0) return 0;
    return _direct_suspend();
}
int vt_power_hibernate(vt_power_t *p) {
    (void)p;
    if (system("systemctl hibernate 2>/dev/null") == 0) return 0;
    return _direct_hibernate();
}
int vt_power_shutdown(vt_power_t *p) {
    (void)p;
    if (system("systemctl poweroff 2>/dev/null") == 0) return 0;
    if (system("shutdown -h now 2>/dev/null") == 0) return 0;
    sync();
    return reboot(LINUX_REBOOT_CMD_POWER_OFF);
}
int vt_power_reboot(vt_power_t *p) {
    (void)p;
    if (system("systemctl reboot 2>/dev/null") == 0) return 0;
    if (system("shutdown -r now 2>/dev/null") == 0) return 0;
    sync();
    return reboot(LINUX_REBOOT_CMD_RESTART);
}
int vt_power_logout(vt_power_t *p) {
    (void)p;
    /* Logout is handled by session manager itself */
    return 0;
}

int vt_power_battery_pct(vt_power_t *p) {
    (void)p;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    int best = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!vt_strstartswith(e->d_name, "BAT")) continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int v = -1;
        if (fscanf(fp, "%d", &v) == 1 && v > best) best = v;
        fclose(fp);
    }
    closedir(d);
    return best;
}
bool vt_power_on_ac(vt_power_t *p) {
    (void)p;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return true;
    bool ac = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!vt_strstartswith(e->d_name, "AC")) continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", e->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int v = 0;
        if (fscanf(fp, "%d", &v) == 1 && v) ac = true;
        fclose(fp);
    }
    closedir(d);
    return ac;
}

vt_init_kind_t vt_init_detect(void) {
    /* Check init system by looking at /proc/1/comm */
    FILE *fp = fopen("/proc/1/comm", "r");
    if (!fp) return VT_INIT_UNKNOWN;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
        fclose(fp);
        if (vt_streq(buf, "systemd")) return VT_INIT_SYSTEMD;
        if (vt_streq(buf, "init") || vt_streq(buf, "busybox")) return VT_INIT_SYSV;
        if (vt_streq(buf, "runit")) return VT_INIT_RUNIT;
        if (vt_streq(buf, "s6-svscan")) return VT_INIT_S6;
        if (vt_streq(buf, "openrc-init")) return VT_INIT_OPENRC;
    } else {
        fclose(fp);
    }
    return VT_INIT_UNKNOWN;
}
const char *vt_init_str(vt_init_kind_t k) {
    switch (k) {
    case VT_INIT_SYSTEMD:  return "systemd";
    case VT_INIT_OPENRC:   return "OpenRC";
    case VT_INIT_S6:      return "s6";
    case VT_INIT_RUNIT:    return "runit";
    case VT_INIT_SYSV:     return "sysv";
    case VT_INIT_MBUS:     return "m-bus";
    case VT_INIT_BUSYBOX:  return "busybox";
    default:                return "unknown";
    }
}
