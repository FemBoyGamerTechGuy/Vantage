#!/bin/bash
# harness-wayland.sh — Vantage Wayland-compositor integration harness
#
# SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
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
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); printf '  FAIL: %s\n' "$1" >> "$FAILS"; }

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

# Wait for a COMPLETE frame dump: SIGUSR1 writes ~2.3MB; polling for
# "non-empty" races the writer. Wait until the size is stable.
wait_ppm() {
  local last=-1 cur=0
  for i in $(seq 1 40); do
    [ -s /tmp/vantage-wayland.ppm ] || { sleep 0.05; continue; }
    cur=$(stat -c %s /tmp/vantage-wayland.ppm 2>/dev/null || echo 0)
    [ "$cur" = "$last" ] && [ "$cur" -gt 100 ] && return 0
    last=$cur
    sleep 0.05
  done
  return 1
}

WORK=$(mktemp -d /tmp/vantage-wl.XXXXXX)
mkdir -p "$WORK/run"
# failures are recorded so the TAIL of a truncated meson log (which
# only keeps the last 100 lines — after the wm/session log dumps)
# still shows exactly WHICH checks failed
FAILS="$WORK/fails.txt"; : > "$FAILS"
export XDG_RUNTIME_DIR="$WORK/run"
chmod 700 "$XDG_RUNTIME_DIR"

# The Wayland backend refuses to nest: make sure neither variable is set
unset DISPLAY WAYLAND_DISPLAY
# Deterministic cursor: force the built-in 16x16 arrow so the sprite
# pixel assertions below are exact on every machine (theme-independent)
export VANTAGE_WL_CURSOR=builtin
# deterministic application database for the launcher checks
mkdir -p "$WORK/data/applications"
# the launch probe lands in Accessories (the FIRST visible category in
# the fixed table) and sorts alone there, so "app row 0" is exactly
# this application on every machine — no dependence on what the host
# has installed
cat > "$WORK/data/applications/vt-harness-probe.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Zz Harness Probe
Exec=/bin/sh -c 'echo launched > $WORK/wl-launch-marker'
Icon=vt-harness-probe
Categories=Utility;
DESK
sed -i "s|\$WORK|$WORK|g" "$WORK/data/applications/vt-harness-probe.desktop"
# deterministic ICON: a private icon theme in the isolated XDG tree with
# a solid-color PNG — proves the panel's icon-theme lookup + PNG decode
# + ARGB scale + row rendering end to end (this code path was silently
# compiled out when the backends target missed the VT_HAVE_PNG defines)
ICON_DIR="$WORK/data/icons/vt-harness-theme/24x24/apps"
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
cat > "$WORK/data/icons/vt-harness-theme/index.theme" <<'EOF'
[Icon Theme]
Name=vt-harness-theme
Directories=24x24/apps

[24x24/apps]
Size=24
Context=Applications
Type=Fixed
EOF
export VANTAGE_ICON_THEME=vt-harness-theme
cat > "$WORK/data/applications/vt-harness-term.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Zz Harness Terminal
Exec=/bin/true
Terminal=true
Categories=System;
DESK
export XDG_DATA_HOME="$WORK/data"
# XDG_DATA_DIRS must be ISOLATED as well: when unset, vt-apps falls
# back to /usr/local/share:/usr/share, so host-installed applications
# (LibreOffice, xfce4-screenshooter, …) leak into the Programs menu and
# "app row 0" depends on the machine — the launch click below then hits
# a REAL app instead of the probe (observed on a real-hardware box:
# the click launched the host's screenshot tool and the marker never
# appeared). An empty XDG_DATA_DIRS dir keeps the DB deterministic:
# the probe is the only Utility entry on every machine.
mkdir -p "$WORK/data-dirs"
export XDG_DATA_DIRS="$WORK/data-dirs"
# Same isolation for the full-session phase below: the session manager
# reads XDG autostart from XDG_CONFIG_HOME (user) + resource dir +
# /etc/xdg/autostart (host). A controlled probe entry keeps REAL
# coverage of the autostart path, while VANTAGE_SESSION_NO_SYSTEM_AUTOSTART
# guarantees the test can never spawn host daemons (the machine's real
# PipeWire/wireplumber fought the harness on the reporting machine) nor
# read the host's real vantage.conf / gtk settings.
export XDG_CONFIG_HOME="$WORK/config"
# XDG autostart spec: user entries live in $XDG_CONFIG_HOME/autostart
# (no application-name subdir — that is only for vantage.conf)
mkdir -p "$XDG_CONFIG_HOME/autostart"
cat > "$XDG_CONFIG_HOME/autostart/vt-harness-autostart.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Vantage Harness Autostart
Exec=/bin/sh -c 'echo autostarted > $WORK/wl-autostart-marker'
DESK
sed -i "s|\$WORK|$WORK|g" "$XDG_CONFIG_HOME/autostart/vt-harness-autostart.desktop"
export VANTAGE_SESSION_NO_SYSTEM_AUTOSTART=1
rm -f "$WORK/wl-launch-marker" "$WORK/wl-autostart-marker"

