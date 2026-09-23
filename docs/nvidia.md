# NVIDIA Setup

Vantage supports the NVIDIA proprietary driver, the open NVIDIA kernel
modules (NVIDIA Open / NVK), and the Nouveau driver.

## Prerequisites

Install the driver per your distribution:

* Arch: `pacman -S nvidia` (or `nvidia-dkms`, or `nouveau` for OSS)
* Debian/Ubuntu: `apt install nvidia-driver firmware-misc-nonfree`
* Fedora: `dnf install akmod-nvidia xorg-x11-drv-nvidia`
* openSUSE: `zypper install x11-video-nvidiaG05`
* Alpine: `apk add nvidia` (proprietary) or `apk add mesa-dri-nouveau`

## Build flags

Make sure meson detects EGL + GL:

```sh
meson setup build
# Confirm the summary shows:
#   egl: true
#   opengl: true
```

If EGL is not detected, install the GLVND packages
(`libglvnd`, `libglvnd0-dev`, etc.).

## X11

```ini
[desktop]
backend=xorg
```

## Wayland

NVIDIA's Wayland support requires driver version 525+ (Turing+).

```ini
[desktop]
backend=wayland
```

Set:
```
GBM_BACKEND=nvidia_drm
__GLX_VENDOR_LIBRARY_NAME=nvidia
```

(Vantage sets `__GLX_VENDOR_LIBRARY_NAME=nvidia` automatically when it
detects NVIDIA + proprietary driver.)

## PRIME / hybrid graphics (NVIDIA + Intel/AMD iGPU)

Vantage respects the standard PRIME setup. To force NVIDIA rendering:

```sh
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia \
__VK_LAYER_NV_optimus=NVIDIA_only \
vantage-session
```

For per-app PRIME offload inside the desktop:

```sh
vantage-config set desktop PRIME 1
```

## EGL Streams vs GBM

Vantage prefers GBM (via `EGL_EXT_platform_device`) on Turing+ for
Wayland. Older cards (Volta and before) with the proprietary driver
require EGL Streams; this is detected automatically.

## Verifying

```sh
vantage-diagnostics
```

Should show:
```
Backend:        wayland
Renderer:       opengl-egl
GPU vendor:     NVIDIA
GPU driver:     nvidia
Hardware accel: ENABLED
```

## Known issues

* EGL Streams on Maxwell may flicker under XWayland. Use the X11 backend
  if you experience this.
* Hybrid Power states (Dynamic Power Management) require kernel 5.9+
  and `nvidia-drm.modeset=1` kernel parameter.
* Multi-renderer (NVIDIA + Nouveau on the same card) is not supported
  by upstream — pick one or the other.
