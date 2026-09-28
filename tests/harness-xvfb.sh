#!/bin/bash
# harness-xvfb.sh — Vantage full-session integration harness (Xorg path)
#
# SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
#
# Boots the REAL session stack on a fresh Xvfb display, exercising the
# X11 backend selection exactly as a user would:
#
#   Xvfb :N  →  vantage-session --x11
#                 ├─ vantage-wm --x11 (EWMH/ICCCM WM + XRender compositor)
#                 ├─ vantage-panel    (dock with struts)
#                 └─ vantage-desktop  (root desktop window)
#
# vantage-session --x11 connects to the existing $DISPLAY server (here
# Xvfb — any conforming X11 server works the same way) and never starts
# an X server of its own.
#
# Verifications:
#   1. session reaches "ready" stage
#   2. EWMH _NET_SUPPORTING_WM_CHECK → _NET_WM_NAME == "Vantage"
#   3. vt-x11-testclient maps two windows; vantage-remote lists them
#   4. IPC window management works (focus + close via vantage-remote)
#   5. root screenshot contains the test windows' known colors
#   6. VISIBLE cursor: XDefineCursor on root + XFixes opaque pixels
#   7. Wayland nesting refusal (a compositor never nests)
#   8. session shuts down cleanly on SIGTERM (all children exit)
#   9. vantage-remote logout round-trip (graceful, exit 0, no SIGKILL)
#
# Usage: harness-xvfb.sh [build-dir]     (default: $VT_BUILD_DIR or PATH)

set -u
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); printf '  FAIL: %s\n' "$1" >> "$FAILS"; }

# ---------------------------------------------------------------- paths
BUILD="${1:-${VT_BUILD_DIR:-}}"
if [ -n "$BUILD" ] && [ -x "$BUILD/src/tools/vantage-session" ]; then
  BIN="$BUILD/src/tools"
  TST="$BUILD/tests"
  # the panel is an independent subproject — its binary lives there
  PANELBIN="$BUILD/subprojects/panel"
  export PATH="$BIN:$PANELBIN:$PATH"
  [ -x "$PANELBIN/vantage-panel" ] \
    || { echo "vantage-panel not built (subprojects/panel)"; exit 77; }
else
  BIN=""; TST=""
  for b in vantage-session vantage-wm vantage-panel vantage-desktop vantage-remote; do
    command -v "$b" >/dev/null 2>&1 || { echo "missing $b in PATH"; exit 77; }
  done
fi
vb() {
  if [ -z "$BIN" ]; then echo "$1"; return; fi
  if [ -x "$BIN/$1" ]; then echo "$BIN/$1"; return; fi
  if [ -n "${PANELBIN:-}" ] && [ -x "$PANELBIN/$1" ]; then
    echo "$PANELBIN/$1"; return
  fi
  echo "$1"
}
tc() { if [ -n "$TST" ]; then echo "$TST/$1"; else echo "$1"; fi; }

command -v Xvfb >/dev/null 2>&1 || { echo "Xvfb not found"; exit 77; }
[ -x "$(tc vt-x11-testclient)" ] || { echo "vt-x11-testclient not built"; exit 77; }

# --------------------------------------------------------- isolated env
WORK=$(mktemp -d /tmp/vantage-xvfb.XXXXXX)
# failures are recorded so the TAIL of a truncated meson log (which
# only keeps the last 100 lines — after any log dumps) still shows
# exactly WHICH checks failed
FAILS="$WORK/fails.txt"; : > "$FAILS"
mkdir -p "$WORK/run" "$WORK/config/vantage" "$WORK/share/applications" \
         "$WORK/data-dirs"
