/*
 * vantage-renderer.c — Renderer probe / debug tool
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reports what renderer Vantage will pick on the current system.
 */

#define VT_LOG_DOMAIN "renderer"
#include <vantage/vt-core.h>
#include <vantage/vt-renderer.h>
#include <vantage/vt-gpu.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    vt_log_set_level(VT_LOG_INFO);

    printf("Vantage renderer probe\n\n");
    printf("Hardware acceleration available: %s\n",
           vt_gpu_probe_hw_accel() ? "yes" : "no");

    /* Enumerate GPUs */
    vt_gpu_list_t *gpus = vt_gpu_enumerate();
    printf("\nGPUs detected: %zu\n", gpus->n);
    for (size_t i = 0; i < gpus->n; i++) {
        vt_gpu_device_t *d = gpus->devs[i];
        printf("  [%zu] %s:%04x:%04x vendor=%s driver=%s path=%s%s\n",
               i,
               vt_gpu_vendor_str(d->vendor),
               d->vendor_id, d->device_id,
               vt_gpu_driver_str(d->driver),
               d->path,
               d->primary ? " (primary)" : "");
    }
    vt_gpu_device_t *best = vt_gpu_list_best(gpus);
    if (best) {
        printf("\nBest GPU: %s (%s)\n",
               vt_gpu_vendor_str(best->vendor),
               vt_gpu_driver_str(best->driver));
        vt_gpu_setup_env(best);
    }

    /* Probe renderers */
    printf("\nRenderers:\n");
    for (int k = VT_RENDERER_GL; k <= VT_RENDERER_SW; k++) {
        vt_renderer_caps_t caps;
        if (vt_renderer_probe((vt_renderer_kind_t)k, &caps)) {
            printf("  %-10s: %-20s %-20s hw_accel=%s\n",
                   vt_renderer_kind_str((vt_renderer_kind_t)k),
                   caps.vendor, caps.renderer,
                   caps.hw_accel ? "yes" : "no");
        }
    }

    /* Instantiate preferred */
    vt_renderer_t *r = vt_renderer_new(VT_RENDERER_AUTO);
    printf("\nSelected renderer: %s\n", vt_renderer_name(r));
    printf("  Vendor:   %s\n", r->caps.vendor);
    printf("  Renderer: %s\n", r->caps.renderer);
    printf("  Version:  %s\n", r->caps.version);
    printf("  GLSL:     %s\n", r->caps.glsl_version);
    printf("  HW accel: %s\n", r->caps.hw_accel ? "yes" : "no");
    printf("  VSync:    %s\n", r->caps.vsync ? "yes" : "no");
    printf("  Shaders:  %s\n", r->caps.shaders ? "yes" : "no");
    printf("  Max tex:  %d\n", r->caps.max_texture_size);
    vt_renderer_free(r);
    vt_gpu_list_free(gpus);
    return 0;
}
