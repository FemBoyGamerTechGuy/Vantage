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
