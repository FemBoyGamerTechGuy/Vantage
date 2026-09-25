# Vantage Architecture

Vantage is a lightweight raw-C Linux desktop environment inspired by XFCE but
implemented from scratch — no XFCE4 source is included or required.

## Component layout

```
┌─────────────────────────────────────────────────────────────────────┐
│                         vantage-session                              │
│                  (lifecycle, autostart, power hooks)                 │
│                                                                       │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │
│  │ vantage-wm  │  │vantage-panel│  │vantage-desk │  │ vantage-conf│ │
│  │   (wm)      │  │  (applets)  │  │ (wallpaper) │  │  (CLI tool) │ │
│  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘ │
│           │              │                │                │         │
│           ▼              ▼                ▼                ▼         │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │              vantage-compositor (damage + frame)             │    │
│  └─────────────────────────────────────────────────────────────┘    │
│           │                                                          │
│           ▼                                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │           vantage-renderer (GL/EGL + software)                │    │
│  └─────────────────────────────────────────────────────────────┘    │
│           │                                                          │
│           ▼                                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │             vantage-backend (Wayland / X11)                 │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                       │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐    │
│  │ vantage-gpu│  │vantage-ipc │  │vantage-conf│  │vantage-them│    │
│  │(NVIDIA/AMD/│  │(UDS RPC)   │  │(INI+watches│  │(JSON palette│    │
│  │ Intel)     │  │            │  │ inotify)   │  │  + GTK/Qt6) │    │
│  └────────────┘  └────────────┘  └────────────┘  └────────────┘    │
│                                                                       │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Optional integrations (audio / network / power / dbus)        │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Design principles

1. **C-first**: All core code is C11/C17. C++ is forbidden for the core.
2. **No mandatory D-Bus, systemd, or Red Hat infrastructure.** Every
   integration is optional and runtime-detected.
3. **Lightweight by design.** No polling loops when event-driven APIs
   exist. No background threads when not needed. No timer ticks.
4. **Hardware acceleration preferred, software fallback always
   available.**
5. **Cross-toolkit theming** via a single JSON theme file → GTK CSS +
   Qt6 QSS generated at theme-apply time.

## Subsystem dependencies

| Subsystem            | Hard deps           | Optional deps                |
|---------------------|---------------------|------------------------------|
| libvantage-core     | libc, libm, pthread |                              |
| libvantage-config   | core                |                              |
| libvantage-ipc      | core                |                              |
| libvantage-renderer | core                | EGL, GLESv2/GL, Vulkan       |
| libvantage-gpu      | core                | libdrm, libudev              |
| libvantage-backends | core, gpu           | wayland-client, xcb, randr   |
| libvantage-compositor| core, renderer, backends |                    |
| libvantage-wm       | core, backends      |                              |
| libvantage-input    | core                | xkbcommon                    |
| libvantage-session  | core, config        |                              |
| libvantage-theme    | core, renderer      | json-c                       |
| libvantage-wallpaper| core, renderer      | ffmpeg, gdk-pixbuf           |
| libvantage-panel    | core, renderer, integrations |                   |
| libvantage-desktop  | core, renderer      |                              |
| libvantage-settings | core, config, theme |                              |
| libvantage-integrations | core           | dbus-1, pipewire, pulse, alsa, libnm, upower, systemd, elogind |
