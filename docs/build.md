# Building Vantage

## Required build tools

* meson ≥ 0.59
* ninja ≥ 1.10
* gcc ≥ 11 (C17 support required)
* pkg-config

Install on Debian/Ubuntu: `apt install meson ninja-build gcc pkg-config`

Install on Arch: `pacman -S meson ninja gcc pkgconf`

Install on Alpine: `apk add meson ninja gcc pkgconf`

Install on Void: `xbps-install meson ninja gcc pkg-config`

## Required runtime libraries

None for the core. The desktop will run in software-rendering, headless
mode without any optional deps.

## Optional dependencies (recommended for full feature set)

| Feature                  | Package(s)                |
|--------------------------|---------------------------|
| X11 backend              | libxcb, xcb-randr         |
| Wayland backend          | wayland-client, wayland-protocols |
| Hardware rendering       | libegl, libgl or libglesv2 |
| Vulkan renderer          | vulkan-loader              |
| GPU detection            | libdrm, libudev            |
| xkb keyboard layouts     | libxkbcommon               |
| Live video wallpaper     | libavcodec, libavformat, libavutil, libswscale |
| Image wallpapers         | gdk-pixbuf-2.0, libpng, libjpeg |
| Audio integration        | pipewire (preferred), or libpulse, or libasound |
| Network status           | libnm (preferred), or /proc fallback |
| Power integration       | libsystemd OR libelogind OR direct /sys |
| UPower battery           | upower-glib                |
| D-Bus integration        | dbus-1                     |

## Build steps

```sh
git clone https://github.com/FemBoyGamerTechGuy/Vantage
cd Vantage
meson setup build
ninja -C build
```

## Run the test suite

```sh
meson test -C build
```

## Install system-wide

```sh
sudo ninja -C build install
```

This installs:

* binaries in `/usr/local/bin/`
* session desktop file in `/usr/local/share/xsessions/`
* themes in `/usr/local/share/vantage/themes/`
* default config in `/usr/local/etc/vantage/`
* headers in `/usr/local/include/vantage-0.1/`
* libraries in `/usr/local/lib/`

## Configuration

The user config file is `~/.config/vantage/vantage.conf`. To generate
a default config:

```sh
vantage-config init
```

## Build options

| Option              | Default | Description                          |
|---------------------|---------|--------------------------------------|
| x11                 | auto    | Build X11 backend                    |
| xlibre              | auto    | Build XLibre backend                  |
| wayland             | auto    | Build Wayland backend                |
| opengl              | auto    | Build OpenGL/EGL renderer            |
| vulkan              | disabled| Build Vulkan renderer (advanced)    |
| software            | enabled | Always build software renderer      |
| video-wallpaper     | auto    | Build ffmpeg video wallpaper support |
| dbus                | auto    | Build D-Bus integration              |
| systemd             | auto    | Build libsystemd integration         |
| elogind             | auto    | Build elogind integration            |
| pipewire            | auto    | Build PipeWire audio integration     |
| pulseaudio          | auto    | Build PulseAudio audio integration   |
| alsa                | auto    | Build ALSA audio integration         |
| networkmanager      | auto    | Build NetworkManager integration    |
| upower              | auto    | Build UPower integration             |
| tests               | enabled | Build unit tests                     |
| docs                | enabled | Install documentation               |

Example: build with minimal feature set for headless use:

```sh
meson setup build -Dwayland=disabled -Dx11=disabled -Dopengl=disabled \
                  -Dvideo-wallpaper=disabled
```

## Debug builds

```sh
meson setup build --buildtype=debug
# OR for max debug + asan
meson setup build --buildtype=debug -Db_sanitize=address
ninja -C build
```

Run a specific tool with verbose logging:

```sh
VANTAGE_LOG_LEVEL=trace ./build/src/tools/vantage-diagnostics
```
