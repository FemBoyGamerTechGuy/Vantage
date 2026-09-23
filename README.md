# Vantage

**Vantage is a new, raw-C Linux desktop environment** — independent of
XFCE4 source code, but architecturally informed by it. Lightweight by
design. New features, minimal overhead.

> Vantage is a raw-C desktop environment extending the ideas of XFCE with
> live video wallpapers, cross-toolkit Qt6/GTK theming, and native
> Wayland + Xorg + XLibre support. Zero Red Hat dependencies. D-Bus
> optional. Lightweight by design — new features, minimal overhead.

## Features

- **Raw C core.** Modern C11/C17, modular, dependency-light.
- **Three native backends**: Wayland, Xorg, XLibre (all first-class).
- **Hardware-accelerated rendering** on NVIDIA (proprietary + NVK),
  AMD (Mesa/radeonsi/radv), and Intel (Mesa/iris/anv). Software fallback
  always available.
- **Live video wallpapers** via ffmpeg — event-driven, pauses when not
  visible, audio routed through the chosen audio backend.
- **Cross-toolkit theming** — one JSON theme file drives both GTK3/4
  CSS and Qt6 QSS.
- **Modular settings** (appearance, displays, keyboard, windows,
  wallpaper, compositor, power, startup) with live reload.
- **Optional integrations**: audio (PipeWire > PulseAudio > ALSA),
  network (NetworkManager > ConnMan > `/proc`), power (logind > UPower
  > direct `/sys/power/state`), D-Bus. All gracefully degrade.
- **Works without systemd, without D-Bus, without NetworkManager.**

## Quick start

```sh
# Build
meson setup build
ninja -C build

# Test
meson test -C build

# Install
sudo ninja -C build install

# Initialize user config
vantage-config init

# Run
vantage-session
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

| Binary                | Purpose                          |
|-----------------------|----------------------------------|
| `vantage-session`     | Session entry point              |
| `vantage-wm`          | Standalone window manager        |
| `vantage-panel`       | Standalone panel                  |
| `vantage-desktop`     | Standalone desktop surface       |
| `vantage-settings`    | Settings CLI                      |
| `vantage-config`      | Config CLI                        |
| `vantage-renderer`     | Renderer probe / debug           |
| `vantage-theme`       | Theme CLI                         |
| `vantage-diagnostics` | Diagnostic dump                  |

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

## Status

This is an early release. The architecture, build system, core libraries,
backends, compositor, WM, session, panel, desktop, wallpaper engine,
theme engine, settings, integrations, and diagnostics are all implemented
and the project builds and runs. Real-world X11 + GPU testing requires
the corresponding dev packages and a real display — see the build summary
for what's currently linked.

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

PRs welcome. Please run `meson test` before submitting.