# (XDG_DATA_HOME must be exported BEFORE the compositor starts: the
# process environment is fixed at exec time, and the panel reads it
# when it lazily loads the .desktop database at first menu open.)
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

# the desktop stage (panel + first frame) completes AFTER the socket
# appears — wait for its marker (bounded) before asserting the trace
for i in $(seq 1 50); do
  grep -qF '[wayland] desktop: ready' "$WORK/wm.log" 2>/dev/null && break
  kill -0 "$WM_PID" 2>/dev/null || break
  sleep 0.1
done

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
# wl_keyboard keymap: the test client binds wl_seat + wl_keyboard like
# a REAL toolkit app and validates the keymap event (fd >= 0, size > 0,
# mmap-able xkb text). A compositor that passes fd -1 here kills the
# client connection with a libwayland marshal error — every launched
# app would die on connect (the exact real-hardware failure).
grep -q "^keymap ok" "$CLIENT_LOG" \
  && ok "wl_keyboard keymap received and valid (fd/size/mmap/xkb)" \
  || bad "wl_keyboard.keymap missing or invalid — real apps would die on connect"
grep -q "^configured" "$CLIENT_LOG" && ok "xdg_surface configure received" \
  || bad "no xdg configure"
grep -q "^committed" "$CLIENT_LOG" && ok "wl_shm buffer committed" \
  || bad "no buffer commit"

# --- SEQUENTIAL keyboard clients: the full client above bound
# --- wl_keyboard first; these keymap-only clients connect one after
# --- another and every one of them must receive a VALID keymap. This
# --- is the real-hardware failure class (second+ app died on connect
# --- with "error marshalling arguments for keymap: dup failed").
KM_FAIL=0
for k in 1 2 3; do
  KLOG="$WORK/keymap-$k.log"
  timeout 8 "$(tc vt-wayland-testclient)" --keymap-only > "$KLOG" 2>&1
  KRC=$?
  if [ $KRC -eq 0 ] && grep -q "^keymap ok" "$KLOG"; then :; else
    KM_FAIL=$((KM_FAIL+1))
  fi
done
[ "$KM_FAIL" -eq 0 ] \
  && ok "3 sequential keyboard clients each got a valid keymap" \
  || bad "keyboard client #$KM_FAIL did not receive a valid keymap"
# the compositor log must be free of libwayland marshal/connection
# errors — a keymap fd bug shows up here verbatim
if grep -q "error marshalling arguments\|error in client communication" \
     "$WORK/wm.log"; then
  bad "libwayland marshal/client errors in the compositor log"
else
  ok "no libwayland marshal/client errors in the log"
