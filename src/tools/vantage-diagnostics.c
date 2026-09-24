/*
 * vantage-diagnostics.c — Diagnostic dump
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Prints a snapshot of Vantage's view of the current system: backend,
 * renderer, GPU, OpenGL version, EGL version, monitors, refresh rates,
 * hardware acceleration, video decode backend, audio/network/power.
 *
 * Useful for bug reports and for verifying that Vantage is actually
 * using the GPU.
 */

#define VT_LOG_DOMAIN "diag"
#include <vantage/vt-core.h>
#include <vantage/vt-diagnostics.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-gpu.h>
#include <vantage/vt-backend.h>
#include <vantage/vt-integrations.h>
#include <vantage/vt-config.h>
#include <vantage/vt-wallpaper.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

vt_diag_t *vt_diag_new(void) {
    return vt_malloc0(sizeof(vt_diag_t));
}
void vt_diag_free(vt_diag_t *d) {
    if (!d) return;
    vt_free(d->backend); vt_free(d->renderer);
    vt_free(d->gpu_vendor); vt_free(d->gpu_device);
    vt_free(d->gpu_driver); vt_free(d->gl_version);
    vt_free(d->egl_version); vt_free(d->vulkan_version);
    vt_free(d->video_decoder); vt_free(d->audio_backend);
    vt_free(d->network_backend); vt_free(d->power_backend);
    vt_free(d->init_system);
    vt_free(d);
}
int vt_diag_run(vt_diag_t *d) {
    if (!d) return VT_ERR_INVAL;
    /* GPU enumeration */
    vt_gpu_list_t *gpus = vt_gpu_enumerate();
    vt_gpu_device_t *best = vt_gpu_list_best(gpus);
    if (best) {
        d->gpu_vendor = vt_strdup(vt_gpu_vendor_str(best->vendor));
        d->gpu_driver = vt_strdup(vt_gpu_driver_str(best->driver));
        d->gpu_device = best->device_str ? vt_strdup(best->device_str) :
                                            vt_strprintf("0x%04x", best->device_id);
        d->hw_accel = best->has_gbm || best->has_egl_stream || vt_gpu_probe_hw_accel();
        d->gbm = best->has_gbm;
        d->egl = best->has_egl;
    } else {
        d->gpu_vendor = vt_strdup("none");
        d->gpu_driver = vt_strdup("none");
        d->gpu_device = vt_strdup("none");
        d->hw_accel = false;
    }
    vt_gpu_list_free(gpus);

    /* Renderer probe */
    vt_renderer_t *r = vt_renderer_new(VT_RENDERER_AUTO);
    d->renderer = vt_strdup(vt_renderer_name(r));
    d->gl_version = vt_strdup(r->caps.version);
    d->egl_version = vt_strdup(r->caps.version);
    d->vsync = r->caps.vsync;
    d->video_decode = false;  /* would need ffmpeg probe */
    /* The renderer probe is authoritative for EGL: if the GL renderer
     * initialized an EGL display + context, EGL is available even when
     * no /dev/dri node exists (e.g. NVIDIA proprietary on X11 without
     * nvidia-drm modeset). Hardware accel additionally requires that the
     * rasterizer is not a CPU fallback (llvmpipe/softpipe/swrast). */
    if (r->kind == VT_RENDERER_GL && r->caps.hw_accel) {
        d->egl = true;
        const char *rr = r->caps.renderer;
        bool software_gl = strstr(rr, "llvmpipe") || strstr(rr, "softpipe") ||
                           strstr(rr, "swrast")  || strstr(rr, "Software") ||
                           strstr(rr, "software");
        if (!software_gl)
            d->hw_accel = true;
    }
#if defined(VT_HAVE_FFMPEG)
    d->video_decoder = vt_strdup("ffmpeg");
    d->video_decode = true;
#else
    d->video_decoder = vt_strdup("disabled");
#endif
    vt_renderer_free(r);

    /* Backend */
    vt_backend_t *b = vt_backend_new(VT_BACKEND_AUTO);
    d->backend = vt_strdup(vt_backend_name(b));
    d->monitor_count = (int)vt_backend_output_count(b);
    for (size_t i = 0; i < vt_backend_output_count(b) && i < 8; i++) {
        const vt_output_t *o = vt_backend_output_at(b, i);
        d->refresh_hz[i] = o->refresh_hz;
    }
    vt_backend_free(b);

    /* Integrations */
    vt_audio_t *a = vt_audio_new();
    vt_audio_init(a);
    d->audio_backend = vt_strdup(vt_audio_backend_str(vt_audio_detect()));
    vt_audio_free(a);

    vt_net_t *n = vt_net_new();
    vt_net_init(n);
    d->network_backend = vt_strdup(vt_net_backend_str(vt_net_detect()));
    vt_net_free(n);

    vt_power_t *p = vt_power_new();
    vt_power_init(p);
    d->power_backend = vt_strdup(vt_power_backend_str(vt_power_detect()));
    vt_power_free(p);

    d->init_system = vt_strdup(vt_init_str(vt_init_detect()));
    d->dbus = vt_dbus_available();
    return VT_OK;
}

