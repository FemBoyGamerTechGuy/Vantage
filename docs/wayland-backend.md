# Vantage Wayland Backend

Vantage's Wayland backend is a **native Wayland compositor** built
directly on `libwayland-server` — Vantage *is* the display server. It
acquires the seat, the VT and DRM master itself, mode-sets real outputs
via KMS and composites client surfaces (wl_shm) into the scanout
buffers with async page flips.

## How a real session starts (the 15-stage pipeline)

Every stage prints a `[wayland]` marker *before and after* it runs, so a
real TTY run pinpoints exactly where setup stops. On success the log
looks like:

```
[wayland] session: starting — XDG_RUNTIME_DIR preparation
[wayland] session: ok — XDG_RUNTIME_DIR=/run/user/1000
[wayland] seat: starting — libseat (logind → elogind-compatible → seatd) or direct VT
[wayland] seat: ok — libseat, seat 'seat0', VT 2
[wayland] vt: starting — VT acquisition/activation
[wayland] vt: ok — VT 2 already active (granted with the session)
[wayland] drm: starting — /dev/dri card discovery
[wayland] drm: ok — /dev/dri/card0 opened through the seat
[wayland] drm-master: ok — DRM master acquired on the card
[wayland] gbm: ok — GBM device on the card
[wayland] egl: ok — EGL display + GLES context initialized
[wayland] renderer: ok — AMD Radeon ... (EGL 1.5, GLES2)
[wayland] outputs: ok — 1 output(s): HDMI-A-1 1920x1080@60
[wayland] crtc: starting — CRTC mode set
[wayland] crtc: ok — HDMI-A-1: mode 1920x1080@60, scanout live
[wayland] scanout: ok — gbm ping-pong buffers, async page flips, hardware cursor
[wayland] input: starting — libinput (udev) + xkbcommon
[wayland] input: ok — 4 device(s), xkb keymap ready
[wayland] socket: starting — wl_display socket
[wayland] socket: ok — WAYLAND_DISPLAY=wayland-0 (/run/user/1000/wayland-0)
[wayland] compositor: READY
[wayland] desktop: starting — first frame
[wayland] desktop: ok — desktop painted 1920x1080 (HDMI-A-1), cursor on the hardware plane
[wayland] desktop: ready
```

A stage that cannot run is marked `skipped` with the reason (e.g.
`gbm: skipped — gbm unusable — dumb scanout buffers`); a stage that
fails is marked `FAILED` with the error. **The last `ok`/`skipped`
marker before the failure is the answer to "where did it stop".**

### Blocking vs. failing — how to read a stuck startup

If the compositor hangs instead of exiting, the markers tell you which
case you are in:

| Observation                                              | Meaning                     |
|----------------------------------------------------------|-----------------------------|
| Log stops at `seat` / `vt` with no further output        | waiting for session/VT access (logind not answering, wrong VT) |
| Log stops at `drm` / `drm-master`                        | waiting on DRM access (master held by another server — check for a running Xorg on another VT) |
| `compositor: READY` printed but screen stays on the TTY   | event loop runs, but no scanout happened — the `crtc`/`scanout` markers above will say why |
| Log stops before `socket`                                 | startup incomplete — no client can connect yet |
| Never returns to the shell after Ctrl+C                   | a bug — the exit path logs `[wayland] compositor: exited cleanly`; file that with the full log |

Ctrl+C (SIGINT) and SIGTERM both take the *clean unwind* path:
CRTC restored to the pre-Vantage mode, VT switched back to text, seat
released, exit status **0**. `vantage-remote logout` triggers the same
unwind via IPC (no SIGKILL anywhere in the normal path).

## Supported launch methods

### 1. From a TTY (no session manager needed)

Log into any virtual console (Ctrl+Alt+F2..F6) and run:

```sh
vantage-session --wayland
```

With logind/elogind active, the PAM session already granted the seat —
libseat picks it up automatically. **Without** any session manager
(plain TTY login as a user who owns the VT), the backend falls back to
direct VT ioctls: it opens `/dev/ttyN` (from `$VANTAGE_VT` /
`$XDG_VTNR` / the active VT), takes `VT_SETMODE(VT_PROCESS)` and calls
`drmSetMaster()` itself. The user needs permission to open the DRM
card (video group) in that mode.

### 2. From a display manager

Add vantage-session as a session entry (the packaged
`vantage-session.desktop` is installed for this). The display manager
runs it on its own VT with logind already holding the session —
identical code path to method 1.

