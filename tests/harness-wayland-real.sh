#!/bin/bash
# harness-wayland-real.sh — REAL Wayland session self-test (DRM/VT/GPU tier)
#
# SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
#
# Tier 2 of the Wayland test pyramid (docs/wayland-backend.md):
#   1. headless protocol testing       — harness-wayland.sh (meson test)
#   2. REAL DRM/VT session testing     — THIS harness
#   3. GPU/NVIDIA reporting            — vantage-diagnostics --gpu
#
# This harness runs the REAL compositor session path: no
# VANTAGE_WAYLAND_FORCE_HEADLESS, VANTAGE_WAYLAND_REQUIRE_KMS=1 — the
# headless fallback is FORBIDDEN, so a machine without DRM/VT access
# cannot fake a pass.
#
# Preconditions (all must hold, otherwise the test SKIPS — never lies):
#   * /dev/dri exists (real KMS hardware)
#   * neither DISPLAY nor WAYLAND_DISPLAY is set (no live session to break)
#   * stdin is a real TTY (run from a console) OR VANTAGE_WAYLAND_SELFTEST=1
#     is set explicitly (operator accepts the screen takeover)
#
# What it verifies when it runs:
#   seat acquired, VT activated, DRM master taken, CRTC mode-set, scanout
#   chain live, input devices open, xdg-shell client + real pixels + the
#   compositor panel, clean SIGINT unwind (CRTC restore, VT text, exit 0).
#
# Usage: harness-wayland-real.sh [build-dir]

set -u
PASS=0; FAIL=0; SKIP=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
skip(){ echo "  SKIP: $1"; SKIP=$((SKIP+1)); }

BUILD="${1:-${VT_BUILD_DIR:-}}"
if [ -n "$BUILD" ] && [ -x "$BUILD/src/tools/vantage-wm" ]; then
  BIN="$BUILD/src/tools"
  TST="$BUILD/tests"
else
  BIN=""; TST=""
  for b in vantage-wm vantage-diagnostics; do
    command -v "$b" >/dev/null 2>&1 || { echo "missing $b in PATH"; exit 77; }
  done
fi
vb() { if [ -n "$BIN" ]; then echo "$BIN/$1"; else echo "$1"; fi; }
tc() { if [ -n "$TST" ]; then echo "$TST/$1"; else echo "$1"; fi; }
[ -x "$(tc vt-wayland-testclient)" ] || { echo "vt-wayland-testclient not built"; exit 77; }

echo "== harness-wayland-real: environment preconditions =="

# --- precondition 1: real DRM hardware --------------------------------
if [ ! -d /dev/dri ]; then
    skip "no /dev/dri on this machine (container/CI) — the real-KMS tier"
    skip "cannot run here by design; run it on the target hardware from a TTY"
    echo
    echo "harness-wayland-real: skipped (no DRM hardware)"
    exit 77
fi
ok "/dev/dri present ($(ls /dev/dri 2>/dev/null | tr '\n' ' '))"

# --- precondition 2: no live graphical session ------------------------
if [ -n "${DISPLAY:-}" ]; then
    skip "DISPLAY='$DISPLAY' is set — running the real tier from inside a"
    skip "desktop would steal DRM master from the live server; switch to a"
    skip "TTY (Ctrl+Alt+F3) and re-run"
    exit 77
fi
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    skip "WAYLAND_DISPLAY='$WAYLAND_DISPLAY' is set — compositor refuses to nest"
    exit 77
fi
ok "no live DISPLAY / WAYLAND_DISPLAY"

# --- precondition 3: an operator-authorized console -------------------
if [ ! -t 0 ] && [ "${VANTAGE_WAYLAND_SELFTEST:-}" != "1" ]; then
    skip "stdin is not a TTY and VANTAGE_WAYLAND_SELFTEST=1 is not set;"
    skip "the real tier takes over the screen — re-run from a console, or"
    skip "set VANTAGE_WAYLAND_SELFTEST=1 to accept the takeover"
    exit 77
fi
ok "console takeover authorized $([ -t 0 ] && echo '(interactive TTY)' || echo '(env flag)')"

# --- GPU / NVIDIA reporting (informational, tier 3) --------------------
echo "== harness-wayland-real: GPU report (honest, informational) =="
"$(vb vantage-diagnostics)" --gpu 2>/dev/null | head -12 || true

# --- the real session run ------------------------------------------------
WORK=$(mktemp -d /tmp/vantage-wl-real.XXXXXX)
mkdir -p "$WORK/run"
export XDG_RUNTIME_DIR="$WORK/run"
chmod 700 "$XDG_RUNTIME_DIR"
rm -f /tmp/vantage-wayland.ppm

echo "== harness-wayland-real: REAL session (REQUIRE_KMS=1, no headless) =="
export VANTAGE_WAYLAND_REQUIRE_KMS=1
unset VANTAGE_WAYLAND_FORCE_HEADLESS
"$(vb vantage-wm)" --wayland > "$WORK/wm.log" 2>&1 &
WM_PID=$!

# wait for READY or death
READY=""
for i in $(seq 1 200); do
    grep -q "\[wayland\] compositor: READY" "$WORK/wm.log" 2>/dev/null && { READY=1; break; }
    kill -0 "$WM_PID" 2>/dev/null || break
    sleep 0.1
