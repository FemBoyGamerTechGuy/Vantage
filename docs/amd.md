# AMD Setup

Vantage uses the Mesa stack (radeonsi for OpenGL, radv for Vulkan) for
AMD GPUs via the `amdgpu` kernel driver.

## Prerequisites

### Driver packages

| Distro        | Open-source                                          |
|---------------|------------------------------------------------------|
| Arch          | `pacman -S mesa lib32-mesa vulkan-radeon lib32-vulkan-radeon` |
| Debian/Ubuntu | `apt install mesa-vulkan-drivers libgl1-mesa-dri`    |
| Fedora        | `dnf install mesa-vulkan-drivers mesa-dri-drivers`   |
| openSUSE      | `zypper install Mesa-vulkan-radeon`                  |
| Alpine        | `apk add mesa-vulkan-drivers mesa-dri-gallium`       |
| Void          | `xbps-install Mesa-vulkan-radeon`                     |

### Kernel + firmware

* Kernel: `amdgpu` is built in for mainline 4.10+.
* Firmware: install `linux-firmware` (Debian/Ubuntu) or `firmware-amd-graphics`.

## X11

```ini
[desktop]
backend=xorg
```

No special config required. AMD + Mesa + libGL works out of the box.

## Wayland

```ini
[desktop]
backend=wayland
```

Mesa + Wayland requires the GBM platform. Vantage automatically uses
the GBM platform when the EGL_MESA_platform_gbm extension is available.

## Video decoding

For video wallpaper acceleration, install `libva-mesa-driver` (or
`mesa-va-drivers` depending on distro). Vantage will use it through
ffmpeg's VAAPI backend.

## Verifying

```sh
vantage-diagnostics
```

Expected:
```
Backend:        wayland
Renderer:       opengl-egl
GPU vendor:     AMD
GPU driver:     amdgpu
Hardware accel: ENABLED
```

## Known issues

* Southern Islands (HD 7000) requires `radeon.cik_support=0 radeon.si_support=1` for `radeonsi`.
* Stutter with TearFree — use the Vantage compositor's vsync instead.
