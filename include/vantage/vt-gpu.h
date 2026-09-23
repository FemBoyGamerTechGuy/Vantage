/*
 * vt-gpu.h — GPU detection and acceleration hooks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Vantage enumerates available GPUs from /dev/dri/card* and render nodes,
 * reads the PCI vendor id, and dispatches to a vendor-specific path:
 *   - NVIDIA  (Turing+ / proprietary driver / EGL Streams + GBM alternative)
 *   - AMD     (Mesa radeonsi/radv, DRM, GBM)
 *   - Intel   (Mesa iris/anv, DRM, GBM)
 *   - VirtualBox/VMware/QEMU/VESA fallback
 *
 * The renderer picks an EGL/OpenGL device that matches the requested GPU.
 */
#ifndef VANTAGE_GPU_H
#define VANTAGE_GPU_H

#include <stdint.h>
#include <stdbool.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VT_GPU_VENDOR_UNKNOWN  = 0,
    VT_GPU_VENDOR_NVIDIA   = 0x10de,
    VT_GPU_VENDOR_AMD      = 0x1002,
    VT_GPU_VENDOR_INTEL    = 0x8086,
    VT_GPU_VENDOR_VMWARE   = 0x15ad,
    VT_GPU_VENDOR_VIRTUALBOX = 0x80ee,
    VT_GPU_VENDOR_PARALLELS = 0x1ab8,
    VT_GPU_VENDOR_QEMU     = 0x1234,
    VT_GPU_VENDOR_MICROSOFT= 0x1414,
} vt_gpu_vendor_t;

typedef enum {
    VT_GPU_DRIVER_UNKNOWN = 0,
    VT_GPU_DRIVER_MESA,
    VT_GPU_DRIVER_NVIDIA_PROP,
    VT_GPU_DRIVER_NOUVEAU,
    VT_GPU_DRIVER_AMDGPU,
    VT_GPU_DRIVER_RADEON,
    VT_GPU_DRIVER_I915,
    VT_GPU_DRIVER_NVK,
    VT_GPU_DRIVER_VMW,
    VT_GPU_DRIVER_VMWGFX,
    VT_GPU_DRIVER_VBOX,
} vt_gpu_driver_t;

typedef struct vt_gpu_device {
    char      *path;            /* /dev/dri/card0 / renderD128 */
    char      *sysfs;          /* sysfs path */
    uint16_t   vendor_id;
    uint16_t   device_id;
    vt_gpu_vendor_t vendor;
    vt_gpu_driver_t driver;
    char      *vendor_str;
    char      *device_str;
    char      *driver_str;
    bool       render_node;
    bool       primary;
    bool       has_gbm;
    bool       has_egl;
    bool       has_vulkan;
    bool       has_egl_stream;  /* NVIDIA-specific */
    int        priority;        /* selection priority (higher = better) */
} vt_gpu_device_t;

typedef struct vt_gpu_list {
    vt_gpu_device_t **devs;
    size_t            n;
    int               best;     /* index of the chosen device */
} vt_gpu_list_t;

vt_gpu_list_t *vt_gpu_enumerate(void);
void            vt_gpu_list_free(vt_gpu_list_t *l);
vt_gpu_device_t *vt_gpu_list_best(vt_gpu_list_t *l);

/* Quick shorthand for diagnostics */
const char    *vt_gpu_vendor_str(vt_gpu_vendor_t v);
const char    *vt_gpu_driver_str(vt_gpu_driver_t d);
vt_gpu_vendor_t vt_gpu_vendor_from_id(uint16_t id);

/* Set environment hints for the EGL/GL platform to use a specific device.
 * Returns 0 on success, <0 if the requested device is unavailable. */
int  vt_gpu_setup_env(const vt_gpu_device_t *d);

/* Try to determine whether hardware acceleration is currently active
 * by probing EGL with the requested device. */
bool vt_gpu_probe_hw_accel(void);

/* NVIDIA-specific EGL stream vs GBM selection — invoked from the renderer
 * if a Turing+ device is detected. Returns 0 if EGL streams path is in use,
 * <0 if GBM path should be used instead. */
int  vt_gpu_nvidia_select_path(vt_gpu_device_t *d);

#ifdef __cplusplus
}
#endif
#endif