export XDG_RUNTIME_DIR="$WORK/run"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_DATA_HOME="$WORK/share"        # isolate XDG autostart
# XDG_DATA_DIRS isolation: vt-apps falls back to /usr/local/share:/usr/share
# when it is unset, so host-installed applications would leak into the
# Programs menu and make the pixel checks machine-dependent. An empty
# dir keeps the menu DB deterministic on every machine.
export XDG_DATA_DIRS="$WORK/data-dirs"
# deterministic menu CONTENT: probe applications in the isolated
# XDG_DATA_HOME — the Programs menu renders REAL rows on every machine
# (before, host-installed apps leaked in via the /usr/share fallback
# and provided the text the pixel checks looked for). The probe sorts
# alone in Utility → "app row 0" is exactly this application.
cat > "$XDG_DATA_HOME/applications/vt-harness-probe.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Zz Harness Probe
Exec=/bin/sh -c 'echo launched > $WORK/x11-launch-marker'
Icon=vt-harness-probe
Categories=Utility;
DESK
sed -i "s|\$WORK|$WORK|g" "$XDG_DATA_HOME/applications/vt-harness-probe.desktop"
# deterministic ICON: private icon theme + solid-color PNG — proves the
# X11 launcher's icon-theme lookup → PNG decode → ARGB → XRender path
ICON_DIR="$XDG_DATA_HOME/icons/vt-harness-theme/24x24/apps"
mkdir -p "$ICON_DIR"
python3 - "$ICON_DIR/vt-harness-probe.png" <<'PYICON'
import struct, zlib, sys
w = h = 24
rgb = (0xc0, 0x40, 0x80)   # unique pink-magenta: used nowhere else
raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack(
        '>I', zlib.crc32(c) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(data)
PYICON
# themed START icon: a solid gold "start-here" in the fixture theme —
# proves the Programs button uses the ICON THEME (like an XFCE start
# button), falling back to the drawn glyph only when themes lack it
python3 - "$ICON_DIR/start-here.png" <<'PYICON2'
import struct, zlib, sys
w = h = 24
rgb = (0xd4, 0xb1, 0x06)   # distinctive gold — used nowhere else
raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack(
        '>I', zlib.crc32(c) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(data)
PYICON2
# GTK's OWN fallback icon: gtk_icon_theme_lookup_icon() recurses into
# itself WITHOUT BOUND when the icon it falls back to ("image-missing")
# is also missing from every theme — GTK 4.18 has no recursion guard,
# so any GTK-internal missing-icon lookup would overflow the stack.
# Shipping it makes the fixture themes as complete as real ones.
python3 - "$ICON_DIR/image-missing.png" <<'PYICON3'
import struct, zlib, sys
w = h = 24
px = [[(28, 28, 30)] * w for _ in range(h)]
for i in range(6, 18):                      # red X on a slate plate
    px[i][i] = px[i][23 - i] = (192, 57, 43)
    px[i][23 - i] = px[i][i] = (192, 57, 43)
raw = b''.join(b'\x00' + b''.join(bytes(p) for p in row) for row in px)
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack(
        '>I', zlib.crc32(c) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(data)
PYICON3
# GTK4 panel resolves icons via GtkIconTheme (hicolor fallback):
# duplicate both fixtures into hicolor so the lookup is deterministic
HICOLOR_DIR="$XDG_DATA_HOME/icons/hicolor/24x24/apps"
mkdir -p "$HICOLOR_DIR"
cp "$ICON_DIR/start-here.png" "$HICOLOR_DIR/"
cp "$ICON_DIR/vt-harness-probe.png" "$HICOLOR_DIR/"
cp "$ICON_DIR/image-missing.png" "$HICOLOR_DIR/"
cat > "$XDG_DATA_HOME/icons/hicolor/index.theme" <<'EOF'
[Icon Theme]
Name=hicolor
Directories=24x24/apps

[24x24/apps]
Size=24
Context=Applications
Type=Fixed
EOF

cat > "$XDG_DATA_HOME/icons/vt-harness-theme/index.theme" <<'EOF'
[Icon Theme]
Name=vt-harness-theme
Directories=24x24/apps

[24x24/apps]
Size=24
Context=Applications
Type=Fixed
EOF
export VANTAGE_ICON_THEME=vt-harness-theme
cat > "$XDG_DATA_HOME/applications/vt-harness-term.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Zz Harness Terminal
Exec=/bin/true
Terminal=true
Categories=System;
DESK
rm -f "$WORK/x11-launch-marker"
# never execute host autostart entries (/etc/xdg/autostart — e.g. the
# machine's real PipeWire/wireplumber) from inside a test session
export VANTAGE_SESSION_NO_SYSTEM_AUTOSTART=1
export XDG_CONFIG_DIRS=""
chmod 700 "$XDG_RUNTIME_DIR"

# --- GTK determinism on REAL machines ---------------------------------
# Same isolation as harness-wayland: the developer desktop's session
# environment must not leak into the GTK4 panel (theme/dark-mode via
# GTK_THEME pin, GApplication registration + portal settings via the
# session bus, HiDPI factors via GDK_SCALE).
export GTK_THEME=Adwaita
unset GDK_SCALE GDK_DPI_SCALE GTK_PATH GTK_MODULES GTK_USE_PORTAL \
      DBUS_SESSION_BUS_ADDRESS DBUS_STARTER_BUS_TYPE \
      DBUS_STARTER_ADDRESS GIO_MODULE_DIR

# Isolated config; backend selection comes from the --x11 flag (the
# config default, backend=auto, would resolve the same way here because
# $DISPLAY is set).
cat > "$XDG_CONFIG_HOME/vantage/vantage.conf" <<EOF
[desktop]
compositor=true
[wm]
focus-new=true
[panel]
enabled=true
EOF

# ---------------------------------------------------------------- Xvfb
export DISPLAY=":$$"        # unique display number per harness run
XVFB_PID=""
start_xvfb() {
  Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp &
  XVFB_PID=$!
  for i in $(seq 1 50); do
    if xdpyinfo >/dev/null 2>&1 || [ -S "/tmp/.X11-unix/X${DISPLAY#:}" ]; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}
if ! command -v xdpyinfo >/dev/null 2>&1; then
  # no xdpyinfo: wait for the socket file instead
  start_xvfb() {
    Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp &
    XVFB_PID=$!
    local sock="/tmp/.X11-unix/X${DISPLAY#:}"
    for i in $(seq 1 50); do
      [ -S "$sock" ] && return 0
      sleep 0.1
    done
    return 1
  }
fi

echo "== harness-xvfb: starting Xvfb on $DISPLAY =="
if ! start_xvfb; then
  echo "  FAIL: Xvfb did not start"; exit 1
fi
ok "Xvfb running (pid $XVFB_PID)"

# ------------------------------------------------------------- session
echo "== harness-xvfb: launching vantage-session --x11 =="
"$(vb vantage-session)" --x11 > "$WORK/session.log" 2>&1 &
SESS_PID=$!

READY=""
for i in $(seq 1 100); do
  grep -q "session: ready" "$WORK/session.log" 2>/dev/null && { READY=1; break; }
  kill -0 "$SESS_PID" 2>/dev/null || break
  sleep 0.1
done
if [ -n "$READY" ]; then ok "session reached ready state"; else
  bad "session never became ready"; tail -20 "$WORK/session.log"; fi

grep -q "display backend: X11" "$WORK/session.log" \
  && ok "session selected the X11 backend" \
  || bad "session did not report the X11 backend"
grep -q "X server:" "$WORK/session.log" \
  && ok "session identified the X server implementation (informational)" \
  || bad "no X server identification in the log"

sleep 1     # let the WM/desktop settle and paint
# the PANEL must be up too: its "ready" line lands in the session log
# (children's stderr is merged there). A fixed sleep races GTK's
# startup on fast machines — poll for the marker instead, so the
# pixel checks below always photograph a painted panel.
PANEL_READY_X=""
for i in $(seq 1 100); do
  grep -q "vantage-panel: ready" "$WORK/session.log" 2>/dev/null \
    && { PANEL_READY_X=1; break; }
  kill -0 "$SESS_PID" 2>/dev/null || break
  sleep 0.1
done
[ -n "$PANEL_READY_X" ] \
  && ok "GTK4 panel client started (EWMH dock, struts reserved)" \
  || bad "panel never reached ready: $(tail -3 "$WORK/session.log")"
sleep 0.5   # first frame + strut settle

# ------------------------------------------------------ cursor checks
echo "== harness-xvfb: visible root cursor =="
# The WM pins a themed root cursor (Xcursor → font → hard-coded arrow)
# instead of relying on the server default, which can be invisible on
# bare Xorg/XLibre with no cursor theme loaded.
WM_LOG="$WORK/wm.log"
# the WM is spawned by the session; find its log via the session log is
# not possible (stderr is merged), so verify behaviorally instead:
CURSOR_OUT=$("$(tc vt-x11-testclient)" --cursor-probe 2>&1)
CURSOR_RC=$?
echo "$CURSOR_OUT" | grep -qE 'cursor-pos=[0-9]+,[0-9]+' \
  && ok "pointer position queryable (XQueryPointer)" \
  || bad "XQueryPointer failed: $CURSOR_OUT"
OPAQUE=$(echo "$CURSOR_OUT" | grep -oE 'cursor-opaque=[-0-9]+' | cut -d= -f2)
if [ "$CURSOR_RC" -eq 0 ] && [ "${OPAQUE:-0}" -gt 0 ]; then
  ok "cursor visible: ${OPAQUE} opaque pixels in the cursor image"
else
  bad "cursor invisible or empty (opaque=${OPAQUE:-none}, rc=$CURSOR_RC)"
fi
C_SIZE=$(echo "$CURSOR_OUT" | grep -oE 'cursor-size=[0-9]+x[0-9]+' | head -1)
[ -n "$C_SIZE" ] && ok "cursor image reported ($C_SIZE)" \
  || bad "no cursor size reported"

# ------------------------------------------------- nesting refusal
echo "== harness-xvfb: Wayland compositor nesting refusal =="
# A compositor never nests: with WAYLAND_DISPLAY already set, the
# Wayland backend must refuse to start instead of stacking on another
# compositor session.
NEST_RC=0
WAYLAND_DISPLAY=vantage-nest-test "$(vb vantage-wm)" --wayland \
  > "$WORK/nest.log" 2>&1 || NEST_RC=$?
if [ "$NEST_RC" -ne 0 ] && grep -qi "refusing to nest" "$WORK/nest.log"; then
  ok "compositor refuses to nest behind WAYLAND_DISPLAY"
else
  bad "vantage-wm --wayland did not refuse nesting (rc=$NEST_RC)"
fi
SESS_WL_RC=0
"$(vb vantage-session)" --wayland > "$WORK/nest-sess.log" 2>&1 || SESS_WL_RC=$?
if [ "$SESS_WL_RC" -ne 0 ] && grep -q "DRM master" "$WORK/nest-sess.log"; then
  ok "vantage-session --wayland refuses a live-X launch (DRM master policy)"
else
  bad "session --wayland did not refuse the live-X launch (rc=$SESS_WL_RC)"
fi

# ------------------------------------------------------------- checks
echo "== harness-xvfb: EWMH WM identity =="
EWMH_OUT=$("$(tc vt-x11-testclient)" --ewmh-probe 2>&1)
echo "$EWMH_OUT" | grep -q "ewmh-wm-name=Vantage" \
  && ok "EWMH WM is Vantage" || bad "EWMH probe: $EWMH_OUT"

echo "== harness-xvfb: window management via IPC =="
# Run the client in the background so the WM queries/IPC ops below happen
# while its windows are still mapped (the client lives --seconds seconds).
CLIENT_LOG="$WORK/client.log"
rm -f "$CLIENT_LOG"
"$(tc vt-x11-testclient)" --seconds 8 --title "Vantage Test" \
  > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

# wait for the windows to be mapped (output is flushed per line)
MAPPED=""
for i in $(seq 1 100); do
  grep -q "^mapped" "$CLIENT_LOG" 2>/dev/null && { MAPPED=1; break; }
  kill -0 "$CLIENT_PID" 2>/dev/null || break
  sleep 0.05
done
grep -q "^connected" "$CLIENT_LOG" && ok "test client connected to $DISPLAY"
[ -n "$MAPPED" ] && ok "two windows mapped" || bad "test client never mapped windows"
sleep 0.5     # let the WM manage + focus them

WMLIST=$("$(vb vantage-remote)" list 2>&1)
echo "$WMLIST" | grep -q "Vantage Test" && ok "vantage-remote lists managed window" \
  || bad "vantage-remote list: $WMLIST"
NWIN=$(echo "$WMLIST" | grep -c . )
[ "${NWIN:-0}" -ge 2 ] && ok "WM tracks multiple windows ($NWIN)" \
  || bad "expected >=2 managed windows, got ${NWIN:-0}"

# focus + maximize + close the test window through IPC (while it is alive)
WID=$(echo "$WMLIST" | grep "Vantage Test" | head -1 | cut -f1)
if [ -n "$WID" ]; then
  "$(vb vantage-remote)" focus "$WID" >/dev/null 2>&1 \
    && ok "IPC focus works" || bad "IPC focus failed"
  sleep 0.3
  "$(vb vantage-remote)" maximize "$WID" >/dev/null 2>&1 \
    && ok "IPC maximize works" || bad "IPC maximize failed"
  sleep 0.3
  "$(vb vantage-remote)" close "$WID" >/dev/null 2>&1 \
    && ok "IPC close works" || bad "IPC close failed"
  sleep 0.3
  WMLIST2=$("$(vb vantage-remote)" list 2>&1)
  echo "$WMLIST2" | grep -q "Vantage Test" \
    && bad "closed window still listed" || ok "closed window removed from list"
else
  bad "no window id found for IPC ops"
fi

echo "== harness-xvfb: window decorations (SSD frames) =="
FRAME_LOG="$WORK/frame.log"
"$(tc vt-x11-testclient)" --frame-probe > "$FRAME_LOG" 2>&1
FRC=$?
if [ $FRC -eq 0 ] && grep -q "framed=yes" "$FRAME_LOG"; then
  ok "client window reparented into a WM frame"
  grep -q "frame-extents=" "$FRAME_LOG" && ok "_NET_FRAME_EXTENTS reported: $(grep -o 'frame-extents=[0-9,]*' "$FRAME_LOG")"
else
  bad "no server-side decorations: $(cat "$FRAME_LOG")"
fi

echo "== harness-xvfb: CSD windows stay undecorated (_MOTIF_WM_HINTS) =="
# GTK/Chromium/Firefox windows that draw their own headerbars set
# _MOTIF_WM_HINTS decorations=0. A WM that ignores this DOUBLE-DECORATES
# them — its own titlebar stacked over the app's (the reported browser
# bug on the X11 session).
MOTIF_LOG="$WORK/motif.log"
"$(tc vt-x11-testclient)" --motif-probe > "$MOTIF_LOG" 2>&1
MRC=$?
if [ $MRC -eq 0 ] && grep -q "motif-undecorated=yes" "$MOTIF_LOG"; then
  ok "CSD window left undecorated (no double titlebar)"
else
  bad "MOTIF CSD window got decorated anyway: $(cat "$MOTIF_LOG")"
fi

echo "== harness-xvfb: narrow-window regression (title ellipsis wedge) =="
# "opening any app locks the entire X11 DE": a NARROW window with a
# LONG title used to spin the WM forever inside the frame-title
# truncation loop (the working string GREW 2 bytes per pass, so the
# measured width never fell back under the limit) — 100% CPU, frozen
# X server under CompositeRedirect, kill-from-TTY territory. Map three
# of them, then prove the WM answers IPC and idles.
NARROW_LOG="$WORK/narrow.log"
"$(tc vt-x11-testclient)" --narrow-probe --seconds 6 > "$NARROW_LOG" 2>&1 &
NARROW_PID=$!
NMAPPED=0
for i in $(seq 1 100); do
  # grep -c ALWAYS prints a count (0 included) — a `|| echo 0` fallback
  # would append a second 0 on the zero case and later [ ] comparisons
  # would see "0\n0" (integer expression expected)
  NMAPPED=$(grep -c "^mapped" "$NARROW_LOG" 2>/dev/null)
  NMAPPED=${NMAPPED:-0}
  [ "${NMAPPED:-0}" -ge 3 ] && break
  kill -0 "$NARROW_PID" 2>/dev/null || break
  sleep 0.05
done
[ "${NMAPPED:-0}" -ge 3 ] && ok "three narrow long-title windows mapped" \
  || bad "narrow windows did not map (${NMAPPED:-0})"
WMCPU_PID=$(pgrep -f "$(vb vantage-wm)" 2>/dev/null | head -1)
CPU0=""
[ -n "$WMCPU_PID" ] && \
  CPU0=$(awk '{print $14+$15}' "/proc/$WMCPU_PID/stat" 2>/dev/null)
sleep 0.7
NLIST=$(timeout 5 "$(vb vantage-remote)" list 2>&1); NRC=$?
echo "$NLIST" | grep -q "Narrow Title Probe" \
  && ok "WM responsive with narrow windows on screen" \
  || bad "WM unresponsive with narrow windows (rc=$NRC)"
sleep 1.5
CPU1=""
[ -n "$WMCPU_PID" ] && \
  CPU1=$(awk '{print $14+$15}' "/proc/$WMCPU_PID/stat" 2>/dev/null)
if [ -n "$CPU0" ] && [ -n "$CPU1" ]; then
  NDELTA=$((CPU1 - CPU0))
  [ "$NDELTA" -lt 100 ] \
    && ok "WM idles after narrow-title paint (cpu +${NDELTA} ticks/2.2s)" \
    || bad "WM spins after narrow-title paint (+${NDELTA} ticks — ellipsis wedge?)"
fi
wait "$NARROW_PID" 2>/dev/null

echo "== harness-xvfb: shell chrome classification (D/K flags) =="
# "the first workspace shows as entirely used + two running apps
# (Vantage panel / Vantage wm)": shell chrome must NOT count as task
# content. WM_QUERY now flags docks (D) and desktops (K); the panel's
# taskbar and the pager filter on exactly these flags.
CHROME_LIST=$(timeout 5 "$(vb vantage-remote)" list 2>&1)
PANEL_FLAGS=$(echo "$CHROME_LIST" | awk -F'\t' '$2 ~ /Vantage Panel/ {print $4; exit}')
DESK_FLAGS=$(echo "$CHROME_LIST" | awk -F'\t' '$2 ~ /Vantage Desktop/ {print $4; exit}')
case "$PANEL_FLAGS" in
  *D*) ok "panel window flagged D (_NET_WM_WINDOW_TYPE_DOCK) — no taskbar entry" ;;
  *)   bad "panel not flagged as dock: flags='${PANEL_FLAGS:-none}' (taskbar pollution)" ;;
esac
case "$DESK_FLAGS" in
  *K*) ok "desktop window flagged K (_NET_WM_WINDOW_TYPE_DESKTOP) — no pager cell fill" ;;
  *)   bad "desktop not flagged as desktop-type: flags='${DESK_FLAGS:-none}' (pager cell filled)" ;;
