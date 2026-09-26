# Vantage Dependencies

Vantage is designed to have **as few dependencies as reasonably possible**.
Every dependency must have a reason.

## Build-time

| Tool          | Required? | Used for              |
|---------------|-----------|------------------------|
| meson         | yes       | Build configuration    |
| ninja         | yes       | Build execution        |
| gcc ≥ 11      | yes       | C compiler             |
| pkg-config    | yes       | Dependency detection   |
| wayland-scanner | optional | Wayland protocol codegen |

## Runtime — mandatory

None for the bare-minimum install. Without any optional deps, Vantage
will run in software-rendering headless mode (still useful for
config/diagnostic testing on remote machines).

## Runtime — optional

### Display backends

| Library         | Package (Debian)        | Notes                           |
|-----------------|-------------------------|---------------------------------|
| libxcb          | libxcb1-dev              | X11 backend core                 |
| libxcb-randr    | libxcb-randr0-dev        | RandR multi-monitor             |
| libxkbcommon    | libxkbcommon-dev         | Keyboard layout (X11 + Wayland) |
| libwayland-client | libwayland-dev         | Wayland clients / test tooling   |
| libwayland-server | libwayland-dev         | Native Vantage Wayland compositor |
| libseat           | libseat-dev            | seat/session (logind, elogind, seatd) |
| libinput          | libinput-dev           | compositor input (udev backend)  |
| libudev           | libudev-dev            | input device discovery (see GPU section for gbm/drm) |
| libxcursor        | libxcursor-dev         | compositor + X11 cursor theming  |

### Rendering / GPU

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| libegl         | libegl-dev                | EGL (always required for HW)   |
| libgl          | libgl-dev                 | OpenGL backend                  |
| libglesv2      | libgles2-dev             | OpenGL ES backend (preferred)   |
| libgbm         | libgbm-dev                | GBM (for DRM/KMS)               |
| libdrm         | libdrm-dev                | DRM                             |
| libudev        | libudev-dev               | udev (device enumeration)        |
| vulkan-loader  | libvulkan-dev             | Optional Vulkan backend         |

### NVIDIA-specific notes

Vantage probes for NVIDIA by reading /sys/class/drm/card*/device/vendor
and matching `0x10de`. If the proprietary driver is detected, Vantage
sets `__GLX_VENDOR_LIBRARY_NAME=nvidia` for EGL/GL selection.

For Turing+ GPUs, Vantage prefers the GBM path (via EGL_EXT_platform_device
or EGL_external_platform). For older cards, EGL Streams is the only path
available with the proprietary driver.

The nvk driver (Nouveau's Vulkan rewrite) is detected and used when
preferred by the user via the renderer config option.

### Audio integrations

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| pipewire       | libpipewire-0.3-dev      | Preferred                       |
| libpulse       | libpulse-dev              | PulseAudio (Wayland-compatible) |
| libasound      | libasound2-dev            | ALSA direct (always available)  |

### Network integrations

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| libnm          | libnm-dev                 | NetworkManager                  |
| (none)         |                          | /proc/net fallback (always available) |

### Power / init integrations

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| libsystemd     | libsystemd-dev            | systemd logind                  |
| libelogind     | libelogind-dev            | elogind (systemd-free)           |
| upower-glib    | libupower-glib-dev        | UPower battery/suspend           |
| (none)         |                          | /sys/power/state + reboot(2)    |

### Theme / config / wallpaper

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| json-c         | libjson-c-dev             | Theme JSON parser               |
| gdk-pixbuf     | libgdk-pixbuf2.0-dev      | Image wallpaper loading         |
| libpng         | libpng-dev                | PNG wallpaper                   |
| libjpeg        | libjpeg-dev               | JPEG wallpaper                  |
| libav*         | libavcodec-dev, libavformat-dev, libavutil-dev, libswscale-dev | Video wallpaper |

### D-Bus integration

| Library        | Package (Debian)        | Notes                          |
|----------------|--------------------------|--------------------------------|
| dbus-1         | libdbus-1-dev             | D-Bus (for portal/accessibility) |
| glib-2.0       | libglib2.0-dev            | Used by upower-glib             |

## Distro-specific package matrix

### Arch Linux

```sh
pacman -S meson ninja gcc pkg-config \
          libxcb libxkbcommon wayland wayland-protocols libseat libinput \
          libegl libgl libglvnd libdrm libudev libxcursor \
          json-c gdk-pixbuf2 libpng libjpeg-turbo ffmpeg \
          pipewire libpulse alsa-lib libnm upower \
          dbus libsystemd
```

### Debian / Ubuntu

```sh
apt install meson ninja-build gcc pkg-config \
            libxcb1-dev libxcb-randr0-dev libxkbcommon-dev \
            libwayland-dev wayland-protocols \
            libegl-dev libgl-dev libgles2-mesa-dev \
            libgbm-dev libdrm-dev libudev-dev libseat-dev libinput-dev \
            libjson-c-dev libgdk-pixbuf2.0-dev libpng-dev libjpeg-dev \
            libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
            libpipewire-0.3-dev libpulse-dev libasound2-dev \
            libnm-dev libupower-glib-dev \
            libdbus-1-dev libsystemd-dev
```

### Fedora

```sh
dnf install meson ninja-build gcc pkg-config \
            xcb-util-devel xcb-util-randr-devel libxkbcommon-devel \
            wayland-devel wayland-protocols-devel \
          mesa-libEGL-devel mesa-libGL-devel mesa-libGLES-devel \
            mesa-libgbm-devel libdrm-devel systemd-devel \
            json-c-devel gdk-pixbuf2-devel libpng-devel libjpeg-turbo-devel \
            ffmpeg-devel pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
            NetworkManager-libnm-devel upower-devel \
            dbus-devel systemd-devel
```

### Alpine Linux

```sh
apk add meson ninja gcc pkgconf \
          libxcb-dev libxkbcommon-dev wayland-dev wayland-protocols \
          mesa-dev mesa-gbm libdrm-dev eudev-dev \
          json-c-dev gdk-pixbuf-dev libpng-dev libjpeg-turbo-dev \
          ffmpeg-dev pipewire-dev pulseaudio-dev alsa-lib-dev \
          networkmanager-dev upower-dev \
          dbus-dev elogind-dev
```

### Void Linux

```sh
xbps-install meson ninja gcc pkg-config \
              libxcb-devel libxkbcommon-devel wayland-devel wayland-protocols \
              mesa-devel libdrm-devel libudev-devel \
              json-c-devel gdk-pixbuf-devel libpng-devel libjpeg-turbo-devel \
              ffmpeg-devel pipewire-devel pulseaudio-devel alsa-lib-devel \
              NetworkManager-devel upower-devel \
              dbus-devel elogind-devel
```

### Gentoo

Most deps are in the @world set. Make sure these USE flags are enabled:

```
USE="wayland X egl gles2 vulkan drm udev ffmpeg pipewire pulse alsa networkmanager upower dbus elogind -systemd"
```
