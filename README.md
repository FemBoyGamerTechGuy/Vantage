# Vantage

**Vantage is a raw-C Linux desktop environment** — independent of XFCE4
source code, but architecturally informed by it. Lightweight by design.
New features, minimal overhead.

> Vantage is a raw-C desktop environment extending the ideas of XFCE with
> live video wallpapers, cross-toolkit Qt6/GTK theming, and native
> Wayland + X11 support (Xorg, XLibre, or any conforming X server).
> Zero Red Hat dependencies. D-Bus optional. Lightweight by design —
> new features, minimal overhead.

## Features

- **Raw C core.** Modern C11/C17, modular, dependency-light. No GLib, no
  systemd, no D-Bus, no Red Hat infrastructure anywhere in the core.
- **Two display backends**: a native Wayland compositor
  (libwayland-server + xdg-shell) that acquires the seat, VT and DRM
  master itself (libseat → logind/elogind/seatd, with a direct-VT
  fallback when no session manager runs) and mode-sets real outputs via
  KMS with async page flips, libinput + xkbcommon input and a hardware
  cursor plane; and an X11 backend that connects to the running X
  server — Xorg, XLibre, or any conforming implementation (identified
  at runtime for diagnostics only).
- **Hardware-accelerated rendering** on NVIDIA (proprietary + NVK),
  AMD (Mesa/radeonsi/radv), and Intel (Mesa/iris/anv). Software
  fallback always available.
- **A real EWMH/ICCCM window manager**: click-to-focus by default
  (hover never steals the keyboard; `[wm] focus=sloppy` opts into
  focus-follows-mouse), workspaces, maximize/fullscreen/minimize,
  edge-snap tiling, alt-drag move/resize, XGrabKey hotkeys, workarea
  with panel struts, and **server-side decorations** — every normal
  window gets a title bar with close/maximize/minimize buttons, accent
  borders and edge resize grips (Kitty and Mirage get real title
  bars).
- **A damage-tracked compositor** (XComposite + XDamage + XRender):
  per-window opacity, optional shadows, fullscreen fast-path, frame
  pacing. Everything expensive is individually switchable and OFF by
  default — idle CPU is zero.
- **Live video wallpapers** via ffmpeg — event-driven, pauses when a
  fullscreen window covers the desktop, plus PNG/JPEG still wallpapers
  loaded directly through libpng/libjpeg (no gdk-pixbuf).
- **A real panel**: dock window with struts, XRender/Xft drawing, live
  tasklist with per-window `_NET_WM_ICON` icons (area-averaged at 20px),
  a **workspace PAGER** (every cell is a miniature of that desktop with
  its windows at their true position and size — click to switch, wheel
  to cycle), a **Programs menu** built from the shared `.desktop`
  database (search bar, scrolling, **themed start-button icon and 24px
  menu icons** — SVG themes rasterize at the exact display size,
  locale-aware names — Russian and other non-English entries render
  correctly via per-codepoint font fallback), calendar popup, ALSA
  volume slider (real mixer values), a functional network indicator,
  and a username/session menu (Lock, Suspend, Switch User, Log Out,
  Reboot, Shutdown, Exit) — all on real system mechanisms
  (logind/elogind, ACPI).
- **A compositor-drawn panel on native Wayland** (no XWayland, no
  placeholder blocks): Programs button with the SAME shared application
  database and menu features (search, scrolling, themed start icon,
  24px theme icons, categories), a real taskbar (app icons via `app_id`,
  adaptive widths, focused/minimized states, click-to-focus,
  click-again-to-minimize), the workspace PAGER with live miniatures,
  network + volume (ALSA) controls, clock + calendar, and the full
  session menu — rendered with FreeType text directly into the scanout
  framebuffer. The desktop background is the SAME `[wallpaper]` config
  the X11 desktop renders (gradient/color/image, cover-scaled).