void vt_diag_print(const vt_diag_t *d, FILE *fp) {
    fprintf(fp, "Vantage Diagnostics\n");
    fprintf(fp, "===================\n");
    fprintf(fp, "Backend:        %s\n", d->backend ? d->backend : "?");
    fprintf(fp, "Renderer:       %s\n", d->renderer ? d->renderer : "?");
    fprintf(fp, "GPU vendor:     %s\n", d->gpu_vendor ? d->gpu_vendor : "?");
    fprintf(fp, "GPU device:     %s\n", d->gpu_device ? d->gpu_device : "?");
    fprintf(fp, "GPU driver:     %s\n", d->gpu_driver ? d->gpu_driver : "?");
    fprintf(fp, "GL version:     %s\n", d->gl_version ? d->gl_version : "?");
    fprintf(fp, "EGL version:    %s\n", d->egl_version ? d->egl_version : "?");
    fprintf(fp, "Vulkan version: %s\n", d->vulkan_version ? d->vulkan_version : "n/a");
    fprintf(fp, "Hardware accel: %s\n", d->hw_accel ? "ENABLED" : "disabled");
    fprintf(fp, "GBM:             %s\n", d->gbm ? "yes" : "no");
    fprintf(fp, "EGL:             %s\n", d->egl ? "yes" : "no");
    fprintf(fp, "VSync:           %s\n", d->vsync ? "ENABLED" : "disabled");
    fprintf(fp, "Video decode:    %s\n", d->video_decode ? "ENABLED" : "disabled");
    fprintf(fp, "Video decoder:   %s\n", d->video_decoder ? d->video_decoder : "?");
    fprintf(fp, "Monitors:        %d\n", d->monitor_count);
    for (int i = 0; i < d->monitor_count && i < 8; i++)
        fprintf(fp, "  [%d] refresh = %d Hz\n", i, d->refresh_hz[i]);
    fprintf(fp, "Audio:           %s\n", d->audio_backend ? d->audio_backend : "?");
    fprintf(fp, "Network:        %s\n", d->network_backend ? d->network_backend : "?");
    fprintf(fp, "Power:          %s\n", d->power_backend ? d->power_backend : "?");
    fprintf(fp, "Init system:    %s\n", d->init_system ? d->init_system : "?");
    fprintf(fp, "D-Bus:          %s\n", d->dbus ? "available" : "unavailable");
}

void vt_diag_print_machine(const vt_diag_t *d, FILE *fp) {
    /* machine-readable for scripts */
    fprintf(fp, "vantage.backend=%s\n", d->backend ? d->backend : "?");
    fprintf(fp, "vantage.renderer=%s\n", d->renderer ? d->renderer : "?");
    fprintf(fp, "vantage.gpu_vendor=%s\n", d->gpu_vendor ? d->gpu_vendor : "?");
    fprintf(fp, "vantage.gpu_driver=%s\n", d->gpu_driver ? d->gpu_driver : "?");
    fprintf(fp, "vantage.hw_accel=%s\n", d->hw_accel ? "1" : "0");
    fprintf(fp, "vantage.monitors=%d\n", d->monitor_count);
    fprintf(fp, "vantage.audio=%s\n", d->audio_backend ? d->audio_backend : "?");
    fprintf(fp, "vantage.network=%s\n", d->network_backend ? d->network_backend : "?");
    fprintf(fp, "vantage.power=%s\n", d->power_backend ? d->power_backend : "?");
    fprintf(fp, "vantage.init=%s\n", d->init_system ? d->init_system : "?");
    fprintf(fp, "vantage.dbus=%s\n", d->dbus ? "1" : "0");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vt_log_set_level(VT_LOG_INFO);
    vt_diag_t *d = vt_diag_new();
    vt_diag_run(d);
    if (argc > 1 && strcmp(argv[1], "--machine") == 0)
        vt_diag_print_machine(d, stdout);
    else
        vt_diag_print(d, stdout);
    vt_diag_free(d);
    return 0;
}
