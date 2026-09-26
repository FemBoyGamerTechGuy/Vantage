#!/bin/bash
# harness-wayland.sh — Vantage Wayland-compositor integration harness
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boots vantage-wm --wayland as a native Wayland compositor
# (libwayland-server, wl_compositor + wl_seat + wl_output +
# xdg_wm_base) and runs a REAL xdg-shell client against it, then boots
# the full vantage-session --wayland and repeats the client check:
#
#   vantage-wm --wayland      (headless compositor, socket wayland-N)
#      ↑ vt-wayland-testclient (xdg_toplevel + wl_shm buffer, color)
#
#   vantage-session --wayland (WM supervised by the session manager)
#      ↑ vt-wayland-testclient
#
# Verifications:
#   1. compositor started and created its socket
#   2. xdg-shell client connects, gets configured, commits a buffer
#   3. SIGUSR1 frame-dump contains the client's committed color
#   4. clean shutdown on SIGTERM
#
# Usage: harness-wayland.sh [build-dir]    (default: $VT_BUILD_DIR or PATH)

set -u
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

BUILD="${1:-${VT_BUILD_DIR:-}}"
if [ -n "$BUILD" ] && [ -x "$BUILD/src/tools/vantage-wm" ]; then
  BIN="$BUILD/src/tools"
  TST="$BUILD/tests"
else
  BIN=""; TST=""
  for b in vantage-wm; do
    command -v "$b" >/dev/null 2>&1 || { echo "missing $b in PATH"; exit 77; }
  done
fi
vb() { if [ -n "$BIN" ]; then echo "$BIN/$1"; else echo "$1"; fi; }
tc() { if [ -n "$TST" ]; then echo "$TST/$1"; else echo "$1"; fi; }
[ -x "$(tc vt-wayland-testclient)" ] || { echo "vt-wayland-testclient not built"; exit 77; }

WORK=$(mktemp -d /tmp/vantage-wl.XXXXXX)
mkdir -p "$WORK/run"
export XDG_RUNTIME_DIR="$WORK/run"
chmod 700 "$XDG_RUNTIME_DIR"

# The Wayland backend refuses to nest: make sure neither variable is set
unset DISPLAY WAYLAND_DISPLAY
# Determinism + host safety: never acquire the host's real seat/VT/GPU
# from a test. The backend then reports seat/vt/drm as "skipped —
# forced headless", so the stage-marker trace below is IDENTICAL on a
# container, a desktop shell and a real-GPU machine (a real /dev/dri
# would otherwise log "drm: FAILED — DRM master refused", which is the
# honest outcome on a desktop but a nondeterministic test). The real
# KMS path is exercised by a genuine TTY launch (docs/wayland-backend.md).
export VANTAGE_WAYLAND_FORCE_HEADLESS=1
rm -f /tmp/vantage-wayland.ppm

# ------------------------------------------------------------- compositor
echo "== harness-wayland: starting vantage-wm --wayland (headless compositor) =="
"$(vb vantage-wm)" --wayland > "$WORK/wm.log" 2>&1 &
WM_PID=$!