fi

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
wait_ppm
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
# panel assertions: top bar is the panel background; the REAL panel is
# drawn (accent start button present, old placeholder squares GONE)
top = pix[:w*32*3]
panel_bg = top.count(bytes((0x23, 0x26, 0x2b)))
accent = pix.count(bytes((0x4f, 0x9a, 0xdc)))
placeholder_red = pix.count(bytes((0xe0, 0x5a, 0x5a)))
placeholder_green = pix.count(bytes((0x7a, 0xc8, 0x60)))
# BACKGROUND: the desktop now renders the [wallpaper] config — the
# default vertical navy gradient. The old bug painted a flat hardcoded
# gray everywhere. Sample far from windows/panel: left edge, below the
# panel (y=40) and near the bottom (y=h-8): gradient colors differ and
# match the engine's interpolation between (18,23,36) and (38,48,79).
def px(x, y):
    i = (y*w+x)*3
    return (pix[i], pix[i+1], pix[i+2])
c_top = px(4, 40)
c_bot = px(4, h - 8)
def near(c, t, tol=8):
    return all(abs(a-b) <= tol for a, b in zip(c, t))
grad_top_ok = near(c_top, (18, 23, 36))
grad_bot_ok = near(c_bot, (38, 48, 79))
grad_diff = c_bot[2] - c_top[2] >= 12
print(f"frame: {w}x{h}, client-color pixels={hits}, panel-bg(top)={panel_bg}, "
      f"accent={accent}, placeholder-red={placeholder_red}, "
      f"placeholder-green={placeholder_green}")
print(f"background: top={c_top} bottom={c_bot} "
      f"(gradient {'OK' if grad_top_ok and grad_bot_ok and grad_diff else 'BAD'})")
panel_ok = panel_bg > w * 8 and accent > 50
placeholders_gone = placeholder_red == 0 and placeholder_green == 0
sys.exit(0 if (hits > 1000 and panel_ok and placeholders_gone and
               grad_top_ok and grad_bot_ok and grad_diff) else 1)
PYEOF
  [ $? -eq 0 ] && ok "client pixels + REAL compositor panel in frame dump" \
    || bad "frame dump check failed (pixels/panel/placeholders/background)"
else
  bad "no frame dump at /tmp/vantage-wayland.ppm"
fi

# --- wl_buffer.release: the compositor must release client buffers,
# --- or double-buffered apps stall after two frames
if grep -q "^buffer released" "$CLIENT_LOG"; then
  ok "wl_buffer.release received by the client (no double-buffer stall)"
else
  bad "no wl_buffer.release — real apps would stall after 2 frames"
fi

# ------------------------------------------------ popup + multipool clients
# The two historical "apps just crash on Wayland" classes, as
# deterministic regression clients:
#   --popup      xdg_popup lifecycle (GTK menus): positioner size honored,
#                configure/ack flow, destroy does not kill the connection
#   --multipool alternating shm pools: the never-ended begin_access used
#                to abort the COMPOSITOR (libwayland assertion) — every
#                real app died with "Broken pipe"
echo "== harness-wayland: xdg_popup lifecycle (menu class) =="
POPUP_LOG="$WORK/popup.log"
timeout 12 "$(tc vt-wayland-testclient)" --popup > "$POPUP_LOG" 2>&1
POPUP_RC=$?
if [ $POPUP_RC -eq 0 ] && grep -q "^popup configured 180x160" "$POPUP_LOG" \
   && grep -q "^popup destroyed without dying" "$POPUP_LOG"; then
  ok "xdg popup: positioner size honored + destroy is survivable"
else
  bad "xdg popup lifecycle failed (rc=$POPUP_RC): $(tail -3 "$POPUP_LOG")"
fi
kill -0 "$WM_PID" 2>/dev/null \
  && ok "compositor alive after popup client" \
  || bad "compositor died on the popup client"

echo "== harness-wayland: multi-pool shm client (crash class) =="
MP_LOG="$WORK/multipool.log"
timeout 15 "$(tc vt-wayland-testclient)" --multipool > "$MP_LOG" 2>&1
MP_RC=$?
if [ $MP_RC -eq 0 ] && grep -q "^multipool ok, releases=" "$MP_LOG"; then
  ok "multi-pool shm commits + wl_buffer.release flow (compositor never aborted)"
else
  bad "multi-pool client failed (rc=$MP_RC): $(tail -3 "$MP_LOG")"