esac

echo "== harness-xvfb: focus policy (click-to-focus, no hover steal) =="
FOCUS_LOG="$WORK/focus.log"
"$(tc vt-x11-testclient)" --focus-probe > "$FOCUS_LOG" 2>&1
FRC=$?
grep -q "hover-steals=no" "$FOCUS_LOG" && ok "hover does NOT steal keyboard focus" \
  || bad "hover steals focus: $(cat "$FOCUS_LOG")"
grep -q "click-focus=yes" "$FOCUS_LOG" && ok "click focuses the window" \
  || bad "click did not focus: $(cat "$FOCUS_LOG")"
[ $FRC -eq 0 ] && ok "focus probe exit status 0" || bad "focus probe rc=$FRC"

echo "== harness-xvfb: pointer-freeze regression (the 'any app locks the DE' bug) =="
FREEZE_LOG="$WORK/nofreeze.log"
"$(tc vt-x11-testclient)" --nofreeze-probe > "$FREEZE_LOG" 2>&1
FRZRC=$?
grep -q "plain-clicks-delivered=2" "$FREEZE_LOG" \
  && ok "plain clicks reach the app (no sync-grab theft)" \
  || bad "plain clicks stolen/frozen: $(cat "$FREEZE_LOG")"
grep -q "post-altclick-motion=[1-9]" "$FREEZE_LOG" \
  && ok "pointer alive after Alt+click on a fullscreen window" \
  || bad "pointer FROZEN after Alt+click on fullscreen: $(cat "$FREEZE_LOG")"
