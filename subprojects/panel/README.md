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

A panel **must** dock. When neither is possible — a Wayland compositor
without `zwlr_layer_shell_v1`, or a binary built without gtk4-layer-shell
(the dependency is required by default at configure time since 0.3.2;
X11-only builds opt out with `-Dlayer-shell=disabled`) — the panel
**refuses to run**: it prints one actionable line (naming the missing
library or protocol and the fix) and exits non-zero, the same contract
as waybar. Before 0.3.2 it silently fell back to a plain floating
window, which broke every panel feature far downstream of the cause.

## Visual identity

The panel carries its OWN look (an application-priority CSS provider):
the dark bar, the light text and — since 0.3.1 — the ENTIRE Programs
menu (sheet, text, search entry, row geometry) are pinned in CSS. The
user's GTK theme can still restyle every other app, but the desktop
shell always looks like itself — the menu's row hit boxes are
theme-independent too, which is what makes launcher automation (the
integration harnesses) deterministic on every machine. Popovers open
with no fade-in: a desktop shell wants instant menus, and
frame-callback-driven animations are not reliable timing on software
renderers.

## Building

Standalone:

    meson setup build
    ninja -C build

gtk4-layer-shell is required by default (`-Dlayer-shell=disabled`
opts out for X11-only use). As part of the Vantage desktop
environment it is pulled in automatically by the parent build as
`subprojects/panel` (`-Dpanel:layer-shell=disabled` reaches the same
option from the parent configure).