fi
kill -0 "$WM_PID" 2>/dev/null \
  && ok "compositor alive after multi-pool client" \
  || bad "compositor died on the multi-pool client"


# ------------------------------------------------- interactive UI checks
# The test-input IPC hook drives the REAL input pipeline (the same
# handlers libinput feeds), so the headless harness exercises the
# actual panel: cursor sprite, Programs menu, search bar, category
# selection, application LAUNCHING, and the volume indicator.
echo "== harness-wayland: interactive panel via real input path =="

ti() { "$(vb vantage-remote)" test-input "$@" >>"$WORK/ui.log" 2>&1; }

# --- cursor sprite: the built-in 16x16 arrow (forced above) has an
# --- EXACT bitmap — verify the pixels; the old stride bug read the
# --- 64-stride image as tight-packed and sheared it into diagonal dots
ti "motion x=300 y=200"
sleep 0.2
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm
if [ -s /tmp/vantage-wayland.ppm ]; then
  python3 - <<'PYEOF2'
import sys
with open('/tmp/vantage-wayland.ppm','rb') as f:
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
def px(x, y):
    i = (y*w+x)*3
    return (pix[i], pix[i+1], pix[i+2])
# built-in 16x16 arrow, hotspot (0,0): the bitmap is EXACTLY
# 73 black + 75 white pixels in the 16x16 box at the cursor position.
# The old stride bug (reading the 64-stride cell as tight-packed)
# shears those pixels into diagonal fragments with different counts.
white = 0
black = 0
far = 0
for dy in range(-4, 20):
    for dx in range(-4, 20):
        c = px(300+dx, 200+dy)
        if c == (255, 255, 255): white += 1
        elif c == (0, 0, 0): black += 1
# staircase debris would also land in the ring 20..64px away
for dy in range(-4, 64):
    for dx in range(20, 64):
        if px(300+dx, 200+dy) in ((255, 255, 255), (0, 0, 0)): far += 1
print(f"cursor: white={white} black={black} far-debris={far}")
ok = white == 75 and black == 73 and far == 0
sys.exit(0 if ok else 1)
PYEOF2
  [ $? -eq 0 ] && ok "cursor sprite is the exact built-in arrow (73 black + 75 white)"     || bad "cursor sprite corrupted (stride bug pattern?)"
else
  bad "no frame dump for the cursor check"
fi

# --- Programs menu: click the start button, verify the menu opens
# --- (search bar background is opaque → exact-matchable)
ti "motion x=63 y=17"
ti "press b=1"
ti "release b=1"
sleep 0.3
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm
if [ -s /tmp/vantage-wayland.ppm ]; then
  python3 - <<'PYEOF3'
import sys
with open('/tmp/vantage-wayland.ppm','rb') as f:
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
# search-field background 0x2a2e35 (opaque) in the menu header band
band = pix[(38*w)*3 : (72*w)*3]
search_bg = band.count(bytes((0x2a, 0x2e, 0x35)))
# text presence must be FONT-INDEPENDENT: the panel renders menu text
# with the system 'sans' font at 13px, antialiased. Counting EXACT
# text-color pixels (0xeceef0) worked with DejaVu but broke with
# thinner fonts (Carlito/Noto measured 47 exact px vs DejaVu's 101 —
# a real Arch box failed the check with a fully rendered menu).
# Light-pixel counting (> 0xa0 in R,G,B — brighter than every fill in
# the menu: bg 0x1a1c22, search 0x2a2e35, accent 0x4f9adc, warn
# 0xe07a50) measures "text is visibly rendered" for ANY font.
menu = pix[(38*w)*3 : (200*w)*3]
light = 0
for i in range(0, len(menu), 3):
    if menu[i] >= 0x90 and menu[i+1] >= 0x90 and menu[i+2] >= 0x90:
        light += 1
