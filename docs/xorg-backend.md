# Vantage Xorg Backend

The X11 backend uses libxcb (preferred over libX11 for performance and
thread-safety). It is the production-ready display backend today.

## Dependencies

* libxcb (mandatory)
* libxcb-randr (optional, for multi-monitor)
* libxcb-icccm (optional, for window manager)
* libxcb-ewmh (optional, for EWMH hints)
* libxkbcommon-x11 (optional, for keyboard layout)

## Configuration

```ini
[desktop]
backend=xorg
```

The backend auto-detects the X server (Xorg or XLibre) at runtime.

## EWMH / ICCCM support

Vantage implements the subset of EWMH/ICCCM required for:

* Window placement + initial geometry
* `_NET_WM_STATE_FULLSCREEN`, `_NET_WM_STATE_MAXIMIZED_*`,
  `_NET_WM_STATE_ABOVE`/`_BELOW`
* `_NET_WM_WINDOW_TYPE_*` (desktop, dock, toolbar, splash, etc.)
* `_NET_WORKAREA` (workspace extents minus panel area)
* `_NET_CLIENT_LIST` (for tasklist)
* `_NET_ACTIVE_WINDOW`
* `_NET_CURRENT_DESKTOP` / `_NET_NUMBER_OF_DESKTOPS`
* ICCCM `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`, `WM_PING`
* ICCCM `WM_NORMAL_HINTS` (with `PWinGravity` for placement hints)

## Multi-monitor (RandR)

Vantage reads RandR outputs via `xcb_randr_get_screen_resources_current`
and the per-output `xcb_randr_get_output_info` calls. It applies the
layout defined in the user's config:

```ini
[displays]
[output:HDMI-1]
enabled=true
x=0
y=0
scale=1.0

[output:DP-1]
enabled=true
x=1920
y=0
scale=2.0
```

## Compositing

Vantage's compositor talks to the X server's Composite extension. It
creates a `XCompositeRedirectWindow` per toplevel surface and uses the
damage extension to track repaint regions.
