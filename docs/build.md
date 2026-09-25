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

## The `./build` entry point

The recommended workflow is Vantage's own build command, which wraps
meson/ninja, keeps the development tree directly runnable, and drives
packaging:

```sh
./build                  # help
./build build            # configure + compile (rootless)
./build test             # full test suite (unit + integration)
./build install          # install to ~/.local (rootless)
./build install --prefix /usr           # or any prefix
./build packages arch    # build + test + stage + real .pkg.tar.zst
./build clean            # remove build outputs
./build distclean        # also remove dist/ artifacts
```

Everything except a system-wide install runs **without root**.

## Run straight from the source tree

After `./build build`, the binaries are directly runnable — no
installation needed. Repo-root symlinks point at the build tree:

```sh
./vantage-session --wayland          # native Wayland compositor session
./vantage-session --x11              # X11 backend on the running $DISPLAY
./vantage-wm --x11                   # window manager only
./vantage-diagnostics --x11          # system report
./vantage-remote list                # talk to a running session
```

Binaries locate their resources (themes, default config, autostart)
relative to their own real location first, then via XDG paths, then the
compiled install prefix — see `src/core/vt-paths.c`. No absolute
build-machine paths are baked in, and running from the tree behaves the
same as running installed, apart from where resources are read from.

## Plain meson (advanced)

```sh
git clone https://github.com/FemBoyGamerTechGuy/Vantage
cd Vantage
meson setup builddir
ninja -C builddir
meson test -C builddir
ninja -C builddir install
```

This installs (for prefix `/usr/local`):

* binaries in `/usr/local/bin/`
* session desktop files in `/usr/local/share/xsessions/` and
  `/usr/local/share/wayland-sessions/`
* themes in `/usr/local/share/vantage/themes/`
* default config in `/usr/local/etc/vantage/`
* headers in `/usr/local/include/vantage-0.1/`
* test clients in `/usr/local/lib/vantage/tests/`

## Packaging

Vantage ships its own packagers — `makepkg` is not the interface:

```sh
./build packages arch
```

configures a release build, runs the test suite, stages the install
and writes real, installable artifacts into `dist/`:

```text
dist/arch/
├── vantage-<version>-<x86_64|aarch64>.pkg.tar.zst
└── vantage-devel-<version>-<x86_64|aarch64>.pkg.tar.zst
```

Dependencies in the generated packages are computed from the actual
link set (`ldd`) of the staged ELF files. Packagers live in
`packaging/<distro>/package.sh`; adding a distro means adding a
directory, and `./build packages <distro>` dispatches to it.

Generated artifacts are never committed to the repository (`dist/` is
git-ignored).

## Configuration

The user config file is `~/.config/vantage/vantage.conf`. To generate
a default config:

```sh
vantage-config init
```

## Build options

| Option              | Default | Description                          |
|---------------------|---------|--------------------------------------|
| x11                 | auto    | Build X11 backend (Xorg, XLibre, …)  |
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
meson setup builddir -Dwayland=disabled -Dx11=disabled -Dopengl=disabled \
                    -Dvideo-wallpaper=disabled
```

## Debug builds

```sh
./build build debug
# OR for max debug + asan
meson setup builddir --buildtype=debug -Db_sanitize=address
ninja -C builddir
```

Run a specific tool with verbose logging:

```sh
VANTAGE_LOG_LEVEL=trace ./builddir/src/tools/vantage-diagnostics
```