### 3. Running the compositor directly

```sh
vantage-wm --wayland
```

Runs the compositor + WM without the session manager (no panel /
desktop / autostart — those are X11 clients today). Ctrl+C exits
cleanly; `vantage-remote logout` also works against the WM socket.

**Never** launch the Wayland session from a terminal inside a running
graphical session: `vantage-session --wayland` detects a live X server
in `$DISPLAY` and refuses (starting a second DRM master would break
the running one), and a live `WAYLAND_DISPLAY` makes the compositor
refuse to nest.

## Seat / VT / DRM access

| Ingredient | Preferred source | Fallback |
|------------|------------------|----------|
| seat + session | libseat → logind/elogind | libseat → seatd daemon |
| VT | granted with the session | direct `VT_SETMODE(VT_PROCESS)` on the login VT |
| DRM master | granted by logind with the active session | `drmSetMaster()` on the directly-opened card |
| input devices | opened through the seat (`libinput` udev context) | direct open (no seat) |

`$VT_SEAT_BACKEND` forces the libseat backend: `logind`|`elogind`|
`seatd`|`builtin`, or `direct` to skip libseat entirely. Nothing here
requires systemd — elogind speaks the same protocol, and the direct
path needs no session manager at all.

Readiness can be checked before launching:

```sh
vantage-diagnostics --wayland-session
```

## Configuration

```ini
[desktop]
backend=wayland

[wayland]
scanout=auto        ; auto | gbm | dumb
```

| Knob                       | Meaning                                        |
|----------------------------|------------------------------------------------|
| `VANTAGE_WAYLAND_SCANOUT`  | `auto` (GBM, dumb fallback), `gbm`, `dumb`    |
| `VANTAGE_WAYLAND_REQUIRE_KMS` | `1` = fail hard instead of the headless fallback |
| `VANTAGE_WAYLAND_FORCE_HEADLESS` | `1` = skip seat/vt/drm outright — deterministic tests/CI that never touch the host GPU/VT (what `meson test` uses) |
| `VT_SEAT_BACKEND`          | force the seat backend (see table above)       |
| `XCURSOR_THEME` / `XCURSOR_SIZE` | cursor theme for the compositor's cursor |
| `XKB_DEFAULT_*`            | keyboard layout (libxkbcommon defaults)        |

### `meson test` runs forced-headless (by design)

The integration harness exports `VANTAGE_WAYLAND_FORCE_HEADLESS=1`, so
the 15-stage trace is identical on a CI container, a desktop shell and
a machine with a real GPU: `seat`, `vt` and `drm` report
`skipped — forced headless` and `/dev/dri` is never opened — a test
must not grab the developer's VT or poke the card the running desktop
owns. Green tests therefore prove the headless contract (socket,
xdg-shell clients, compositing, IPC, logout) and say **nothing** about
real scanout. Without the knob, a launch from a desktop terminal
honestly reports `drm: FAILED — DRM master refused` (the running X
server owns the card); the real KMS path is exercised only by a TTY
launch (method 1 above).

## Cursor

The compositor cursor uses the hardware cursor plane (64x64 ARGB) when
the driver offers it; otherwise a software sprite is alpha-blended at
the pointer position. The image comes from Xcursor
(`$XCURSOR_THEME`, `left_ptr`) with a built-in arrow fallback, so the
pointer is always visible. Clients can replace it per-surface via
`wl_pointer.set_cursor` (rendered as the software sprite).

## Honest limitations

* **Headless fallback**: when no KMS output can be acquired (no
  `/dev/dri`, container, no DRM master), the compositor says so loudly
  (`[wayland] NOTICE: HEADLESS mode …`) and renders into an in-memory
  framebuffer for tests/CI. It never *claims* a real display it does
  not own; `VANTAGE_WAYLAND_REQUIRE_KMS=1` turns the fallback into an
  error.
* The panel and desktop components are X11 clients today; the Wayland
  session runs the compositor + WM (windows are composited and
  manageable through `vantage-remote`).
* Composition is CPU-side into the mapped scanout buffers — the same
  code path the headless tests exercise. The EGL/GLES context exists
  for honest renderer reporting and future GPU compositing.
* Multi-monitor: every connected connector is driven (cloned content);
  per-monitor layouts are not yet configurable.
* wl_subcompositor, pointer gestures and IME are not implemented; the
  core desktop protocols (compositor/shm/seat/output/xdg_shell) are.
