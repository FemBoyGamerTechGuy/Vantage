/*
 * vt-gpu.c — GPU enumeration and device selection
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reads /dev/dri/card* and /dev/dri/renderD* nodes, resolves PCI vendor
 * IDs from sysfs, and dispatches to vendor-specific setup (NVIDIA
 * proprietary driver detection, AMD/Intel Mesa detection, etc.).
 *
 * The goal is to detect, at runtime, the best available GPU+driver
 * combination and to prepare EGL platform environment variables so the
 * renderer can pick the right device. This is NOT about choosing between
 * X11 and Wayland — that's the backend's job.
 */

#define VT_LOG_DOMAIN "gpu"
#include <vantage/vt-gpu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

vt_gpu_vendor_t vt_gpu_vendor_from_id(uint16_t id) {
    switch (id) {
    case 0x10de: return VT_GPU_VENDOR_NVIDIA;
    case 0x1002: return VT_GPU_VENDOR_AMD;
    case 0x8086: return VT_GPU_VENDOR_INTEL;
    case 0x15ad: return VT_GPU_VENDOR_VMWARE;
    case 0x80ee: return VT_GPU_VENDOR_VIRTUALBOX;
    case 0x1ab8: return VT_GPU_VENDOR_PARALLELS;
    case 0x1234: return VT_GPU_VENDOR_QEMU;
    case 0x1414: return VT_GPU_VENDOR_MICROSOFT;
    default:     return VT_GPU_VENDOR_UNKNOWN;
    }
}

const char *vt_gpu_vendor_str(vt_gpu_vendor_t v) {
    switch (v) {
    case VT_GPU_VENDOR_NVIDIA:    return "NVIDIA";
    case VT_GPU_VENDOR_AMD:       return "AMD";
    case VT_GPU_VENDOR_INTEL:     return "Intel";
    case VT_GPU_VENDOR_VMWARE:    return "VMware";
    case VT_GPU_VENDOR_VIRTUALBOX:return "VirtualBox";
    case VT_GPU_VENDOR_PARALLELS: return "Parallels";
    case VT_GPU_VENDOR_QEMU:      return "QEMU";
    case VT_GPU_VENDOR_MICROSOFT: return "Microsoft";
    default:                       return "Unknown";
    }
}

const char *vt_gpu_driver_str(vt_gpu_driver_t d) {
    switch (d) {
    case VT_GPU_DRIVER_MESA:      return "mesa";
    case VT_GPU_DRIVER_NVIDIA_PROP:return "nvidia";
    case VT_GPU_DRIVER_NOUVEAU:   return "nouveau";
    case VT_GPU_DRIVER_AMDGPU:    return "amdgpu";
    case VT_GPU_DRIVER_RADEON:    return "radeon";
    case VT_GPU_DRIVER_I915:      return "i915";
    case VT_GPU_DRIVER_NVK:       return "nvk";
    case VT_GPU_DRIVER_VMW:       return "vmw";
    case VT_GPU_DRIVER_VMWGFX:    return "vmwgfx";
    case VT_GPU_DRIVER_VBOX:      return "vboxvideo";
    default:                       return "unknown";
    }
}

static char *_read_file(const char *path) {
    return vt_file_read_all(path, NULL);
}
static char *_read_sysfs_link(const char *path) {
    char buf[1024];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = 0;
    return vt_strdup(buf);
}

static bool _parse_pci_id(const char *s, uint16_t *vendor, uint16_t *device) {
    if (!s) return false;
    unsigned v = 0, d = 0;
    if (sscanf(s, "%x %x", &v, &d) < 2) return false;
    if (vendor) *vendor = (uint16_t)v;
    if (device) *device = (uint16_t)d;
    return true;
}