[ $FRZRC -eq 0 ] && ok "nofreeze probe exit status 0" || bad "nofreeze probe rc=$FRZRC"

echo "== harness-xvfb: continuous geometry feed during a drag (~30 fps) =="
# The pager's live feed — the X11 twin of the Wayland geo-flow probe,
# driven through REAL XTest events (the exact user path of a mouse
# title-bar drag). The WM throttles window-geometry broadcasts to
# ~30 fps during drags (op_last_geo_us / 33 ms, both backends); the
# panel's pager redraws from that feed instead of catching up on
# release. The test client announces its grab point and settles for
# 1 s — the subscriber below attaches in that window — then drags for
# ~2 s at ~80 motions/s; expect ~60 events (35..75) from ~160 motions.
DFLOW_LOG="$WORK/dragflow.log"
"$(tc vt-x11-testclient)" --drag-flow-probe > "$DFLOW_LOG" 2>&1 &
DFLOW_PID=$!
DFLOW_GRAB=""
for i in $(seq 1 100); do
  DFLOW_GRAB=$(grep -o '^grab [0-9-]* [0-9-]*$' "$DFLOW_LOG" 2>/dev/null | head -1)
  [ -n "$DFLOW_GRAB" ] && break
  kill -0 "$DFLOW_PID" 2>/dev/null || break
  sleep 0.05
