# Vantage Wayland Backend

Vantage's Wayland backend uses `libwayland-client` for display connection
and `libwayland-server` for the compositor (planned — currently the
compositor runs as part of the session binary).

## Protocols used

| Protocol                        | Purpose                              |
|---------------------------------|--------------------------------------|
| `wl_compositor`                 | Surface creation                     |
| `wl_shm`                        | Software buffer sharing              |
| `wl_subcompositor`              | Subsurfaces (menus, overlays)        |
| `wl_seat`                       | Keyboard / pointer / touch           |
| `xdg_shell`                     | Toplevel + popup surfaces            |
| `xdg_output_unstable_v1`       | Per-monitor logical geometry         |
| `wp_fractional_scale_v1`       | HiDPI fractional scaling             |
| `wp_viewporter`                 | Viewport for scale/clip              |
| `zwp_pointer_gestures_v1`      | Pinch / swipe                        |
| `zwp_idle_inhibit_v1`           | Inhibit screensaver                  |
| `zwp_input_method_v2`           | IME input                            |
| `ext_idle_notification_v1`     | Idle state detection                 |

## Config

```ini
[desktop]
backend=wayland
```

## Environment

Vantage respects the standard Wayland env vars:

* `WAYLAND_DISPLAY` — socket name (set by compositor)
* `WAYLAND_SOCKET` — fd of socket (set by compositor)
* `XDG_RUNTIME_DIR` — runtime dir

## Limitations

* The current Wayland backend is a **client** implementation. The full
  compositor side (running Vantage itself as the Wayland server) is a
  follow-up work item; today Vantage runs as a Wayland client of a host
  compositor (e.g. when run under Sway or KWin). The full compositor
  will be added in a later release.
* For now, the X11 backend is the production-ready path; Wayland works
  for the panel/desktop tools as overlay windows.

## Multi-monitor

Vantage enumerates outputs via `xdg-output-unstable-v1` and applies
the user's monitor layout config. Fractional scaling is honored where
`wp_fractional_scale_v1` is supported by the host compositor.

## HiDPI

When an output reports scale > 1, Vantage:

* Rescales its own panel / desktop surfaces.
* Sets `QT_WAYLAND_DISABLE_WINDOWDECORATION=1` and tells Qt/GTK to use
  the fractional scale value via the standard env vars.
* Honors the per-output scale factor independently.
