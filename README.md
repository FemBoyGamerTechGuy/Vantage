# Vantage

**Vantage is a raw-C Linux desktop environment** — independent of XFCE4
source code, but architecturally informed by it. Lightweight by design.
New features, minimal overhead.

> Vantage is a raw-C desktop environment extending the ideas of XFCE with
> live video wallpapers, cross-toolkit Qt6/GTK theming, and native
> Wayland + Xorg + XLibre support. Zero Red Hat dependencies. D-Bus
> optional. Lightweight by design — new features, minimal overhead.

## Features

- **Raw C core.** Modern C11/C17, modular, dependency-light. No GLib, no
  systemd, no D-Bus, no Red Hat infrastructure anywhere in the core.
- **Two native backends, three servers**: a native Wayland compositor
  (libwayland-server + xdg-shell), and a shared X11 path that talks to
  both Xorg and XLibre (runtime-detected).
- **Hardware-accelerated rendering** on NVIDIA (proprietary + NVK),
  AMD (Mesa/radeonsi/radv), and Intel (Mesa/iris/anv). Software
  fallback always available.
- **A real EWMH/ICCCM window manager**: focus (click + sloppy),
  workspaces, maximize/fullscreen/minimize, edge-snap tiling, alt-drag
  move/resize, XGrabKey hotkeys, workarea with panel struts.
- **A damage-tracked compositor** (XComposite + XDamage + XRender):
  per-window opacity, optional shadows, fullscreen fast-path, frame
  pacing. Everything expensive is individually switchable and OFF by
  default — idle CPU is zero.
- **Live video wallpapers** via ffmpeg — event-driven, pauses when a
  fullscreen window covers the desktop, plus PNG/JPEG still wallpapers
  loaded directly through libpng/libjpeg (no gdk-pixbuf).
- **A real panel**: dock window with struts, XRender/Xft drawing, live
  tasklist, workspace switcher, XDG application launcher menu, XEmbed
  system tray, ALSA volume, network and battery applets.
- **Cross-toolkit theming** — one JSON theme file drives GTK3 CSS,
  GTK4 CSS, `settings.ini`, Qt6 QSS, a qt6ct conf and a KColorScheme
  file.
- **Modular settings** (appearance, displays, keyboard, windows,
  wallpaper, compositor, power, startup) with live apply.
- **Supervised session**: restart backoff for components, XDG autostart
  (OnlyShowIn/NotShowIn/TryExec-aware), clean process-group shutdown.
- **Native IPC** over Unix sockets (multi-client, event broadcast) —
  `vantage-remote` is the CLI front-end. D-Bus stays optional.
- **Optional integrations**: audio (PulseAudio > ALSA), network
  (sysfs), power (direct ACPI sysfs). All gracefully degrade.
- **Works without systemd, without D-Bus, without NetworkManager.**

## Quick start

```sh
# Build (meson + ninja)
meson setup build
ninja -C build

# Test (unit tests + Xvfb/Wayland integration harnesses)
meson test -C build

# Install
sudo ninja -C build install

# Initialize user config
vantage-config init

# Run (from a display manager, or startx / weston-launch style)
vantage-session
```

Manual integration tests (they boot real servers headlessly):

```sh
scripts/xvfb-smoke.sh      # WM + compositor + client + screenshot + pixels
scripts/session-test.sh    # full session (WM+panel+desktop) end-to-end
scripts/wayland-test.sh    # native Wayland compositor + client + pixels
```

## Documentation

See [`docs/`](docs/) for:

- [Architecture](docs/architecture.md)
- [Build instructions](docs/build.md)
- [Dependencies](docs/dependencies.md)
- [Wayland backend](docs/wayland-backend.md)
- [Xorg backend](docs/xorg-backend.md)
- [XLibre backend](docs/xlibre-backend.md)
- [GPU acceleration](docs/gpu.md)
- [NVIDIA setup](docs/nvidia.md)
- [AMD setup](docs/amd.md)
- [Intel setup](docs/intel.md)
- [Theme engine](docs/theme-engine.md)
- [Qt6 integration](docs/qt6-integration.md)
- [GTK integration](docs/gtk-integration.md)
- [Wallpaper engine](docs/wallpaper-engine.md)
- [Configuration](docs/configuration.md)
- [Plugin API](docs/plugin-api.md)
- [Troubleshooting](docs/troubleshooting.md)

## Quick diagnostics

```sh
vantage-diagnostics
```

## Components

| Binary                | Purpose                                        |
|-----------------------|------------------------------------------------|
| `vantage-session`     | Session entry point (supervised components)   |
| `vantage-wm`          | Window manager + compositor + WM IPC server   |
| `vantage-panel`       | Panel with tasklist / launcher / tray / clock  |
| `vantage-desktop`     | Desktop surface: wallpaper, icons, menus      |
| `vantage-settings`    | Settings CLI (modular, live apply)            |
| `vantage-config`      | Config CLI                                     |
| `vantage-remote`      | Control a running session from scripts        |
| `vantage-renderer`    | Renderer probe / debug                        |
| `vantage-theme`       | Theme CLI (list / apply / generate)           |
| `vantage-diagnostics` | Diagnostic dump                                |

## IPC

`vantage-wm` listens on `$XDG_RUNTIME_DIR/vantage.sock` and
`vantage-session` on `$XDG_RUNTIME_DIR/vantage-session.sock`.
Framed binary messages with text payloads (`key=value` lines) —
debuggable with any tool that can write to a socket. Examples:

```sh
vantage-remote list           # all windows (id/title/ws/flags/class)
vantage-remote focus 0x400001
vantage-remote close 0x400001
vantage-remote ws 2           # switch workspace
vantage-remote launch "xterm" # spawn an app
vantage-remote watch          # stream window events
```

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

## Status

The full stack builds and runs: session supervision, the X11
EWMH window manager, the damage-tracked compositor, panel, desktop
(wallpaper + icons + menus), theme generation, settings, integrations,
and the native Wayland compositor. Integration harnesses verify real
pixel output under Xvfb and Wayland. Real-hardware GPU acceleration
(NVIDIA/AMD/Intel) engages through the EGL/GL paths when a DRM device
is present — see the build summary for what is linked on your system.

## Why another DE?

Because lightweight desktop environments with modern features (Wayland,
live wallpapers, cross-toolkit theming) shouldn't require systemd,
D-Bus, NetworkManager, GTK, or Qt. Vantage is built so that:

1. **You** can choose exactly which optional integrations you want.
2. The core stays small and readable (a single C codebase you can
   actually read in a day).
3. **No vendor lock-in** — works on Arch, Debian, Ubuntu, Fedora,
   openSUSE, Alpine, Void, Gentoo, and any Linux that follows the
   base specs.
4. **No XFCE4 source code** — written from scratch.

## Contributing

PRs welcome. Please run `meson test` and the three integration
harnesses under `scripts/` before submitting.