done
if [ -z "$READY" ]; then
    bad "compositor did not reach READY — last stage markers:"
    grep -E "\[wayland\]" "$WORK/wm.log" | tail -8
    kill -TERM "$WM_PID" 2>/dev/null
    wait "$WM_PID" 2>/dev/null
    echo
    echo "harness-wayland-real: $PASS passed, $FAIL failed"
    exit 1
fi
ok "REAL compositor READY (seat/VT/DRM/master/CRTC/scanout path taken)"

# --- stage markers: the real path must SUCCEED (not skip) ---------------
for st in 'seat: ok' 'vt: ok' 'drm: ok' 'drm-master: ok' \
          'crtc: ok' 'scanout: ok' 'socket: ok' 'compositor: READY' \
          'desktop: ready'; do
    if grep -qF "[wayland] $st" "$WORK/wm.log"; then
        ok "real stage: $st"
    else
        bad "real stage missing/failed: [wayland] $st"
        grep -F "[wayland] $st" "$WORK/wm.log" | tail -1
    fi
done
if grep -qF "[wayland] input: ok" "$WORK/wm.log"; then
    ok "real input devices open (libinput through the seat)"
    DEVN=$(grep -oE "input device '[^']*'" "$WORK/wm.log" | wc -l)
    echo "        ($DEVN input device(s) reported)"
elif grep -qF "NO usable input devices" "$WORK/wm.log"; then
    bad "input: NO usable devices — add the user to the 'input' group or"
    bad "run under elogind/seatd (see the [wayland] input log above)"
else
    skip "input stage neither ok nor dead — inspect $(grep '\[wayland\] input' "$WORK/wm.log" | tail -1)"
fi

# honest renderer line
grep -E "\[wayland\] renderer: ok" "$WORK/wm.log" | head -1 | grep -qiE "llvmpipe|softpipe|swrast|Software" \
    && skip "renderer is SOFTWARE (llvmpipe-class) — GPU acceleration NOT active" \
    || ok "renderer reports hardware acceleration (see the log line)"

# --- socket + xdg client --------------------------------------------------
SOCKET=$(grep -o 'WAYLAND_DISPLAY=[a-z0-9-]*' "$WORK/wm.log" | head -1 | cut -d= -f2)
CLIENT_LOG="$WORK/client.log"
if [ -n "$SOCKET" ] && [ -S "$XDG_RUNTIME_DIR/$SOCKET" ]; then
    ok "socket live ($SOCKET)"
    timeout 15 env WAYLAND_DISPLAY="$SOCKET" "$(tc vt-wayland-testclient)" \
        0xff5a9a3a 300 200 > "$CLIENT_LOG" 2>&1 &
    C_PID=$!
    for i in $(seq 1 100); do
        grep -q "^committed" "$CLIENT_LOG" 2>/dev/null && break
        kill -0 "$C_PID" 2>/dev/null || break
        sleep 0.05
    done
    grep -q "^committed" "$CLIENT_LOG" && ok "xdg client committed on the REAL output" \
        || bad "xdg client never committed: $(cat "$CLIENT_LOG")"
    wait "$C_PID" 2>/dev/null
else
    bad "no live socket found ($SOCKET)"
fi

# --- pixels: the panel + client color on the REAL framebuffer ------------
kill -USR1 "$WM_PID" 2>/dev/null
for i in $(seq 1 20); do [ -s /tmp/vantage-wayland.ppm ] && break; sleep 0.05; done
if [ -s /tmp/vantage-wayland.ppm ]; then
    ok "frame dump written from the REAL scanout path"
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
hits = pix.count(bytes((0x5a, 0x9a, 0x3a)))
top = pix[:w*32*3]
panel_bg = top.count(bytes((0x23, 0x26, 0x2b)))
print(f"frame: {w}x{h}, client-color={hits}, panel-bg={panel_bg}")
sys.exit(0 if (hits > 1000 and panel_bg > w * 8) else 1)
PYEOF
    [ $? -eq 0 ] && ok "real pixels: client window + compositor panel present" \
        || bad "frame dump lacks client pixels/panel"
else
    bad "no frame dump"
fi
rm -f /tmp/vantage-wayland.ppm

# --- clean shutdown (Ctrl+C path) ------------------------------------------
echo "== harness-wayland-real: clean shutdown (SIGINT) =="
kill -INT "$WM_PID" 2>/dev/null
EXITED=""
for i in $(seq 1 60); do
    kill -0 "$WM_PID" 2>/dev/null || { EXITED=1; break; }
    sleep 0.1
done
[ -n "$EXITED" ] && ok "compositor exited on SIGINT" || bad "compositor ignored SIGINT"
RC=0
wait "$WM_PID" 2>/dev/null || RC=$?
[ "$RC" -eq 0 ] && ok "exit status 0 (clean unwind — no reboot needed)" \
    || bad "exit status $RC"
grep -qF '[wayland] compositor: exited cleanly' "$WORK/wm.log" \
    && ok "full unwind logged" || bad "no exited-cleanly marker"
grep -q "KMS closed — original CRTC restored" "$WORK/wm.log" \
    && ok "original CRTC restored" || bad "CRTC not restored"
grep -q "seat released — VT returned to text mode" "$WORK/wm.log" \
    && ok "VT returned to text mode" || bad "VT not returned to text"

echo
echo "============================================"
echo "harness-wayland-real: $PASS passed, $FAIL failed, $SKIP skipped"
echo "artifacts: $WORK"
echo "============================================"
[ "$FAIL" -eq 0 ]