SOCKET=""
for i in $(seq 1 100); do
  SOCK_FILE=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$WORK/wm.log" 2>/dev/null | head -1 | cut -d= -f2)
  if [ -n "$SOCK_FILE" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK_FILE" ]; then
    SOCKET="$SOCK_FILE"; break
  fi
  kill -0 "$WM_PID" 2>/dev/null || break
  sleep 0.1
done
if [ -n "$SOCKET" ]; then
  ok "compositor socket created ($SOCKET)"
else
  bad "compositor socket never appeared"; tail -20 "$WORK/wm.log"
  kill -TERM "$WM_PID" 2>/dev/null; exit 1
fi
export WAYLAND_DISPLAY="$SOCKET"

# ----------------------------------------------------- stage markers
# VANTAGE_WAYLAND_FORCE_HEADLESS=1 (exported above) makes the whole
# 15-stage trace deterministic on every machine: every hardware stage
# is skipped-with-reason, so each marker is asserted EXACTLY. This is
# what keeps `meson test` green on developer boxes with a real GPU —
# the harness must not depend on the machine it runs on, and it must
# never touch the host's GPU, VT or session manager.
echo "== harness-wayland: startup stage markers (forced-headless contract) =="
for st in 'session: ok' 'seat: skipped' 'vt: skipped' 'drm: skipped' \
          'drm-master: skipped' 'gbm: skipped' 'egl: skipped' \
          'renderer: skipped' 'outputs: skipped' 'crtc: skipped' \
          'scanout: skipped' 'input: skipped' 'socket: ok' \
          'compositor: READY' 'desktop: ready'; do
  if grep -qF "[wayland] $st" "$WORK/wm.log"; then
    ok "stage marker: $st"
  else
    bad "missing stage marker: [wayland] $st"
  fi
done
if grep -qF '[wayland] NOTICE: HEADLESS mode' "$WORK/wm.log"; then
  ok "honest HEADLESS fallback notice present"
else
  bad "no HEADLESS notice — the fallback would be silent (lied about KMS)"
fi

# ------------------------------------------------------------- client
echo "== harness-wayland: xdg-shell client =="
CLIENT_LOG="$WORK/client.log"
rm -f "$CLIENT_LOG"
timeout 15 "$(tc vt-wayland-testclient)" 0xff5a9a3a 300 200 > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

# wait for the buffer commit (client stays alive 2s after committing)
COMMITTED=""
for i in $(seq 1 100); do
  grep -q "^committed" "$CLIENT_LOG" 2>/dev/null && { COMMITTED=1; break; }
  kill -0 "$CLIENT_PID" 2>/dev/null || break
  sleep 0.05
done
sleep 0.4     # allow a few compositor paint cycles

grep -q "^connected" "$CLIENT_LOG" && ok "client connected to $SOCKET" \
  || bad "client failed to connect: $(cat "$CLIENT_LOG")"
grep -q "^configured" "$CLIENT_LOG" && ok "xdg_surface configure received" \
  || bad "no xdg configure"
grep -q "^committed" "$CLIENT_LOG" && ok "wl_shm buffer committed" \
  || bad "no buffer commit"

# ------------------------------------------------- window-list mirror
# The xdg window must appear in the WM model (vantage-remote list works
# on Wayland exactly like on X11).
echo "== harness-wayland: WM window-list mirror (vantage-remote list) =="
sleep 0.3   # let the map event reach the sink
if "$(vb vantage-remote)" list > "$WORK/wl-list.txt" 2>"$WORK/wl-list.err"; then
  if grep -q "Vantage Wayland Test" "$WORK/wl-list.txt"; then
    ok "wayland window mirrored into the WM model"
    if grep -q "vantage.wltest" "$WORK/wl-list.txt"; then
      ok "app_id propagated to the window list"
    else
      bad "app_id missing from the window list"
    fi
  else
    bad "window list does not contain the wayland window:
$(cat "$WORK/wl-list.txt")"
  fi
else
  bad "vantage-remote list failed on the wayland WM:
$(cat "$WORK/wl-list.err")"
fi

# ------------------------------------------------------------- pixels
echo "== harness-wayland: compositor frame-dump check =="
# frame dump MUST happen while the client surface is still alive
kill -USR1 "$WM_PID" 2>/dev/null
for i in $(seq 1 20); do
  [ -s /tmp/vantage-wayland.ppm ] && break
  sleep 0.05
done
wait "$CLIENT_PID" 2>/dev/null
if [ -s /tmp/vantage-wayland.ppm ]; then
  ok "SIGUSR1 frame dump written"
  python3 - <<'PYEOF'
import sys
p = '/tmp/vantage-wayland.ppm'
with open(p, 'rb') as f:
    data = f.read()
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
target = bytes((0x5a, 0x9a, 0x3a))    # client color 0x5a9a3a (ARGB 0xff5a9a3a)
hits = pix.count(target)
print(f"frame: {w}x{h}, client-color pixels={hits}")
sys.exit(0 if hits > 1000 else 1)
PYEOF
  [ $? -eq 0 ] && ok "client pixels present in compositor framebuffer" \
    || bad "client color not found in frame dump"
else
  bad "no frame dump at /tmp/vantage-wayland.ppm"
fi

# ------------------------------------------------------------- shutdown
# Ctrl+C (SIGINT) takes the same clean-unwind path as SIGTERM: restore,
# unwind, exit 0. Assert the exit STATUS and the cleanup logs.
echo "== harness-wayland: clean shutdown (SIGINT == Ctrl+C) =="
kill -INT "$WM_PID" 2>/dev/null
EXITED=""
for i in $(seq 1 50); do
  kill -0 "$WM_PID" 2>/dev/null || { EXITED=1; break; }
  sleep 0.1
done
[ -n "$EXITED" ] && ok "compositor exited on SIGINT (Ctrl+C)" \
  || bad "WM ignored SIGINT"
WM_RC=0
wait "$WM_PID" 2>/dev/null || WM_RC=$?
[ "$WM_RC" -eq 0 ] && ok "Ctrl+C exit status is 0 (clean unwind)" \
  || bad "Ctrl+C exit status $WM_RC (expected 0)"
grep -q "wm: shutting down" "$WORK/wm.log" \
  && ok "WM logged clean shutdown" || bad "no clean-shutdown log line"
grep -qF '[wayland] compositor: exited cleanly' "$WORK/wm.log" \
  && ok "compositor logged full unwind" \
  || bad "no compositor-exited-cleanly marker"

# ===================================================================
# Full session: vantage-session --wayland (session manager + WM)
# ===================================================================
echo "== harness-wayland: starting vantage-session --wayland =="
SESS_LOG="$WORK/session.log"
rm -f "$SESS_LOG" /tmp/vantage-wayland.ppm
# the compositor session must not see the earlier export — it creates
# its own socket (nesting is refused by design)
unset WAYLAND_DISPLAY
"$(vb vantage-session)" --wayland > "$SESS_LOG" 2>&1 &
SESS_PID=$!

SESS_READY=""
SOCK2=""
for i in $(seq 1 100); do
  grep -q "session: ready" "$SESS_LOG" 2>/dev/null && SESS_READY=1
  SOCK2=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$SESS_LOG" 2>/dev/null | head -1 | cut -d= -f2)
  if [ -n "$SESS_READY" ] && [ -n "$SOCK2" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK2" ]; then
    break
  fi
  kill -0 "$SESS_PID" 2>/dev/null || break
  sleep 0.1
done
[ -n "$SESS_READY" ] && ok "vantage-session --wayland reached ready state" \
  || bad "session never became ready"
[ -n "$SOCK2" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK2" ] \
  && ok "session's WM created compositor socket ($SOCK2)" \
  || bad "no compositor socket from the session"
grep -q "display backend: Wayland" "$SESS_LOG" \
  && ok "session reported the Wayland backend" \
  || bad "session did not report the Wayland backend"

if [ -n "$SOCK2" ] && [ -S "$XDG_RUNTIME_DIR/$SOCK2" ]; then
  CLIENT_LOG2="$WORK/client-session.log"
  timeout 10 env WAYLAND_DISPLAY="$SOCK2" \
    "$(tc vt-wayland-testclient)" 0xff3a9a5f 260 180 > "$CLIENT_LOG2" 2>&1 &
  C2=$!
  COMMITTED2=""
  for i in $(seq 1 100); do
    grep -q "^committed" "$CLIENT_LOG2" 2>/dev/null && { COMMITTED2=1; break; }
    kill -0 "$C2" 2>/dev/null || break
    sleep 0.05
  done
  [ -n "$COMMITTED2" ] && ok "xdg client committed under the full session" \
    || bad "client never committed under the session"

  # frame-dump check: SIGUSR1 goes to the WM child (it runs in its own
  # session after setsid, so signal it directly via its pid from the log)
  WM_CHILD=$(grep -o "started 'wm' pid=[0-9]*" "$SESS_LOG" | head -1 | cut -d= -f2)
  if [ -n "${WM_CHILD:-}" ] && kill -0 "$WM_CHILD" 2>/dev/null; then
    kill -USR1 "$WM_CHILD" 2>/dev/null
    for i in $(seq 1 20); do
      [ -s /tmp/vantage-wayland.ppm ] && break
      sleep 0.05
    done
    [ -s /tmp/vantage-wayland.ppm ] \
      && ok "frame dump written under the full session" \
      || bad "no frame dump under the session"
  fi
  # NOTE: do NOT terminate the session here — the logout round-trip
  # below must be the thing that stops it.
fi

# ------------------------------------------------- logout round-trip
# vantage-remote logout → session IPC → graceful child shutdown
# (SIGTERM + grace, no SIGKILL) → exit 0.
echo "== harness-wayland: vantage-remote logout round-trip =="
if [ -n "${SESS_PID:-}" ] && kill -0 "$SESS_PID" 2>/dev/null; then
  LOGOUT_RC=0
  "$(vb vantage-remote)" logout > "$WORK/logout.txt" 2>&1 || LOGOUT_RC=$?
  [ "$LOGOUT_RC" -eq 0 ] && ok "vantage-remote logout accepted" \
    || bad "vantage-remote logout failed ($(cat "$WORK/logout.txt"))"
  for i in $(seq 1 60); do
    kill -0 "$SESS_PID" 2>/dev/null || break
    sleep 0.1
  done
fi
SESS_EXITED=""
for i in $(seq 1 60); do
  kill -0 "$SESS_PID" 2>/dev/null || { SESS_EXITED=1; break; }
  sleep 0.1
done
[ -n "$SESS_EXITED" ] && ok "session exited after logout" \
  || bad "session ignored logout"
SESS_RC=0
wait "$SESS_PID" 2>/dev/null || SESS_RC=$?
[ "$SESS_RC" -eq 0 ] && ok "session logout exit status is 0 (no SIGKILL)" \
  || bad "session exit status $SESS_RC (SIGKILLed?)"
grep -q "policy: SIGTERM" "$SESS_LOG" \
  && ok "session logged the graceful shutdown policy" \
  || bad "no shutdown-policy log line"
grep -q "session: exited" "$SESS_LOG" \
  && ok "session logged clean exit" || bad "no clean session-exit log line"
sleep 0.3
STRAGGLERS=$(pgrep -f "$(vb vantage-wm)|$(vb vantage-session)" 2>/dev/null | wc -l)
if [ "${STRAGGLERS:-0}" -eq 0 ]; then
  ok "no session/WM stragglers left"
else
  bad "$STRAGGLERS process(es) survived session shutdown"
  pgrep -af "$(vb vantage-wm)|$(vb vantage-session)" 2>/dev/null | head -5
fi

rm -f /tmp/vantage-wayland.ppm
echo
echo "============================================"
echo "harness-wayland: $PASS passed, $FAIL failed"
echo "artifacts: $WORK"
echo "============================================"
[ "$FAIL" -eq 0 ] || { echo "---- wm.log ----"; cat "$WORK/wm.log"; \
                        echo "---- session.log ----"; cat "$SESS_LOG"; }
[ "$FAIL" -eq 0 ]