static vt_gpu_driver_t _detect_driver(const char *sysfs) {
    /* Look at the driver symlink in sysfs */
    if (!sysfs) return VT_GPU_DRIVER_UNKNOWN;
    char *drv_path = vt_path_join(sysfs, "driver");
    char *link = _read_sysfs_link(drv_path);
    vt_free(drv_path);
    if (!link) return VT_GPU_DRIVER_UNKNOWN;
    char *base = vt_file_basename(link);
    vt_gpu_driver_t d = VT_GPU_DRIVER_UNKNOWN;
    if (vt_streq(base, "nvidia"))         d = VT_GPU_DRIVER_NVIDIA_PROP;
    else if (vt_streq(base, "nvidia_drm")) d = VT_GPU_DRIVER_NVIDIA_PROP;
    else if (vt_streq(base, "nouveau"))   d = VT_GPU_DRIVER_NOUVEAU;
    else if (vt_streq(base, "amdgpu"))    d = VT_GPU_DRIVER_AMDGPU;
    else if (vt_streq(base, "radeon"))    d = VT_GPU_DRIVER_RADEON;
    else if (vt_streq(base, "i915"))     d = VT_GPU_DRIVER_I915;
    else if (vt_streq(base, "nvk"))      d = VT_GPU_DRIVER_NVK;
    else if (vt_streq(base, "vmwgfx"))    d = VT_GPU_DRIVER_VMWGFX;
    else if (vt_streq(base, "vboxvideo")) d = VT_GPU_DRIVER_VBOX;
    vt_free(base); vt_free(link);
    return d;
}

static char *_device_name_from_uevent(const char *sysfs) {
    char *p = vt_path_join(sysfs, "uevent");
    char *buf = _read_file(p);
    vt_free(p);
    if (!buf) return NULL;
    char *line = strtok(buf, "\n");
    while (line) {
        if (vt_strstartswith(line, "OF_FULLNAME=") ||
            vt_strstartswith(line, "PCI_SLOT_NAME=")) {
            char *eq = strchr(line, '=');
            char *val = eq ? vt_strdup(eq + 1) : NULL;
            vt_free(buf);
            return val;
        }
        line = strtok(NULL, "\n");
    }
    vt_free(buf);
    return NULL;
}

static char *_device_description(const char *sysfs) {
    /* /sys/class/drm/card0/device/device -> .../0000:XX:XX.X */
    char *p = vt_path_join(sysfs, "device");
    char *sub = _read_sysfs_link(p);
    vt_free(p);
    if (!sub) return NULL;
    char *p2 = vt_path_join(sub, "subsystem_device");
    char *buf = _read_file(p2);
    vt_free(p2); vt_free(sub);
    if (buf) { char *r = vt_strtrim(buf); vt_free(buf); return r; }
    return NULL;
}

static void _probe_node(const char *devpath, vt_gpu_device_t *out) {
    memset(out, 0, sizeof(*out));
    out->path = vt_strdup(devpath);
    /* /dev/dri/card0 -> /sys/class/drm/card0 */
    const char *base = strrchr(devpath, '/');
    base = base ? base + 1 : devpath;
    char *sysfs = vt_strprintf("/sys/class/drm/%s", base);
    out->sysfs = sysfs;
    /* render nodes start with renderD */
    out->render_node = vt_strstartswith(base, "renderD");
    /* cardN -> device -> vendor */
    char *vpath = vt_path_join(sysfs, "device/vendor");
    char *vbuf = _read_file(vpath);
    vt_free(vpath);
    if (vbuf) {
        uint16_t v = 0, d = 0;
        char *dpath = vt_path_join(sysfs, "device/device");
        char *dbuf = _read_file(dpath);
        vt_free(dpath);
        if (_parse_pci_id(vbuf, &v, NULL)) {
            out->vendor_id = v;
            out->vendor = vt_gpu_vendor_from_id(v);
        }
        if (dbuf && _parse_pci_id(dbuf, NULL, &d)) out->device_id = d;
        vt_free(dbuf);
        vt_free(vbuf);
    }
    out->driver = _detect_driver(sysfs);
    out->vendor_str = vt_strdup(vt_gpu_vendor_str(out->vendor));
    out->driver_str = vt_strdup(vt_gpu_driver_str(out->driver));
    out->device_str = _device_description(sysfs);
    if (!out->device_str)
        out->device_str = _device_name_from_uevent(sysfs);
    /* Probing for EGL/GBM/Vulkan happens lazily */
    out->has_gbm = access("/dev/dri/renderD128", F_OK) == 0;
    out->has_egl = false;     /* set by renderer */
    out->has_vulkan = false;
    out->has_egl_stream = (out->vendor == VT_GPU_VENDOR_NVIDIA &&
                            out->driver == VT_GPU_DRIVER_NVIDIA_PROP);
    /* Priority: NVIDIA prop > AMD/Intel Mesa > others */
    out->priority = 10;
    if (out->vendor == VT_GPU_VENDOR_NVIDIA) {
        if (out->driver == VT_GPU_DRIVER_NVIDIA_PROP) out->priority = 100;
        else if (out->driver == VT_GPU_DRIVER_NVK) out->priority = 80;
    } else if (out->vendor == VT_GPU_VENDOR_AMD) {
        out->priority = 90;
    } else if (out->vendor == VT_GPU_VENDOR_INTEL) {
        out->priority = 85;
    }
    if (out->render_node) out->priority -= 5;  /* primary card preferred */
}

