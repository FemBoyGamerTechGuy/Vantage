/*
 * vt-diagnostics.h — Vantage diagnostics
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Produces a single human-readable snapshot of what Vantage sees on the
 * current machine: backend, renderer, GPU, driver, OpenGL version, EGL
 * version, monitors, refresh rates, hardware acceleration, video decode
 * backend. Exposed via the `vantage-diagnostics` binary.
 */
#ifndef VANTAGE_DIAGNOSTICS_H
#define VANTAGE_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdio.h>
#include <vantage/vt-core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostics can probe an explicit backend (matching the
 * vantage-session --wayland | --x11 flags) or auto-detect. */
struct vt_backend;
typedef struct vt_diag {
    char  *backend;
    char  *server;            /* X server implementation, informational */
    char  *renderer;
    char  *gpu_vendor;
    char  *gpu_device;
    char  *gpu_driver;
    char  *gl_version;
    char  *egl_version;
    char  *vulkan_version;
    int    monitor_count;
    int    refresh_hz[8];
    bool   hw_accel;
    char  *accel_reason;      /* honest reason when hw_accel is off */
    bool   vsync;
    bool   video_decode;
    bool   gbm;
    bool   egl;
    char  *video_decoder;
    char  *audio_backend;
    char  *network_backend;
    char  *power_backend;
    char  *init_system;
    bool   dbus;
    /* CLI state */
    int    kind;              /* vt_backend_kind_t, 0 = auto */
    bool   machine;
} vt_diag_t;

vt_diag_t *vt_diag_new(void);
void        vt_diag_free(vt_diag_t *d);
int         vt_diag_run(vt_diag_t *d);
void        vt_diag_print(const vt_diag_t *d, FILE *fp);
void        vt_diag_print_machine(const vt_diag_t *d, FILE *fp);

#ifdef __cplusplus
}
#endif
#endif
