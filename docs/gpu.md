# Vantage GPU Acceleration

Vantage probes available GPUs from `/dev/dri/card*` and `/dev/dri/renderD*`,
reads the PCI vendor ID from sysfs, dispatches to the right driver path:

| Vendor  | PCI ID  | Driver path               |
|---------|---------|---------------------------|
| NVIDIA  | 0x10de  | nvidia (proprietary) or nvk (Vulkan) |
| AMD     | 0x1002  | amdgpu or radeon (Mesa)   |
| Intel   | 0x8086  | i915 (Mesa)               |
| VMware  | 0x15ad  | vmwgfx                     |
| VirtualBox | 0x80ee | vboxvideo                 |

## Renderer selection priority

1. **OpenGL/EGL** (if available) — preferred for performance, broad compat.
2. **Vulkan** (if enabled) — optional, advanced; not required.
3. **Software** (always) — fallback when no GPU is available.

## Environment setup

When a GPU is detected, Vantage sets:

* `__GLX_VENDOR_LIBRARY_NAME=nvidia` (for proprietary NVIDIA)
* `DRI_PRIME=1` (no-op for single GPU, hints EGL which device to use)
* `QT_QPA_PLATFORM=xcb;wayland` (so Qt apps pick up the right backend)

For multi-GPU (PRIME / hybrid graphics), Vantage prefers the dedicated
GPU for rendering and the integrated GPU for display, similar to
standard Mesa PRIME behavior.

## Verifying hardware acceleration

Run:

```sh
vantage-diagnostics
```

Look for:
```
Hardware accel: ENABLED
Renderer:       opengl-egl
GPU vendor:     NVIDIA
```

If `Renderer: software`, then either no GPU is detected or EGL/GL
libraries are not linked in. Check that `meson setup` printed `egl: true`
and `opengl: true` in the build summary.

## VSync

Vsync is on by default for the OpenGL renderer. The compositor calls
`eglSwapBuffers` with `EGL_SWAP_BEHAVIOR_PRESERVED`. To disable:

```ini
[desktop]
vsync=false
```

## Client-side hardware acceleration (zwp_linux_dmabuf_v1)

Apps render on the GPU too, not just the compositor. Vantage advertises
`zwp_linux_dmabuf_v1` v4 so GPU clients (browsers, games, EGL/Vulkan
toolkits, and Xwayland/glamor for X11 GL apps) can hand their
rendered buffers to the compositor as dma-bufs:

```
app renders → EGL/Vulkan on the GPU driver
            → wl_buffer backed by a dma-buf
            → compositor imports it (EGL/GBM on the render node)
            → readback into the composition surface → screen
```

* Formats: ARGB/XRGB/ABGR/XBGR8888, LINEAR and INVALID modifier
  (INVALID maps to the driver's implicit layout).
* v4 feedback is honored: the main device (`dev_t` of the render node)
  and the format/modifier table are sent via the feedback event, so
  clients allocate on the SAME GPU the compositor scans out from.
* Import ladder: EGL image import → readback (`glReadPixels`, Y-flip,
  format convert) → compositor composition; mmap fallback for LINEAR
  buffers when EGL is unavailable; failed imports release the buffer
  immediately (clients fall back to wl_shm, never stall).
* The buffer is released right after readback: the client's swapchain
  never stalls on the compositor.
* On machines without `/dev/dri`, the EGL import engine uses Mesa's
  software EGL device (llvmpipe) so the protocol path is still
  exercised end-to-end; with a GPU present the same code imports from
  the real render node via GBM.

Verify from `vantage-diagnostics`:
```
dmabuf: zwp_linux_dmabuf_v1 v4 advertised — import renderer '<name>'
```

## Frame pacing (wl_surface.frame)

Frame callbacks are the compositor's frame clock: they fire every loop
tick (paced to ~60 Hz, `VANTAGE_FRAME_INTERVAL_US` to tune) with a real
timestamp, on a global list that survives surface unmapping, and the
`done` reaches the wire in the same iteration it was marshaled. The
callback resource destruction is deferred one tick so `done` and
`wl_display.delete_id` never share a client dispatch batch.