vt_gpu_list_t *vt_gpu_enumerate(void) {
    vt_gpu_list_t *l = vt_malloc0(sizeof(*l));
    l->devs = NULL; l->n = 0; l->best = -1;
    DIR *d = opendir("/dev/dri");
    if (!d) {
        vt_logi("gpu: /dev/dri not available — no KMS / GPU detected");
        return l;
    }
    size_t cap = 8;
    l->devs = vt_malloc(sizeof(vt_gpu_device_t *) * cap);
    struct dirent *e;
    int best_pri = -1;
    while ((e = readdir(d))) {
        if (!vt_strstartswith(e->d_name, "card") &&
            !vt_strstartswith(e->d_name, "renderD")) continue;
        /* skip card0-DP-1 etc. (connector entries) */
        if (strchr(e->d_name, '-')) continue;
        if (l->n + 1 >= cap) {
            cap *= 2;
            l->devs = vt_realloc(l->devs, sizeof(vt_gpu_device_t *) * cap);
        }
        char *path = vt_strprintf("/dev/dri/%s", e->d_name);
        vt_gpu_device_t *dev = vt_malloc0(sizeof(*dev));
        _probe_node(path, dev);
        l->devs[l->n] = dev;
        if (dev->priority > best_pri) {
            best_pri = dev->priority;
            l->best = (int)l->n;
        }
        if (vt_strstartswith(e->d_name, "card0") && !dev->render_node)
            dev->primary = true;
        l->n++;
        vt_free(path);
    }
    closedir(d);
    return l;
}

void vt_gpu_list_free(vt_gpu_list_t *l) {
    if (!l) return;
    for (size_t i = 0; i < l->n; i++) {
        vt_gpu_device_t *d = l->devs[i];
        if (!d) continue;
        vt_free(d->path); vt_free(d->sysfs);
        vt_free(d->vendor_str); vt_free(d->device_str);
        vt_free(d->driver_str);
        vt_free(d);
    }
    vt_free(l->devs);
    vt_free(l);
}
vt_gpu_device_t *vt_gpu_list_best(vt_gpu_list_t *l) {
    if (!l || l->best < 0 || (size_t)l->best >= l->n) return NULL;
    return l->devs[l->best];
}

int vt_gpu_setup_env(const vt_gpu_device_t *d) {
    if (!d) return VT_ERR_INVAL;
    /* Hint EGL to use GBM platform on the device's render node */
    if (d->render_node || d->path) {
        setenv("DRI_PRIME", "1", 0);  /* no-op for single GPU but consistent */
        /* Use GBM/EGL platform: __EGL_VENDOR_LIBRARY_FILENAMES etc */
        if (d->vendor == VT_GPU_VENDOR_NVIDIA &&
            d->driver == VT_GPU_DRIVER_NVIDIA_PROP) {
            /* NVIDIA prefers EGL_EXT_platform_device or EGL_EXTERNAL_PLATFORM */
            setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
        }
    }
    return VT_OK;
}

bool vt_gpu_probe_hw_accel(void) {
    /* Try opening /dev/dri/renderD128 — simplest check */
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        vt_logi("gpu: no /dev/dri/renderD128 — software renderer will be used");
        return false;
    }
    close(fd);
    return true;
}

int vt_gpu_nvidia_select_path(vt_gpu_device_t *d) {
    /* For Turing+ NVIDIA, GBM is preferred (via EGL_EXT_platform_device).
     * For older cards (Volta and before) with proprietary driver, EGL Streams
     * is the only path. We don't have a reliable way to detect this without
     * EGL, so we return 0 (prefer GBM path) for now. The renderer can fall
     * back if needed.
     */
    if (!d) return VT_ERR_INVAL;
    if (d->vendor != VT_GPU_VENDOR_NVIDIA) return VT_ERR_INVAL;
    return 0;  /* prefer GBM path */
}
