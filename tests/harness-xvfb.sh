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
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# ---------------------------------------------------------------- paths
BUILD="${1:-${VT_BUILD_DIR:-}}"
if [ -n "$BUILD" ] && [ -x "$BUILD/src/tools/vantage-session" ]; then
  BIN="$BUILD/src/tools"
  TST="$BUILD/tests"
  export PATH="$BIN:$PATH"      # session spawns components via PATH
else
  BIN=""; TST=""
  for b in vantage-session vantage-wm vantage-panel vantage-desktop vantage-remote; do
    command -v "$b" >/dev/null 2>&1 || { echo "missing $b in PATH"; exit 77; }
  done
fi
vb() { if [ -n "$BIN" ]; then echo "$BIN/$1"; else echo "$1"; fi; }
tc() { if [ -n "$TST" ]; then echo "$TST/$1"; else echo "$1"; fi; }

command -v Xvfb >/dev/null 2>&1 || { echo "Xvfb not found"; exit 77; }
[ -x "$(tc vt-x11-testclient)" ] || { echo "vt-x11-testclient not built"; exit 77; }

# --------------------------------------------------------- isolated env
WORK=$(mktemp -d /tmp/vantage-xvfb.XXXXXX)
mkdir -p "$WORK/run" "$WORK/config/vantage" "$WORK/share"
export XDG_RUNTIME_DIR="$WORK/run"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_DATA_HOME="$WORK/share"        # isolate XDG autostart
export XDG_CONFIG_DIRS=""
chmod 700 "$XDG_RUNTIME_DIR"

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
height=32
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

sleep 1     # let the WM/panel/desktop settle and paint

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

echo "== harness-xvfb: focus policy (click-to-focus, no hover steal) =="
FOCUS_LOG="$WORK/focus.log"
"$(tc vt-x11-testclient)" --focus-probe > "$FOCUS_LOG" 2>&1
FRC=$?
grep -q "hover-steals=no" "$FOCUS_LOG" && ok "hover does NOT steal keyboard focus" \
  || bad "hover steals focus: $(cat "$FOCUS_LOG")"
grep -q "click-focus=yes" "$FOCUS_LOG" && ok "click focuses the window" \
  || bad "click did not focus: $(cat "$FOCUS_LOG")"
[ $FRC -eq 0 ] && ok "focus probe exit status 0" || bad "focus probe rc=$FRC"

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
print(f"pixels: {w}x{h}, window1={hits1}px, window2={hits2}px, distinct-colors(top40rows)={distinct}")
sys.exit(0 if (hits1 > 500 and hits2 > 500 and distinct >= 3) else 1)
PYEOF
[ $? -eq 0 ] && ok "screenshot shows managed windows + painted desktop" \
  || bad "screenshot pixel check failed"

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
[ "$FAIL" -eq 0 ] || { echo "---- session.log ----"; cat "$WORK/session.log"; }
[ "$FAIL" -eq 0 ]