done
XGEO=""
if [ -n "$DFLOW_GRAB" ]; then
  XGEO=$(python3 - <<'PYXG'
import os, socket, struct, sys, time
# subscribe to the WM's IPC event socket (VT_IPC_DEFAULT_SOCKET —
# vantage.sock, the same endpoint vantage-remote connects to) and
# count window-geometry broadcasts while the XTest drag runs
sock = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "vantage.sock")
if not os.path.exists(sock):
    print("no-socket"); sys.exit(1)
MAGIC, SUB = 0x56544352, 0x0005
TYPE_REQ, TYPE_RESP = 1, 2
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
events = b""
def req(msg_id, payload=b""):
    global events
    s.sendall(struct.pack("<IIII", MAGIC, msg_id, TYPE_REQ,
                          len(payload)) + payload)
    buf = b""
    s.settimeout(5)
    while True:
        while len(buf) >= 16:
            magic, mid, mtype, plen = struct.unpack("<IIII", buf[:16])
            if magic != MAGIC:
                buf = buf[1:]; continue
            if len(buf) < 16 + plen:
                break
            msg = buf[16:16 + plen]; buf = buf[16 + plen:]
            if mtype == TYPE_RESP:
                return msg
            events += msg
        chunk = s.recv(65536)
        if not chunk:
            # EOF must fail fast: recv()==b'' is not an error and
            # would otherwise hot-spin forever (no timeout fires on it)
            raise RuntimeError("IPC server closed the connection")
        buf += chunk
