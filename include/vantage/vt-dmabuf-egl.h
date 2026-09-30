/*
 * vt-dmabuf-egl.h — dma-buf import engine (EGL) for the Wayland backend
 *
 * SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
 *
 * Acquires an EGL display + GLES context for importing CLIENT dma-bufs
 * (zwp_linux_dmabuf_v1 buffers) as GL textures and reading them back
 * into the compositor's CPU framebuffer:
 *
 *   real hardware: EGL display on the DRM RENDER node via GBM — the
 *     same device clients allocate on, so eglCreateImageKHR does a
 *     real GEM import through the driver. Applications render with
 *     the actual GPU; the compositor reads the RESULT back.
 *   headless/sandbox (no /dev/dri): EGL device platform — Mesa's
 *     software EGL device (llvmpipe). The full import→texture→
 *     glReadPixels path still runs (verified: a memfd-backed buffer
 *     round-trips byte-exact), so the code path is exercised even
 *     without a GPU, and on a GPU machine the identical code runs
 *     against the real driver.
 */
#ifndef VT_DMABUF_EGL_H
#define VT_DMABUF_EGL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef struct vt_dmabuf_image vt_dmabuf_image_t;

/* One-time init/fini. Init is idempotent; returns 0 when an importable
 * EGL context exists, -1 when EGL is unavailable (the dmabuf protocol
 * module then stays unadvertised — clients keep using wl_shm). */
int  vt_dmabuf_egl_init(void);
void vt_dmabuf_egl_fini(void);
bool vt_dmabuf_egl_available(void);

/* dev_t of the DRM render node backing the EGL display (0 = unknown /
 * software device) — the linux-dmabuf v4 feedback main_device. */
dev_t vt_dmabuf_egl_device(void);
/* GL_RENDERER string for logging ("llvmpipe", "Radeon RX…", …). */
const char *vt_dmabuf_egl_renderer(void);

/* Import a dma-buf layout (up to 4 planes) as an EGLImage-backed
 * texture. `fds` are NOT closed by the callee and must stay open for
 * the lifetime of the returned image. modifier = DRM_FORMAT_MOD_*.
 * Returns NULL when this layout cannot be imported. */
vt_dmabuf_image_t *vt_dmabuf_import(const int *fds, const uint32_t *offsets,
                                    const uint32_t *strides,
                                    uint64_t modifier, uint32_t w, uint32_t h,
                                    uint32_t fourcc, int n_planes);

/* Read the image back into an ARGB8888 CPU buffer (dest_stride in
 * uint32 units), converting from the source fourcc and Y-flipping
 * (GL's origin is bottom-left, dma-buf rows are top-down; a
 * Y_INVERT-flagged buffer is already bottom-up). The image must not
 * be used concurrently. Returns false on GL failure. */
bool vt_dmabuf_readback_argb(vt_dmabuf_image_t *img, uint32_t *dest,
                             int dest_stride_u32, bool y_invert);

void vt_dmabuf_image_free(vt_dmabuf_image_t *img);

#endif /* VT_DMABUF_EGL_H */
