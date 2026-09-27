# vantage-panel

The Vantage desktop panel — one binary, both compositors.

Built on **GTK4** (like the XFCE panel is built on GTK, but a
generation newer), it is an *independent subproject*: it shares no code
with the Vantage window manager or compositor, has its own build and
license, and talks to them exclusively through the documented Vantage
WM IPC protocol (a small Unix-socket wire format) — the same way the
XFCE panel talks to xfwm4 through EWMH/X11 instead of linking it.

## Applets

| Applet        | What it does                                                |
|---------------|-------------------------------------------------------------|
| Programs      | searchable, categorized menu over the GDesktopAppInfo DB;  |
|               | icons from the active icon theme at full fidelity; the      |
|               | fallback start icon is a transparent 9-dot grid (no plate)  |
| Pager         | live workspace MINIATURES — each desktop shows its windows  |
|               | at their true relative position/size (no numbers)          |
| Tasklist      | window buttons with themed icons (app_id / WM_CLASS),      |
|               | click to focus/restore, middle-click to close               |
| Network       | link/wifi quality from sysfs + /proc/net/wireless           |
| Volume        | real ALSA mixer (Master/PCM/Front), wheel + slider + mute   |
| Clock         | local time + calendar popover                               |
| Session       | logout / reboot / shutdown through the session manager      |

## Docking

* **Wayland**: `zwlr_layer_shell_v1` (top layer, top edge, exclusive
  zone, on-demand keyboard) via gtk4-layer-shell.
* **X11**: `_NET_WM_WINDOW_TYPE_DOCK` + `_NET_WM_STATE_ABOVE` +
  `_NET_WM_STRUT_PARTIAL` — plain EWMH.

## Building

Standalone:

    meson setup build
    ninja -C build

As part of the Vantage desktop environment: it is pulled in
automatically by the parent build as `subprojects/panel`.