req(SUB)          # subscribe; the ack confirms the WM is listening
# ~1 s settle + ~2.1 s drag + tail, all inside a 3.6 s window.
# Frames are parsed properly (a raw byte-append could split the
# "window-geometry" string across a recv boundary and undercount).
s.settimeout(0.25)
buf = b""
t0 = time.time()
while time.time() - t0 < 3.6:
    try:
        chunk = s.recv(65536)
    except socket.timeout:
        continue
    if not chunk:
        break
    buf += chunk
    while len(buf) >= 16:
        magic, mid, mtype, plen = struct.unpack("<IIII", buf[:16])
        if magic != MAGIC:
            buf = buf[1:]; continue
        if len(buf) < 16 + plen:
            break
        events += buf[16:16 + plen]
        buf = buf[16 + plen:]
geo = events.count(b"window-geometry")
print(f"geometry={geo}")
sys.exit(0 if 35 <= geo <= 75 else 1)
PYXG
)
fi
wait "$DFLOW_PID" 2>/dev/null
DFLOW_MOTIONS=$(grep -o '^motions=[0-9]*' "$DFLOW_LOG" 2>/dev/null | cut -d= -f2)
DFLOW_GEO=$(echo "$XGEO" | grep -o 'geometry=[0-9]*' | cut -d= -f2)
if [ -n "$DFLOW_GRAB" ] && [ "${DFLOW_MOTIONS:-0}" -gt 60 ] \
   && [ -n "$DFLOW_GEO" ] && [ "$DFLOW_GEO" -ge 35 ] && [ "$DFLOW_GEO" -le 75 ]; then
  ok "geometry events flow during the drag ($DFLOW_MOTIONS motions -> $DFLOW_GEO events at ~30 fps — the pager's live feed)"
else
  bad "geometry feed wrong during drag: motions=${DFLOW_MOTIONS:-0} geo=${DFLOW_GEO:-none} (${XGEO:-no subscriber})"
fi

echo "== harness-xvfb: pixel verification =="
# screenshot while the second test window is still alive
"$(tc vt-x11-testclient)" --seconds 1 --screenshot "$WORK/shot.ppm" \
  --title "Vantage Shot" > "$WORK/shot.log" 2>&1
grep -q "screenshot" "$WORK/shot.log" || bad "no screenshot taken"
wait "$CLIENT_PID" 2>/dev/null

python3 - "$WORK/shot.ppm" <<'PYEOF'
import sys
p = sys.argv[1]
with open(p, 'rb') as f:
    data = f.read()
# parse P6 header (whitespace/comment tolerant)
i = data.find(b'P6')
vals, pos = [], i + 2
while len(vals) < 3:
    while data[pos:pos+1].isspace(): pos += 1
    if data[pos:pos+1] == b'#':
        while data[pos:pos+1] not in (b'\n', b''): pos += 1
        continue
    j = pos
    while not data[j:j+1].isspace(): j += 1
    vals.append(int(data[pos:j])); pos = j
