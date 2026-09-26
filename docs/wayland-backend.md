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

### Test tiers

1. **Headless protocol** — `harness-wayland.sh` (runs in `meson test`):
   socket, xdg-shell, pixels, panel, IPC, logout against the in-memory
   framebuffer.
2. **Real DRM/VT session** — `harness-wayland-real.sh`
   (`meson test` runs it but it SKIPS unless `/dev/dri` exists and the
   run happens on a console): boots the compositor with
   `VANTAGE_WAYLAND_REQUIRE_KMS=1` — no headless fallback allowed — and
   asserts the real seat/VT/master/CRTC/scanout/input stages, xdg client
   pixels on the real output, the compositor panel, and a clean SIGINT
   unwind (CRTC restored, VT text, exit 0). On the target hardware:
   `./build test` from a TTY, or
   `VANTAGE_WAYLAND_SELFTEST=1 tests/harness-wayland-real.sh`.
3. **GPU/NVIDIA reporting** — `vantage-diagnostics --gpu`: vendor,
   renderer, hardware vs software (llvmpipe-class) rendering, never
   claiming acceleration that is not there.

## Cursor

The pointer is ALWAYS visible: the software cursor sprite is the source
of truth, blended into the scanout framebuffer on every frame, and the
64x64 ARGB hardware cursor plane is used on top when the driver actually
supports it (including NVIDIA, where legacy cursor ioctls can silently
fail — the sprite covers that case). The image comes from Xcursor
(`$XCURSOR_THEME`, `left_ptr`) with a built-in arrow fallback, so the
pointer is never invisible. Clients can replace it per-surface via
`wl_pointer.set_cursor` (rendered as the software sprite).

Cursor images are copied with their REAL row stride (client surfaces
follow the client's own padding; the built-in/Xcursor image lives in a
64-px-stride cell). A historic bug read these buffers as tightly packed,
which sheared the arrow into ~8 diagonal dots — the exact corruption
seen on real hardware. The headless harness now asserts the exact
built-in arrow bitmap (73 black + 75 white pixels at the sprite
position), so that class of bug cannot return silently.
`VANTAGE_WL_CURSOR=builtin` forces the built-in arrow (tests, and a
manual override when a theme misbehaves).

## The compositor panel

The native Wayland session draws a REAL panel with the compositor itself
(vt-wl-panel.c) — not XWayland, not X11 clients, and not placeholder
blocks:

* LEFT: **Programs** button → categorized application menu built from
  XDG `.desktop` entries via the SHARED vt-apps database (identical
  parser, locale handling and category table as the X11 panel — there
  is no second app list) with a **search bar** (type to filter by name
  and keywords, BackSpace edits, Escape closes), **scrolling** (wheel
  over the list, scrollbar indicator), **icon-theme icons** (the user's
  configured theme via vt-icons) and a Quit Session entry; window list
  of xdg toplevels (click to focus, right-click to close)
* RIGHT: workspace buttons (with glow on the active one), network
  indicator (real `/sys/class/net` state, wired + wireless quality),
  volume (native ALSA mixer — real values, wheel adjusts, popup slider),
  clock with a calendar popup (previous/next month), username menu with
  Lock Screen / Suspend / Switch User / Log Out / Reboot / Shutdown /
  Exit Session

Text is rasterized with FreeType + fontconfig **with per-codepoint font
fallback** (a second face is matched lazily when the primary sans lacks
a glyph — Cyrillic/CJK app names render correctly); a built-in bitmap
font is the fallback so labels never disappear.

Applications actually launch: the `Exec=` line is cleaned of field codes
(`%f %u …`), `Terminal=true` entries are wrapped in a real terminal
emulator, and children inherit `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` from
the compositor (it exported them when it created the socket). Launched
children are auto-reaped (`SIGCHLD: SIG_IGN`) — no zombies.

Session actions route through the SESSION MANAGER when one is running:
"Log Out"/"Exit Session"/"Reboot"/"Shutdown" send the session IPC
message, the supervisor then SIGTERMs every child (the compositor's
unwind restores the CRTC and returns the VT to text mode) and the
session exits — **it never restarts the compositor on a clean logout**
(see "Logout that actually returns to the TTY" below). Lock/Suspend
spawn `loginctl` detached — the compositor event loop never blocks on
`system()`. Failures are reported honestly in the menu status line.

Real-client protocol surface: `wl_compositor`, `wl_shm`, `wl_seat`
(pointer + keyboard, xkb keymaps), `wl_output`, `xdg_wm_base`
(toplevels with move/resize/maximize/fullscreen, popups),
**`wl_subcompositor`/`wl_subsurface`** (GTK/Qt overlays paint relative
to their parent) and **`wl_data_device_manager`** (in-session clipboard:
selection tracking, per-client offers, `data_offer.receive` pipe
through to the source).

## Input requirements (no session manager)

When the session runs through elogind/logind or seatd, input devices are
opened through the seat automatically. On a bare TTY login with no
session manager, Linux requires the user to be in the `input` group
before `/dev/input/event*` can be opened:

```sh
sudo usermod -aG input $USER   # then log out and back in
```

Without it the compositor logs a loud diagnostic
(`wayland: input: NO usable input devices …`) and keyboard/pointer will
not work — that is a permission problem, not a Vantage bug.

## Logout that actually returns to the TTY

Two independent mechanisms guarantee "Log Out" leaves the Wayland
session instead of re-taking the screen:

1. The panel routes logout through the session manager's IPC socket
   (`$XDG_RUNTIME_DIR/vantage-session.sock`): the supervisor enters
   SHUTDOWN, SIGTERMs every child, and exits. The compositor's own
   unwind restores the saved CRTC and returns the VT to text mode.
2. The supervisor treats a CRITICAL component exiting CLEANLY (status
   0 — exactly what the compositor's logout unwind does) as an
   intentional logout and ends the session instead of restarting it.
   Crashes (signal / nonzero status) still restart for self-healing,
   bounded at 8 attempts.

`vantage-remote logout` takes the same IPC path (session socket first,
compositor fallback for standalone runs).

## Escape hatches / recovery

The compositor owns the keyboard via evdev, so the kernel's own
Ctrl+Alt+F1..F12 console switching never fires. Vantage implements it
itself:

| Key                    | Action                                        |
|------------------------|-----------------------------------------------|
| Ctrl+Alt+F1..F12       | switch to that VT (DRM master dropped/retaken, CRTC restored) |
| Ctrl+Alt+Delete        | clean logout (graceful unwind, exit 0)        |
| Ctrl+Alt+Left/Right    | previous/next workspace                       |
| Alt+F4                 | close the focused window                      |
| Escape                 | close any open panel menu                     |

A wedged session therefore never requires a hardware reboot: switch to
another VT with Ctrl+Alt+F3, log in, and either `vantage-remote logout`
(the IPC socket lives in $XDG_RUNTIME_DIR) or `pkill -TERM vantage-wm`
— SIGTERM takes the same clean-unwind path (CRTC restore, VT text
mode, exit 0).

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