# the probe application's ICON (solid 0xc04080 PNG from the isolated
# icon theme, scaled to 18x18 in the app row) must actually be rendered
icon_px = pix.count(bytes((0xc0, 0x40, 0x80)))
print(f"menu: search-bg={search_bg} light-text={light} icon-px={icon_px}")
sys.exit(0 if search_bg > 3000 and light > 120 and icon_px > 200 else 1)
PYEOF3
  [ $? -eq 0 ] && ok "Programs menu opened (search bar + content visible)"     || bad "Programs menu did not render"
else
  bad "no frame dump for the menu check"
fi

# --- application launch from the DEFAULT category (Accessories, where
# --- the probe sorts alone → deterministic row 0); a category click on
# --- row 0 exercises the same path
ti "motion x=80 y=89"
ti "press b=1"
ti "release b=1"
sleep 0.2
# first application row: y = 38+34+4+13 = 89
ti "motion x=300 y=89"
ti "press b=1"
ti "release b=1"
for i in $(seq 1 20); do
  [ -f "$WORK/wl-launch-marker" ] && break
  sleep 0.1
done
if [ -f "$WORK/wl-launch-marker" ] &&    grep -q "launched" "$WORK/wl-launch-marker"; then
  ok "application LAUNCHED from the Programs menu (marker file)"
else
  bad "application did not launch from the menu"
fi
grep -q "wl-panel: launched" "$WORK/wm.log"   && ok "compositor logged the launch"   || bad "no launch log line in the compositor log"

# --- menu must close after launching (click went through) ---
sleep 0.2
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm
if [ -s /tmp/vantage-wayland.ppm ]; then
  python3 - <<'PYEOF4'
import sys
with open('/tmp/vantage-wayland.ppm','rb') as f:
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
band = pix[(38*w)*3 : (72*w)*3]
search_bg = band.count(bytes((0x2a, 0x2e, 0x35)))
sys.exit(0 if search_bg < 500 else 1)
PYEOF4
  [ $? -eq 0 ] && ok "menu closed after launching the application"     || bad "menu stayed open after launching"
fi

# ------------------------------------------------------ workspace PAGER
# The panel's workspace switcher is a real PAGER: each cell shows that
# desktop's windows as miniatures at their true relative geometry.
# Map a window on ws1, switch to ws2, map another, switch back — the
# frame must then show BOTH: a focused miniature (accent) in the active
# cell and an unfocused one (slate) in the ws2 cell, inside the bar.
echo "== harness-wayland: workspace pager miniatures =="
"$(vb vantage-remote)" ws 1 >/dev/null 2>&1
PG_A_LOG="$WORK/pager-a.log"
timeout 8 "$(tc vt-wayland-testclient)" 0xffaa7a3a 260 160 > "$PG_A_LOG" 2>&1 &
PG_A_PID=$!
for i in $(seq 1 60); do
  grep -q "^committed" "$PG_A_LOG" 2>/dev/null && break
  kill -0 "$PG_A_PID" 2>/dev/null || break
  sleep 0.05
done
"$(vb vantage-remote)" ws 2 >/dev/null 2>&1
PG_B_LOG="$WORK/pager-b.log"
timeout 8 "$(tc vt-wayland-testclient)" 0xff3a7aaa 260 160 > "$PG_B_LOG" 2>&1 &
PG_B_PID=$!
for i in $(seq 1 60); do
  grep -q "^committed" "$PG_B_LOG" 2>/dev/null && break
  kill -0 "$PG_B_PID" 2>/dev/null || break
  sleep 0.05
done
"$(vb vantage-remote)" ws 1 >/dev/null 2>&1
sleep 0.4
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm
if [ -s /tmp/vantage-wayland.ppm ]; then
  python3 - <<'PYEOF5'
