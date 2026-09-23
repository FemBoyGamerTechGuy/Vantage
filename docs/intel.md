# Intel Setup

Vantage uses Mesa (iris/i965 for OpenGL, anv for Vulkan) for Intel GPUs
via the `i915` kernel driver.

## Prerequisites

### Driver packages

| Distro        | Packages                                |
|---------------|------------------------------------------|
| Arch          | `pacman -S mesa lib32-mesa vulkan-intel`  |
| Debian/Ubuntu | `apt install libgl1-mesa-dri mesa-vulkan-drivers` |
| Fedora        | `dnf install mesa-dri-drivers mesa-vulkan-drivers` |
| openSUSE      | `zypper install Mesa`                       |
| Alpine        | `apk add mesa-vulkan-drivers mesa-dri-gallium` |
| Void          | `xbps-install Mesa-vulkan-intel`            |

### Kernel + firmware

* Kernel: `i915` is built-in since mainline 3.x.
* No additional firmware needed for most Intel GPUs.

## X11

```ini
[desktop]
backend=xorg
```

## Wayland

```ini
[desktop]
backend=wayland
```

Intel + Mesa + Wayland is the most-tested combo.

## Video decoding

Install `intel-media-va-driver-non-free` (newer) or
`libva-intel-mfx-driver` (for older Atom CPUs) to enable hardware
H.264/H.265 decoding for video wallpaper.

## Verifying

```sh
vantage-diagnostics
```

Expected:
```
Backend:        wayland
Renderer:       opengl-egl
GPU vendor:     Intel
GPU driver:     i915
Hardware accel: ENABLED
```

## Known issues

* Some older Intel GPUs (Broadwell and before) need
  `i915.enable_guc=2` or `i915.enable_fbc=1` for better perf.
* `anv` (Vulkan) is preferred over `radv` for hybrid Intel+AMD setups.
