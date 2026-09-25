# Vantage X11 Backend

Vantage has **one X11 backend**. It is a client of the X server named
by `$DISPLAY` and uses libX11 (with libxcb-based helpers where
available). It is the production-ready display backend today.

## One backend, any X server

Xorg and XLibre both provide an X11 server interface, so Vantage does
not distinguish between them at the API or configuration level:

```text
Vantage X11 backend
        |
        v
      X11 API
        |
   +----+----+
   |         |
 Xorg      XLibre      (also Xvfb, Xvnc, …)
```

`vantage-session --x11` means *connect to the existing X server*. The
server may be Xorg or XLibre (or anything else that speaks X11);
Vantage never launches or embeds an X server itself and needs no
privileges. The running implementation is queried once, via the
server vendor string, purely so diagnostics can report:

```text
Display backend: X11
X server:       Xorg (informational)
```

There is no `--xorg`, `--xlibre`, or `--x11l` mode — those would be
duplicated backends for the same protocol.

## Usage

```sh
vantage-session --x11          # session on the running $DISPLAY server
vantage-wm --x11               # window manager only
vantage-diagnostics --x11      # probe the X11 path and report
```

If `$DISPLAY` is unset (or the server cannot be reached), the session
fails fast with a clear error and a nonzero exit status — it never
falls back to starting an X server.

## Configuration

```ini
[desktop]
backend=x11      ; or: wayland, auto (default)
```

Legacy spellings `backend=xorg` and `backend=xlibre` are accepted as
aliases of `x11`. Command-line flags and `$VANTAGE_BACKEND` override
the configuration file.

## Dependencies

* libX11 (mandatory for the backend)
* libxcb, libxcb-randr (multi-monitor)
* libxcb-icccm / libxcb-ewmh (window manager hints)
* libxkbcommon-x11 (keyboard layout)
* XRender, XComposite, XDamage, XFixes (compositor)

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
* ICCCM `WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`
* ICCCM `WM_NORMAL_HINTS` (with `PWinGravity` for placement hints)

## Multi-monitor (RandR)

Vantage reads RandR outputs and applies the layout defined in the
user's config:

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
redirects toplevel windows and uses the damage extension to track
repaint regions, compositing through XRender (see
`src/compositor/vt-compositor-x11.c`).