pos += 1
w, h, _ = vals
pix = data[pos:pos + w*h*3]
c1 = bytes((0x3a, 0x5f, 0x9a))   # test window 1
c2 = bytes((0x9a, 0x3a, 0x5f))   # test window 2
hits1 = pix.count(c1)
hits2 = pix.count(c2)
distinct = len(set(pix[i:i+3] for i in range(0, min(len(pix), w*3*40), 3)))
# the Programs button's THEMED start icon (solid gold PNG from the
# fixture theme) — proves the button uses the icon theme, not a glyph
start_icon = pix[:w*46*3].count(bytes((0xd4, 0xb1, 0x06)))
# the workspace PAGER miniatures: every test window lives on ws 1, so
# the CURRENT (accent) cell carries their miniatures — the focused
# one near-white, the others slate. Scan the CELL BAND as a real 2D
# region (rows 3..36, cols 145..420 — measured on the X11 dock):
# the previous flat slice bar[120*3:420*3] read exactly ONE row (row
# 0: pure bar background), so every count came back zero even with
# a perfect pager. ±2 tolerance: GTK's rgba→pixel conversion rounds
# the slate fill to #9ea3ae (one LSB off the nominal #9ea3ad).
def pnp(x, y):
    i = (y*w + x)*3
    return (pix[i], pix[i+1], pix[i+2])
pager_foc = pager_unf = pager_bg = 0
for y in range(3, 37):
    for x in range(145, 421):
        r, g, b = pnp(x, y)
        if r >= 0xe0 and g >= 0xe0 and b >= 0xe0:
            pager_foc += 1
        elif abs(r-0x9e) <= 2 and abs(g-0xa3) <= 2 and abs(b-0xad) <= 2:
            pager_unf += 1
        elif abs(r-0x47) <= 2 and abs(g-0x75) <= 2 and abs(b-0xc7) <= 2:
            pager_bg += 1
print(f"pixels: {w}x{h}, window1={hits1}px, window2={hits2}px, distinct-colors(top40rows)={distinct}, "
      f"start-icon-gold={start_icon}, pager-focused={pager_foc}, "
      f"pager-unfocused={pager_unf}, pager-bg={pager_bg}")
ok = hits1 > 500 and hits2 > 500 and distinct >= 3
sys.exit(0 if (ok and start_icon > 60 and pager_foc > 4 and
               pager_unf > 4 and pager_bg > 20) else 1)
PYEOF
# capture the pixel verdict IMMEDIATELY: a later [ $? -eq 0 ] tested
# the if/fi block above instead of this python — the check's failure
# (pager miniatures missing, start icon gone, …) was silently
# swallowed and the harness reported PASS
PIXEL_RC=$?
[ "$PIXEL_RC" -eq 0 ] && ok "screenshot shows managed windows + painted desktop" \
    || bad "screenshot pixel check failed (windows/pager/start-icon)"

echo "== harness-xvfb: Programs menu (shared app database) =="
# deterministic application database — the panel process reads
# XDG_DATA_HOME at .desktop scan time
# click the Programs button (panel top-left; the GTK panel is 45 rows
# tall), screenshot, verify the menu: dark popover + light text/icons
"$(tc vt-x11-testclient)" --seconds 2 --click 60,22 \
    --screenshot "$WORK/menu.ppm" > "$WORK/menu.log" 2>&1 &
MPID=$!
for i in $(seq 1 40); do
  [ -s "$WORK/menu.ppm" ] && break
  sleep 0.1
done
sleep 0.4
if [ -s "$WORK/menu.ppm" ]; then
  python3 - "$WORK/menu.ppm" <<'PYMENU'
import sys
p = sys.argv[1]
with open(p, 'rb') as f:
    data = f.read()
vals, pos = [], data.find(b'P6') + 2
while len(vals) < 3:
    while data[pos:pos+1].isspace(): pos += 1
    j = pos
    while not data[j:j+1].isspace(): j += 1
    vals.append(int(data[pos:j])); pos = j
pos += 1
w, h, _ = vals
pix = data[pos:pos + w*h*3]
# the GTK popover spans ~560px below the 45-row panel: dark
# translucent backdrop + rendered text. Light-pixel counting is
# FONT-INDEPENDENT (any antialiased text produces bright pixels).
menu_area = pix[(48*w)*3 : (420*w)*3]
light = 0
for i in range(0, len(menu_area), 3):
    if menu_area[i] >= 0x90 and menu_area[i+1] >= 0x90 and menu_area[i+2] >= 0x90:
        light += 1
# the probe application's ICON (solid 0xc04080 PNG) renders through
# GtkIconTheme — icon-theme lookup + decode + GDK compositing.
# The GTK4 panel scales the 24px fixture to the 22px row height with
# filtering — exact-color counting finds ZERO pixels. Count pixels
# NEAR the fixture color (tolerance per channel) instead: the icon's
# resampled body, not its antialiased edge.
near = 0
for i in range(0, len(pix) - 2, 3):
    if abs(pix[i] - 0xc0) <= 24 and abs(pix[i+1] - 0x40) <= 24 and \
       abs(pix[i+2] - 0x80) <= 24:
        near += 1