import sys
with open('/tmp/vantage-wayland.ppm','rb') as f:
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
# the pager strip lives in the bar, right of the taskbar: scan the bar
# rows (4..30) in the right HALF of the screen for miniature fills
bar = pix[4*w*3 : 30*w*3]
half = bar[(w//2)*3:]
foc = half.count(bytes((0x6f, 0xaa, 0xe8)))   # focused miniature (accent)
unf = half.count(bytes((0x4a, 0x51, 0x60)))   # unfocused miniature (slate)
print(f"pager: focused-mini px={foc} unfocused-mini px={unf}")
sys.exit(0 if foc >= 6 and unf >= 6 else 1)
PYEOF5
  [ $? -eq 0 ] && ok "pager draws real window miniatures per workspace" \
    || bad "pager miniatures missing (focused/unfocused cells)"
else
  bad "no frame dump for the pager check"
fi
wait "$PG_A_PID" 2>/dev/null
wait "$PG_B_PID" 2>/dev/null

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

# ------------------------------------------------- autostart (isolated)
# The session must run the CONTROLLED user autostart entry (marker
# file) — real coverage of the autostart path — while the system
# autostart is provably isolated (skipped, host daemons untouched).
ASTARTED=""
for i in $(seq 1 30); do
  [ -f "$WORK/wl-autostart-marker" ] && { ASTARTED=1; break; }
  sleep 0.1
done
if [ -n "$ASTARTED" ] && grep -q "autostarted" "$WORK/wl-autostart-marker"; then
  ok "session autostart entry ran (marker file)"
else
  bad "session autostart entry did not run"
fi
grep -qF "session: system autostart skipped" "$SESS_LOG" \
  && ok "system autostart isolated (no /etc/xdg entries executed)" \
  || bad "system autostart was not isolated"

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
  grep -q "^keymap ok" "$CLIENT_LOG2" \
    && ok "wl_keyboard keymap valid under the full session" \
    || bad "keymap missing/invalid under the session"

  # frame-dump check: SIGUSR1 goes to the WM child (it runs in its own
  # session after setsid, so signal it directly via its pid from the log)
  WM_CHILD=$(grep -o "started 'wm' pid=[0-9]*" "$SESS_LOG" | head -1 | cut -d= -f2)
  if [ -n "${WM_CHILD:-}" ] && kill -0 "$WM_CHILD" 2>/dev/null; then
    kill -USR1 "$WM_CHILD" 2>/dev/null
    wait_ppm
    [ -s /tmp/vantage-wayland.ppm ] \
      && ok "frame dump written under the full session" \
      || bad "no frame dump under the session"
  fi
  # NOTE: do NOT terminate the session here — the logout round-trip
  # below must be the thing that stops it.
fi

# ------------------------------------------------- logout round-trip
# TWO real paths: (1) the panel's session menu driven by real pointer
# input, (2) vantage-remote logout → session IPC. Both must end the
# session with the graceful SIGTERM policy (no SIGKILL, no restart).
echo "== harness-wayland: panel-driven logout (real input path) =="
if [ -n "${SESS_PID:-}" ] && kill -0 "$SESS_PID" 2>/dev/null; then
  # open the username menu (right edge of the bar) then click Log Out
  ti "motion x=990 y=17"
  ti "press b=1"
  ti "release b=1"
  sleep 0.3
  # Log Out is the 4th action row: y = 38+4+4+3*26+13 = 134
  ti "motion x=990 y=134"
  ti "press b=1"
  ti "release b=1"
  for i in $(seq 1 80); do
    kill -0 "$SESS_PID" 2>/dev/null || break
    sleep 0.1
  done
fi
if ! kill -0 "$SESS_PID" 2>/dev/null; then
  ok "panel Log Out ended the session (no restart loop)"
  grep -q "intentional logout, ending the session" "$SESS_LOG" \
    && ok "supervisor recognized the intentional logout" \
    || bad "no intentional-logout policy log line"
else
  # NEVER silently skip a broken real-input path: the remote-logout
  # fallback below would still end the session and every later check
  # would pass — hiding the fact that the panel's session menu did
  # not react to clicks (exactly what a long-username machine hit
  # when the menu was anchored to the username applet).
  bad "panel Log Out did NOT end the session (session-menu click path broken)"
fi

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
else
  ok "session already ended by the panel logout (remote path skipped)"
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
                        echo "---- session.log ----"; cat "$SESS_LOG"; \
                        echo "---- failed checks ----"; cat "$FAILS"; }
[ "$FAIL" -eq 0 ]