- **Real Wayland clients work — and stay alive**: xdg-shell toplevels
  AND popups (real positioner placement, grabs, popup_done dismissal),
  xdg-decoration (CSD apps are never double-decorated; server-mode
  windows get a compositor titlebar), wl_subcompositor subsurfaces and
  an in-session clipboard (wl_data_device_manager). Client buffers are
  copied at commit and released immediately (double-buffered apps never
  stall); GTK3/GTK4 applications run against the compositor in CI-grade
  headless tests. Applications launched from the menu inherit the
  compositor's `WAYLAND_DISPLAY` and actually appear; launched children
  are reaped, never zombified.
- **CSD apps never double-decorated on X11 either**: `_MOTIF_WM_HINTS`
  is honored (GTK/Chromium/Firefox headerbar windows stay undecorated,
  re-evaluated live when an app toggles its own decorations).
- **Daily-driver window management**: Alt+Tab cycling, Super+drag
  move/resize (Wayland — works even on undecorated windows), minimize
  from the SSD titlebar or the taskbar, workspaces that follow focus,
  maximized windows respect the panel workarea.
- **Logout that always returns you to the TTY**: the panel routes
  session actions through the session manager (SIGTERM + grace, no
  SIGKILL), and the supervisor never restarts a compositor that exited
  cleanly — a Wayland logout ends the session instead of re-taking
  the screen.
- **Recovery without reboots**: on Wayland the compositor implements
  Ctrl+Alt+F1..F12 VT switching itself (evdev owns the keyboard, so
  the kernel combos never fire) plus Ctrl+Alt+Delete clean logout;
  SIGTERM/SIGINT always restore the CRTC, return the VT to text mode
  and exit 0.
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
./build build              # configure + compile (rootless)
./build test               # unit tests + Xvfb/Wayland/CLI integration harnesses
./vantage-session --x11    # run straight from the tree on the running X server
./vantage-session --wayland# or start the native Wayland compositor session

./build install            # install to ~/.local (still rootless)
vantage-session --x11      # same binary, same behavior, installed paths

./build packages arch      # produce real *.pkg.tar.zst artifacts in dist/
```

Backend selection on `vantage-session` (and `vantage-wm`):

```text
--wayland    native Vantage Wayland compositor session
--x11        X11 backend: connect to the X server named by $DISPLAY
(no flag)    automatic: X11 when $DISPLAY is set, else Wayland
```

The X11 backend works identically on Xorg and XLibre — both speak X11,
so Vantage has a single X11 code path and reports the server
implementation informationally (`vantage-diagnostics` prints
`X server: Xorg` / `X server: XLibre`).

Integration harnesses (they boot real sessions headlessly):

```sh
tests/harness-xvfb.sh     # full session on Xvfb: WM+panel+desktop+EWMH+pixels
tests/harness-wayland.sh  # native Wayland compositor + xdg-shell client
tests/harness-cli.sh      # CLI surface + dev-tree + installed execution
```

## Documentation

See [`docs/`](docs/) for:

- [Architecture](docs/architecture.md)
- [Build instructions](docs/build.md)
- [Dependencies](docs/dependencies.md)
- [Wayland backend](docs/wayland-backend.md)
- [X11 backend](docs/x11-backend.md)
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
vantage-remote maximize 0x400001   # toggle; unmaximize to force off
vantage-remote minimize 0x400001   # restore brings it back
vantage-remote ws 2           # switch workspace
vantage-remote ws-move 0x400001 2 # move window to workspace 2
vantage-remote launch "xterm" # spawn an app
vantage-remote status         # session version/stage/children
vantage-remote watch          # stream window events
```

## License

Vantage is proprietary, source-available software, copyright
FemBoyGamerTechGuy. See [LICENSE](LICENSE) for the full terms (restricted
copying, redistribution, modification and publication; no sublicensing;
trademark protection).

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

PRs welcome. Please run `./build test` (the unit tests plus the three
integration harnesses) before submitting.