icon_px = near
print(f"menu: light-text={light} icon-px={icon_px}")
# rendered text + rendered icon = the REAL Programs menu
sys.exit(0 if light > 150 and icon_px > 100 else 1)
PYMENU
  [ $? -eq 0 ] && ok "Programs menu opened (search bar + text rendered)" \
    || bad "Programs menu did not open/render"
else
  bad "no menu screenshot"
fi
kill $MPID 2>/dev/null; wait $MPID 2>/dev/null

# --- application LAUNCH from the menu: the menu is still open (no
# --- FocusOut close in the launcher) — drive the REAL input path:
# --- category row 0 (Accessories, where the probe sorts alone), then
# --- application row 0 — the marker file proves the full X11 launch
# --- chain (row hit-test → .desktop Exec → spawn) end to end
echo "== harness-xvfb: application launch from the Programs menu =="
# the probe sorts alone in the DEFAULT category (Accessories): its row
# is the first in the right column; GTK margins vary per theme, so try
# a few row heights (misses land on empty list space, harmless)
CLICKS=""
for row_y in 120 140 160 180 200 220; do
  CLICKS="${CLICKS:+$CLICKS;}300,$row_y"
done
"$(tc vt-x11-testclient)" --no-windows --seconds 0 \
    --clicks "$CLICKS" > "$WORK/clicks.log" 2>&1
LAUNCHED=""
for i in $(seq 1 30); do
  [ -f "$WORK/x11-launch-marker" ] && { LAUNCHED=1; break; }
  sleep 0.1
done
if [ -n "$LAUNCHED" ] && grep -q "launched" "$WORK/x11-launch-marker"; then
  ok "application LAUNCHED from the Programs menu (marker file)"
else
  bad "application did not launch from the X11 menu"
fi

# ------------------------------------------------------------- shutdown
echo "== harness-xvfb: logout round-trip (graceful) =="
# vantage-remote logout → session IPC → SIGTERM + grace (never SIGKILL as
# the normal path) → exit 0, X server untouched.
LOGOUT_RC=0
"$(vb vantage-remote)" logout > "$WORK/logout.txt" 2>&1 || LOGOUT_RC=$?
[ "$LOGOUT_RC" -eq 0 ] && ok "vantage-remote logout accepted" \
  || bad "vantage-remote logout failed ($(cat "$WORK/logout.txt"))"
EXITED=""
for i in $(seq 1 100); do
  kill -0 "$SESS_PID" 2>/dev/null || { EXITED=1; break; }
  sleep 0.1
done
if [ -z "$EXITED" ]; then
  # graceful logout failed — SIGTERM fallback, counts as failure
  kill -TERM "$SESS_PID" 2>/dev/null
  for i in $(seq 1 100); do
    kill -0 "$SESS_PID" 2>/dev/null || { EXITED=1; break; }
    sleep 0.1
  done
fi
if [ -z "$EXITED" ]; then
  kill -KILL "$SESS_PID" 2>/dev/null   # last resort; counts as failure
fi
SESS_RC=0
wait "$SESS_PID" 2>/dev/null || SESS_RC=$?
[ -n "$EXITED" ] && ok "session exited after logout" \
  || bad "session ignored logout"
[ "$SESS_RC" -eq 0 ] && ok "logout exit status is 0 (no SIGKILL)" \
  || bad "session exit status $SESS_RC (SIGKILLed?)"
grep -q "policy: SIGTERM" "$WORK/session.log" \
  && ok "session logged the graceful shutdown policy" \
  || bad "no shutdown-policy log line"
grep -q "session: exited" "$WORK/session.log" \
  && ok "session logged clean exit" || bad "no clean-exit log line"
kill -0 "$XVFB_PID" 2>/dev/null \
  && ok "X server survived the session" || bad "X server died during session"

# stragglers? (components AND the session process itself)
sleep 0.3
STRAGGLERS=$(pgrep -f "$(vb vantage-wm)|$(vb vantage-panel)|$(vb vantage-desktop)|$(vb vantage-session)" 2>/dev/null | grep -v "^$" | wc -l)
if [ "${STRAGGLERS:-0}" -eq 0 ]; then
  ok "no component stragglers left"
else
  bad "$STRAGGLERS component process(es) survived shutdown"
  pgrep -af "$(vb vantage-wm)|$(vb vantage-panel)|$(vb vantage-desktop)|$(vb vantage-session)" 2>/dev/null | head -5
fi

kill -TERM "$XVFB_PID" 2>/dev/null
wait "$XVFB_PID" 2>/dev/null

# ------------------------------------------------------------- summary
echo
echo "=========================================="
echo "harness-xvfb: $PASS passed, $FAIL failed"
echo "artifacts: $WORK"
echo "=========================================="
[ "$FAIL" -eq 0 ] || { echo "---- session.log ----"; cat "$WORK/session.log"; \
                        echo "---- failed checks ----"; cat "$FAILS"; }
[ "$FAIL" -eq 0 ]
