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

[wm]
focus=click      ; click (default) | sloppy (follows pointer)
```

Legacy spellings `backend=xorg` and `backend=xlibre` are accepted as
aliases of `x11`. Command-line flags and `$VANTAGE_BACKEND` override
the configuration file.

## Window decorations (SSD)

Vantage draws real server-side decorations: every normal window is
reparented into a WM-owned frame with a 26px title bar and a 2px accent
border — visible on Kitty, Mirage and every other client regardless of
their toolkit:

* title (UTF-8 via Xft with per-codepoint font fallback — Cyrillic and
  CJK titles render even when the primary sans face lacks the glyphs —
  truncated with an ellipsis when long)
* close / maximize / minimize buttons (right end of the title bar)
* double-click the title bar toggles maximize
* drag the title bar to move; drag any border/corner to resize
* active window: accent border + bright title; inactive: dimmed
* fullscreen removes the chrome and restores it afterwards
* `_NET_FRAME_EXTENTS` reports the real insets so EWMH-aware toolbars
  and panels size correctly

Docks (`_NET_WM_WINDOW_TYPE_DOCK`) and desktop windows stay undecorated.

## Focus policy

The default is **click-to-focus**: hovering a window NEVER steals
keyboard focus — focus moves only on a click anywhere in the window
(client area, title bar or frame border). Users who prefer
focus-follows-mouse can set `[wm] focus=sloppy`.

## The panel

The panel is `vantage-panel` — a GTK4 client (subprojects/panel, an
independent subproject) that docks as a REAL EWMH dock:
`_NET_WM_WINDOW_TYPE_DOCK` + `_NET_WM_STATE_ABOVE` +
`_NET_WM_STRUT_PARTIAL` sized to its actual rendered height. It is the
SAME binary that docks through wlr-layer-shell on the native Wayland
compositor — one panel, both backends, driven entirely by the WM IPC
protocol (window list, geometry, workspaces, actions).

* LEFT: **Programs** button → application menu built on GLib's
  GDesktopAppInfo (locale-aware `.desktop` parsing — Russian
  `Name[ru]` lines resolve per locale, categories, `NoDisplay` /
  `NotShowIn` / `TryExec` filtering) with a **search bar** (click the
  field, type to filter; BackSpace edits, Escape clears/closes),
  themed icons via GtkIconTheme and a Quit Session entry; taskbar for
  the CURRENT workspace (titles with ellipsis, `_NET_WM_ICON` window
  icons, focused/minimized states, click focuses, click-on-focused
  minimizes)
* CENTER: workspace buttons (the active cell follows
  `_NET_CURRENT_DESKTOP` via the WM's workspace-changed broadcast)
* RIGHT: network indicator (real `/sys/class/net` +
  `/proc/net/wireless` state), volume (native ALSA, real mixer values,
  wheel + popup slider), clock (date AND time), username + session
  menu
* Window placement cascades within the workarea; when a panel docks
  (strut changes) existing windows that would end up hidden behind it
  are nudged back inside the new workarea.
* Plain click-to-focus no longer grabs Button1: the WM OBSERVES
  ButtonPress instead. A core XGrabButton+ReplayPointer pair replays
  presses through the core protocol only, which Xorg does not
  translate to XInput2 — XI2-only toolkits (GDK4 — every GTK4 app,
  the panel itself) never saw plain clicks. Observed presses deliver
  one copy to the WM (focus/raise) while the client keeps its own.
* Docks never take keyboard focus (EWMH): focusing the panel would rip
  the keyboard out of a popover the same click just opened — GTK
  popovers close on focus-out, so the start menu would flash shut.

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

## CSD windows and _MOTIF_WM_HINTS

Applications that draw their own headerbars (GTK apps with client-side
decorations, Chromium, Firefox with the system titlebar disabled) set
`_MOTIF_WM_HINTS` with `decorations=0`. Vantage honors it: those
windows are left UNDECORATED — no double titlebar stacked over the
app's own. The request is re-evaluated at runtime (PropertyNotify), so
an app toggling its CSD on/off is followed immediately. Partial
decoration requests (border-only, etc.) still get the Vantage frame.

The panel's Programs button uses the active icon theme's `start-here`
icon (XFCE-style) resolved through GtkIconTheme. The workspace
switcher is a real PAGER: every cell shows that desktop's windows at
their true relative position and size (minimized windows stay in the
taskbar only, per the user's preference), click switches desktops, the
wheel cycles them.
