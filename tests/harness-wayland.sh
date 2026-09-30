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
vb() {
  if [ -z "$BIN" ]; then echo "$1"; return; fi
  if [ -x "$BIN/$1" ]; then echo "$BIN/$1"; return; fi
  # the panel is an independent subproject — its binary lives under
  # <builddir>/subprojects/panel (BIN is <builddir>/src/tools)
  local pb="$BIN/../../subprojects/panel/$1"
  if [ -x "$pb" ]; then echo "$pb"; return; fi
  echo "$1"
}
tc() { if [ -n "$TST" ]; then echo "$TST/$1"; else echo "$1"; fi; }
[ -x "$(tc vt-wayland-testclient)" ] || { echo "vt-wayland-testclient not built"; exit 77; }
[ -x "$(vb vantage-panel)" ] || { echo "vantage-panel not built (subprojects/panel)"; exit 77; }

# The panel must be able to DOCK. A build configured while
# gtk4-layer-shell was missing compiled the panel WITHOUT layer-shell
# support (HAVE_LAYER_SHELL undefined); it then maps as a plain
# floating window and EVERY pixel/interaction check below fails for
# the wrong reason — observed on a real machine as seven cascading
# failures (panel never mapped / frame dump / menu / launch / pager)
# with nothing pointing at the build. Fail HERE instead, with the fix
# spelled out. (Since 0.3.2 the meson build makes the library required
# by default, so fresh configurations cannot fall into this hole;
# this check catches stale build dirs and hand-swapped binaries.)
PANEL_BIN="$(vb vantage-panel)"
PANEL_LINKS=$(ldd "$PANEL_BIN" 2>/dev/null)
if printf '%s\n' "$PANEL_LINKS" | grep -q 'libgtk4-layer-shell'; then
  if printf '%s\n' "$PANEL_LINKS" | grep 'libgtk4-layer-shell' \
       | grep -q 'not found'; then
    echo "FATAL: $PANEL_BIN links libgtk4-layer-shell but the library"
    echo "       cannot be loaded at runtime (broken install / wrong"
    echo "       LD_LIBRARY_PATH). Fix the library resolution and retry."
    exit 1
  fi
  ok "panel links gtk4-layer-shell (dockable on Wayland)"
else
  echo "FATAL: $PANEL_BIN was built WITHOUT gtk4-layer-shell — it cannot"
  echo "       dock on Wayland and every panel check would fail misleadingly."
  echo "       The dependency 'gtk4-layer-shell-0' was not found when this"
  echo "       build directory was configured. Fix:"
  echo "         Debian/Ubuntu: sudo apt install libgtk4-layer-shell-dev"
  echo "         Arch:          sudo pacman -S gtk4-layer-shell"
  echo "         Fedora:        sudo dnf install gtk4-layer-shell-devel"
  echo "       then: meson setup --reconfigure $BUILD   (or wipe the build dir)"
  echo "       X11-only builds: meson setup -Dpanel:layer-shell=disabled"
  exit 1
fi

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
# --- GTK determinism on REAL machines ---------------------------------
# The harness runs on developer desktops where a full session
# environment exists. Left as-is, the host leaks into the GTK4 panel:
#   * the user's GTK theme / dark-mode -> popover colors and row
#     geometry follow the machine (the panel's own CSS pins its
#     visual identity; GTK_THEME locks the base theme as well)
#   * DBUS_SESSION_BUS_ADDRESS -> the panel registers on the USER's
#     session bus (GApplication single-instance: a leftover panel
#     from an earlier run would steal activation) and reads portal
#     settings
#   * GDK_SCALE / GDK_DPI_SCALE (HiDPI setups) -> 2x panel geometry
export GTK_THEME=Adwaita
unset GDK_SCALE GDK_DPI_SCALE GTK_PATH GTK_MODULES GTK_USE_PORTAL \
      DBUS_SESSION_BUS_ADDRESS DBUS_STARTER_BUS_TYPE \
      DBUS_STARTER_ADDRESS GIO_MODULE_DIR
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
Exec=/bin/sh -c 'echo "launched DISPLAY=$DISPLAY XAUTHORITY=$XAUTHORITY WAYLAND=$WAYLAND_DISPLAY" > $WORK/wl-launch-marker'
Icon=vt-harness-probe
Categories=Utility;
DESK
sed -i "s|\$WORK|$WORK|g" "$WORK/data/applications/vt-harness-probe.desktop"
# 60 filler applications in the SAME Utility category: the Programs
# menu's app list must SCROLL with more entries than fit on screen —
# the scrolling regression check below depends on them being present
# BEFORE the panel process starts (the launcher database is loaded at
# panel construction).
for _i in $(seq -w 1 60); do
  cat > "$WORK/data/applications/vt-harness-filler$_i.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Zzz Filler $_i
Exec=/bin/true
Categories=Utility;
DESK
done
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
# The GTK4 panel resolves icons through GtkIconTheme, which ALWAYS
# falls back to hicolor — install the fixtures there too so the
# icon-path assertions are theme-name-independent
HICOLOR_DIR="$WORK/data/icons/hicolor/24x24/apps"
mkdir -p "$HICOLOR_DIR"
cp "$ICON_DIR/vt-harness-probe.png" "$HICOLOR_DIR/"
cat > "$WORK/data/icons/hicolor/index.theme" <<'EOF'
[Icon Theme]
Name=hicolor
Directories=24x24/apps

[24x24/apps]
Size=24
Context=Applications
Type=Fixed
EOF
# a themed START icon (solid gold) — the Programs button must resolve
# it from the icon theme instead of its drawn fallback
python3 - "$HICOLOR_DIR/start-here.png" <<'PYICON2'
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
cat > "$WORK/data/applications/vt-harness-term.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=Zz Harness Terminal
Exec=/bin/true
Terminal=true
Categories=System;
DESK
# GTK's OWN fallback icon: gtk_icon_theme_lookup_icon() recurses into
# itself WITHOUT BOUND when "image-missing" is absent from every theme
# (GTK 4.18 has no recursion guard) — ship it in both fixture themes
# so any GTK-internal missing-icon lookup cannot overflow the stack
python3 - "$HICOLOR_DIR/image-missing.png" <<'PYICON3'
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
cp "$HICOLOR_DIR/image-missing.png" "$ICON_DIR/"
# taskbar icon for the REAL GTK4 probe window: its app_id is
# org.vantage.gtkprobe — a solid TEAL icon (used nowhere else on the
# screen) makes the taskbar button LOCATABLE by pixel scan, which the
# taskbar-dropdown checks below need (right-click → context menu).
python3 - "$HICOLOR_DIR/org.vantage.gtkprobe.png" <<'PYICON4'
import struct, zlib, sys
w = h = 24
rgb = (0x20, 0x9a, 0x8a)   # teal — unique to the probe's taskbar button
raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack(
        '>I', zlib.crc32(c) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(data)
PYICON4
cp "$HICOLOR_DIR/org.vantage.gtkprobe.png" "$ICON_DIR/"
# same art under the BINARY name: GDK sends set_app_id from the
# program name on some GTK builds (observed: app_id "vt-gtk4-probe"
# despite GtkApplication("org.vantage.gtkprobe")) — ship both names
# so the button is locatable on every machine
cp "$HICOLOR_DIR/org.vantage.gtkprobe.png" "$HICOLOR_DIR/vt-gtk4-probe.png"
cp "$HICOLOR_DIR/org.vantage.gtkprobe.png" "$ICON_DIR/vt-gtk4-probe.png"
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

# The panel is a CLIENT now (GTK4, subprojects/panel): docked through
# wlr-layer-shell, driven by the same WM IPC the taskbar/pager use.
echo "== harness-wayland: starting the GTK4 panel client =="
PANEL_LOG="$WORK/panel.log"
: > "$PANEL_LOG"
"$(vb vantage-panel)" > "$PANEL_LOG" 2>&1 &
PANEL_PID=$!
PANEL_READY=""
for i in $(seq 1 150); do
  grep -q "vantage-panel: ready" "$PANEL_LOG" 2>/dev/null && PANEL_READY=1 && break
  kill -0 "$PANEL_PID" 2>/dev/null || break
  sleep 0.1
done
[ -n "$PANEL_READY" ] && ok "GTK4 panel client started (layer-shell dock)" \
  || bad "panel client did not reach ready: $(tail -3 "$PANEL_LOG")"
# The READY line only means the process is up — its layer surface must
# actually MAP before any pixel is on screen. GTK's first frame takes
# a variable amount of time (CSS + icon theme + Pango fonts + the
# layer configure/ack/commit round-trips); on a fast machine a fixed
# sleep can easily photograph an empty desktop instead of the panel
# (observed on a real-hardware box: every panel pixel check failed
# while the panel process was alive and healthy).
PANEL_MAPPED=""
for i in $(seq 1 150); do
  grep -q "layer surface 'vantage-panel' mapped" "$WORK/wm.log" 2>/dev/null \
    && { PANEL_MAPPED=1; break; }
  kill -0 "$PANEL_PID" 2>/dev/null || break
  kill -0 "$WM_PID" 2>/dev/null || break
  sleep 0.1
done
[ -n "$PANEL_MAPPED" ] && ok "panel layer surface mapped (pixels on screen)" \
  || bad "panel surface never mapped: $(tail -5 "$PANEL_LOG")"
# first frame committed at map time; one paint cycle settles the bar
sleep 0.5

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

# ------------------------------------------------------------- registry
# The staging protocols must be advertised with their SPEC names (an
# independent client — no toolkit's own detection bugs involved; foot
# <= 1.22 matched the WRONG name for the toplevel-icon manager and
# warned regardless of compositor support, fixed upstream in 1.23).
if [ -x "$(tc vt-registry-probe)" ]; then
  if "$(tc vt-registry-probe)" > "$WORK/registry.log" 2>&1; then
    ok "staging protocols advertised (cursor-shape, activation, fractional-scale, toplevel-icon, viewporter)"
  else
    bad "staging protocols missing from the registry: $(tail -1 "$WORK/registry.log")"
  fi
else
  echo "  (vt-registry-probe not built — registry check skipped)"
fi

# ------------------------------------------------------- GPU dmabuf path
# The hardware-acceleration contract: zwp_linux_dmabuf_v1 v4 with EGL
# import + readback. The probe does what a GPU app does (feedback →
# params → create_immed → attach → frame-callback animation) and must
# BOTH map on screen and keep redrawing — the exact failure modes of
# the browser-stuck-on-one-frame bug class. On headless boxes the EGL
# device is llvmpipe (the same import code path a real GPU driver
# runs); on real hardware the render node takes over automatically.
if [ -x "$(tc vt-dmabuf-probe)" ]; then
  DMOUT=$(timeout 6 "$(tc vt-dmabuf-probe)" 4 2>&1 | tail -1)
  if echo "$DMOUT" | grep -q "imported=yes" && \
     echo "$DMOUT" | grep -q "feedback=yes"; then
    ok "linux-dmabuf v4: global advertised, feedback parsed, buffers imported (EGL)"
  else
    bad "linux-dmabuf probe: $DMOUT"
  fi
  DMFRAMES=$(echo "$DMOUT" | grep -o 'frames=[0-9]*' | cut -d= -f2)
  if [ -n "$DMFRAMES" ] && [ "$DMFRAMES" -gt 60 ]; then
    ok "dmabuf client animates continuously ($DMFRAMES frames/4s — no single-frame stall)"
  else
    bad "dmabuf client stalled (frames=$DMFRAMES — the browser-freeze class)"
  fi
else
  echo "  (vt-dmabuf-probe not built — dmabuf check skipped)"
fi

# Frame-callback pacing (SHM clients): the compositor's frame clock
# must fire wl_surface.frame callbacks without coupling them to
# damage-driven repaints — weston-simple-damage froze at a random frame
# before the fix (the lost-callback race).
if [ -x "$(tc vt-frame-probe)" ]; then
  FPOUT=$(timeout 6 "$(tc vt-frame-probe)" 4 2>&1 | tail -1)
  FPFRAMES=$(echo "$FPOUT" | grep -o 'frames=[0-9]*' | cut -d= -f2)
  if [ -n "$FPFRAMES" ] && [ "$FPFRAMES" -gt 60 ]; then
    ok "frame-callback clock keeps SHM clients animating ($FPFRAMES frames/4s)"
  else
    bad "frame-callback stall for SHM clients (frames=$FPFRAMES)"
  fi
else
  echo "  (vt-frame-probe not built — frame-clock check skipped)"
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
# panel assertions: the GTK4 panel docks at the top (45 rows) — its
# dark bar + light icon/text pixels must cover a real strip; old
# placeholder squares must be GONE. Panel presence is asserted through
# its STRUCTURAL signature: the pager cells (accent current-desktop
# cell + slate other cells — colors used nowhere else) and the light
# bar text. (An earlier heuristic counted pixels differing from the
# old flat gray wallpaper; it silently depended on the bar being an
# alpha-over-light #1f2125 blend, which vanished with the dark window
# background fix.)
def pnp(x, y):
    i = (y*w+x)*3
    return (pix[i], pix[i+1], pix[i+2])
# panel assertions: the GTK4 panel docks at the top — its dark bar +
# light icon/text pixels must cover a real strip; old placeholder
# squares must be GONE. Panel presence is asserted through its
# STRUCTURAL signature: the pager cells (accent current-desktop cell
# + slate other cells — colors used nowhere else) and the light bar
# text. The band is deliberately TALL (rows 4..54): the bar's natural
# height follows the machine's font metrics (44..52px measured), so a
# fixed 45-row window can photograph past the cells on big fonts.
panel_cells = sum(1 for y in range(4, 54) for x in range(0, w, 2)
                  if pnp(x, y) in ((0x47, 0x75, 0xc7), (0x2e, 0x30, 0x38)))
top = pix[:w*58*3]
light = sum(1 for i in range(0, len(top), 3)
            if top[i] >= 0x90 and top[i+1] >= 0x90 and top[i+2] >= 0x90)
placeholder_red = pix.count(bytes((0xe0, 0x5a, 0x5a)))
placeholder_green = pix.count(bytes((0x7a, 0xc8, 0x60)))
# BACKGROUND: the desktop now renders the [wallpaper] config — the
# default vertical navy gradient. The old bug painted a flat hardcoded
# gray everywhere. Sample far from windows/panel: left edge, below the
# panel (y=64: the GTK panel's natural height is ~45 rows on both
# backends — y=40 would land ON the bar) and near the bottom (y=h-8):
# gradient colors differ and match the engine's interpolation between
# (18,23,36) and (38,48,79).
def px(x, y):
    i = (y*w+x)*3
    return (pix[i], pix[i+1], pix[i+2])
c_top = px(4, 64)
c_bot = px(4, h - 8)
def near(c, t, tol=8):
    return all(abs(a-b) <= tol for a, b in zip(c, t))
grad_top_ok = near(c_top, (18, 23, 36))
grad_bot_ok = near(c_bot, (38, 48, 79))
grad_diff = c_bot[2] - c_top[2] >= 12
print(f"frame: {w}x{h}, client-color pixels={hits}, panel-cells px={panel_cells}, "
      f"panel-light px={light}, placeholder-red={placeholder_red}, "
      f"placeholder-green={placeholder_green}")
print(f"background: top={c_top} bottom={c_bot} "
      f"(gradient {'OK' if grad_top_ok and grad_bot_ok and grad_diff else 'BAD'})")
panel_ok = panel_cells > 800 and light > 40
placeholders_gone = placeholder_red == 0 and placeholder_green == 0
sys.exit(0 if (hits > 1000 and panel_ok and placeholders_gone and
               grad_top_ok and grad_bot_ok and grad_diff) else 1)
PYEOF
  [ $? -eq 0 ] && ok "client pixels + docked panel in frame dump" \
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

# --- Programs menu: click the Programs button, verify the menu opens
# --- (popover: the shell's own pinned dark sheet + light text/icons —
# --- font-independent light-pixel counting below the bar). The menu's
# --- content arrives over several composited frames (async icon
# --- loads + text layout), so the check POLLS fresh frame dumps
# --- instead of photographing one arbitrary instant.
ti "click 60,22"
MENU_OK=""
for try in 1 2 3 4 5 6; do
  sleep 0.5
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || continue
  [ -s /tmp/vantage-wayland.ppm ] || continue
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
# The popover spans ~560px below the bar: the shell's pinned dark
# sheet + light text. Verify (a) the menu area differs from the bare
# gradient wallpaper, (b) light text/icons are visibly rendered
# (font-independent: any antialiased text produces mid-bright pixels),
# (c) the probe's icon (solid 0xc04080 PNG from the isolated icon
# theme) is rendered — counted with per-channel tolerance, because
# GTK resamples the 24px fixture down to the row's 22px with
# filtering (exact-color counting finds only the interior).
def wpdiff(x, y):
    i = (y*w+x)*3
    c = (pix[i], pix[i+1], pix[i+2])
    # bare gradient at this height (interpolated navy)
    t = 1.0 - y / h
    g0, g1 = (18, 23, 36), (38, 48, 79)
    ref = tuple(int(a + (b - a) * t) for a, b in zip(g0, g1))
    return abs(c[0]-ref[0]) + abs(c[1]-ref[1]) + abs(c[2]-ref[2])
menu = pix[(48*w)*3 : (420*w)*3]
light = 0
for i in range(0, len(menu), 3):
    if menu[i] >= 0x90 and menu[i+1] >= 0x90 and menu[i+2] >= 0x90:
        light += 1
area_px = sum(1 for y in range(50, 415, 3) for x in range(8, 560, 6)
              if wpdiff(x, y) > 20)
near = sum(1 for i in range(0, len(pix) - 2, 3)
           if abs(pix[i] - 0xc0) <= 24 and abs(pix[i+1] - 0x40) <= 24
           and abs(pix[i+2] - 0x80) <= 24)
icon_px = near
print(f"menu: area-diff={area_px} light-text={light} icon-px={icon_px}")
sys.exit(0 if area_px > 4000 and light > 150 and icon_px > 100 else 1)
PYEOF3
  [ $? -eq 0 ] && { MENU_OK=1; break; }
done
[ -n "$MENU_OK" ] && ok "Programs menu opened (search bar + content visible)" \
  || bad "Programs menu did not render"

# --- application launch: the menu's DEFAULT category is Accessories,
# --- where the probe sorts alone; its row sits at the top of the right
# --- column. The rows' hit boxes are pinned by the panel's own CSS
# --- (30px min-height); the scan still walks a few heights because a
# --- miss only lands on empty list space (harmless).
LAUNCHED=""
for row_y in 120 140 160 180 200 220 240 260; do
  [ -n "$LAUNCHED" ] && break
  ti "click 300,$row_y"
  for i in $(seq 1 8); do
    [ -f "$WORK/wl-launch-marker" ] && { LAUNCHED=1; break; }
    sleep 0.1
  done
done
if [ -n "$LAUNCHED" ] && grep -q "launched" "$WORK/wl-launch-marker"; then
  ok "application LAUNCHED from the Programs menu (marker file)"
else
  bad "application did not launch from the menu"
fi

# --- XWayland env through the REAL user path: the panel exports the
# --- Xwayland DISPLAY/XAUTHORITY to launched apps (the reported
# --- "X11 apps cannot be launched through XWayland": the panel's own
# --- env has no DISPLAY — setenv cannot cross process boundaries).
# --- The marker records what the launched app actually SAW. This is
# --- the STANDALONE-compositor phase: Xwayland + panel both run under
# --- the headless WM above.
if [ -n "$LAUNCHED" ]; then
  XWL_DISPLAY1=$(grep -o 'xwayland: ready — DISPLAY=:[0-9]*' "$WORK/wm.log" \
                 2>/dev/null | head -1 | grep -o ':[0-9]*$')
  if [ -n "$XWL_DISPLAY1" ]; then
    grep -q "DISPLAY=$XWL_DISPLAY1" "$WORK/wl-launch-marker" \
      && ok "panel-launched app got the Xwayland DISPLAY ($XWL_DISPLAY1) — X11 apps launch" \
      || bad "panel-launched app lacked DISPLAY=$XWL_DISPLAY1: $(cat "$WORK/wl-launch-marker")"
    grep -q "WAYLAND=$SOCKET" "$WORK/wl-launch-marker" \
      && ok "panel-launched app keeps WAYLAND_DISPLAY (native apps unaffected)" \
      || bad "panel-launched app lost WAYLAND_DISPLAY: $(cat "$WORK/wl-launch-marker")"
    # and the xwl-env IPC answers the same truth the panel used
    XWLENV_OUT=$(WAYLAND_DISPLAY="$SOCKET" "$(vb vantage-remote)" xwl-env 2>/dev/null || true)
    echo "$XWLENV_OUT" | grep -q "display=$XWL_DISPLAY1" \
      && ok "vantage-remote xwl-env reports the live display" \
      || bad "xwl-env IPC wrong: '$XWLENV_OUT'"
  else
    bad "standalone compositor never started Xwayland (no ready line)"
  fi
fi

# --- menu must close after launching (click went through) ---
# The cursor is moved OUT of the scanned band first: it sits at the
# last click position, dead inside the menu area, and a visible
# cursor sprite (every real machine — the built-in arrow has 75 white
# pixels) would read as "menu still open" even with the menu gone.
ti "motion x=12 y=740"
sleep 0.3
MENU_CLOSED=""
for try in 1 2 3 4; do
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || continue
  [ -s /tmp/vantage-wayland.ppm ] || continue
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
# menu gone: the area below the bar is bare gradient again (light
# text pixels vanish)
menu = pix[(48*w)*3 : (420*w)*3]
light = 0
for i in range(0, len(menu), 3):
    if menu[i] >= 0x90 and menu[i+1] >= 0x90 and menu[i+2] >= 0x90:
        light += 1
sys.exit(0 if light < 40 else 1)
PYEOF4
  [ $? -eq 0 ] && { MENU_CLOSED=1; break; }
  sleep 0.5
done
[ -n "$MENU_CLOSED" ] && ok "menu closed after launching the application" \
  || bad "menu stayed open after launching"

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
# the panel polls the WM at 400 ms and re-renders on events; the dump
# is retried because the miniature state must be committed to the
# compositor framebuffer before it can be photographed
sleep 0.9
PAGER_OK=""
for try in 1 2 3 4; do
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || continue
  [ -s /tmp/vantage-wayland.ppm ] || continue
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
# the pager sits right of the Programs button: the cells are 64x34
# and vertically centered in the bar. The scan LOCATES the cells
# first (their exact fill colors are used nowhere else) and then
# counts the miniatures INSIDE that band only — the bar's height
# and the pager's x-offset follow the machine's font metrics (the
# Programs label width), and counting near-white pixels across the
# whole bar would also add up the tasklist/applet glyphs, diluting
# the focused/unfocused-miniature assertions. Current cell
# background is the accent (0.28,0.46,0.78 → ~#4775c7), the other
# cells are slate (#2e3038); the focused window miniature is
# near-white, unfocused is mid-slate #9ea3ad.
def is_cell_bg(r, g, b):
    return (abs(r-0x47) <= 2 and abs(g-0x75) <= 2 and
            abs(b-0xc7) <= 2) or (abs(r-0x2e) <= 2 and
                                  abs(g-0x30) <= 2 and abs(b-0x38) <= 2)
# column DENSITY filters the band: a real cell fills 30+ rows of its
# columns, while stray antialiased text pixels elsewhere in the bar
# match the slate tolerance for exactly one row
from collections import Counter
colcnt = Counter()
for y in range(4, 54):
    for x in range(0, w):
        i = (y*w + x)*3
        if is_cell_bg(pix[i], pix[i+1], pix[i+2]):
            colcnt[x] += 1
cols = sorted(x for x, c in colcnt.items() if c >= 20)
cell_bg = sum(colcnt[x] for x in cols)
foc = unf = 0
if cols:
    x0, x1 = cols[0], cols[-1]
    for y in range(4, 54):
        for x in range(x0, x1 + 1):
            i = (y*w + x)*3
            r, g, b = pix[i], pix[i+1], pix[i+2]
            if r >= 0xe0 and g >= 0xe0 and b >= 0xe0:
                foc += 1
            elif (abs(r-0x9e) <= 2 and abs(g-0xa3) <= 2 and
                  abs(b-0xad) <= 2):
                unf += 1
print(f"pager: cells at x={cols[0] if cols else '-'}..{cols[-1] if cols else '-'}"
      f" focused-mini px={foc} unfocused-mini px={unf} cell-bg={cell_bg}")
# all three must be present: the focused miniature on the current
# desktop, the UNFOCUSED miniature on the other desktop's cell (pins
# the WM_QUERY empty-flags parser: an empty flags column used to make
# strtok_r collapse the tabs and drop the window entirely), and the
# accent background of the current cell (pins the per-cell queue_draw:
# cells used to freeze at their first snapshot)
sys.exit(0 if (foc >= 6 and unf >= 4 and cell_bg >= 20) else 1)
PYEOF5
  [ $? -eq 0 ] && { PAGER_OK=1; break; }
  sleep 0.5
done
[ -n "$PAGER_OK" ] && ok "pager draws real window miniatures per workspace" \
  || bad "pager miniatures missing (focused/unfocused cells)"
wait "$PG_A_PID" 2>/dev/null
wait "$PG_B_PID" 2>/dev/null

# ------------------------------------------------- burst drag stress
# "moving the windows too fast bugs out the pager ... it locks the
# entire DE until it updates": a full-speed drag floods the WM event
# socket with geometry broadcasts; the old BLOCKING write parked the
# compositor inside send() until the panel drained (~2 s whole-DE
# freeze), and the misaligned WS answer collapsed the pager to one
# cell mid-drag. The dragged window is a REAL X11 app through
# Xwayland — the user's exact scenario (SSD title bar, X-side
# configure round-trips) — which also pins the Composite-Redirect
# pixel path: an Xwayland window that renders nothing has no color
# blob to find and fails here. Pins: the 5000-step burst dispatches
# fast (throttle + non-blocking broadcasts), the window lands where
# it was dragged with its title bar still REACHABLE (frame-aware
# workarea clamp), and the pager still shows the full 4-cell band.
echo "== harness-wayland: full-speed drag burst (no DE lock, pager intact) =="
XWL_DISPLAY1=$(grep -o 'xwayland: ready — DISPLAY=:[0-9]*' "$WORK/wm.log" \
                2>/dev/null | head -1 | grep -o ':[0-9]*$')
XWL_AUTH1=$(grep -o 'auth [^ )]*' "$WORK/wm.log" 2>/dev/null \
            | head -1 | cut -d' ' -f2)
DRAG_LOG="$WORK/drag-client.log"
if [ -n "$XWL_DISPLAY1" ] && [ -n "$XWL_AUTH1" ]; then
  timeout 12 env DISPLAY="$XWL_DISPLAY1" XAUTHORITY="$XWL_AUTH1" \
    "$(tc vt-x11-testclient)" --seconds 10 --title "DragProbe" \
    > "$DRAG_LOG" 2>&1 &
  DRAG_PID=$!
  for i in $(seq 1 80); do
    grep -q "^mapped" "$DRAG_LOG" 2>/dev/null && break
    kill -0 "$DRAG_PID" 2>/dev/null || break
    sleep 0.05
  done
  sleep 0.4    # frame + WM-manage settle
fi
# locate the dragged window by its unique color: the X11 test
# client's second window is solid 0x9a3a5f (300x220)
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm || true
DRAG_GEOM=$(python3 - <<'PYDRAG0'
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
xs, ys = [], []
for y in range(0, h, 2):
    for x in range(0, w, 2):
        i = (y*w + x)*3
        if abs(pix[i]-0x9a) <= 8 and abs(pix[i+1]-0x3a) <= 8 \
           and abs(pix[i+2]-0x5f) <= 8:
            xs.append(x); ys.append(y)
if xs and len(xs) > 500:
    print(f"{(min(xs)+max(xs))//2} {min(ys)}")
PYDRAG0
)
TBX=${DRAG_GEOM%% *}
# the SSD title bar sits ABOVE the client surface (26px title + 2px
# border): grab the middle of the title band, not the client area
TBY=$(($(echo "$DRAG_GEOM" | cut -d' ' -f 2) - 14))
if [ -n "$TBX" ] && [ "$TBX" -gt 0 ] 2>/dev/null; then
  ti "motion x=$TBX y=$TBY"
  ti "press b=1"
  # ONE IPC dispatch carrying 5000 motion steps — the reproducer
  # class for the flood: the old code emitted one geometry broadcast
  # per step (350 KB of events, more than the socket buffer) and
  # blocked inside send() until the panel drained
  BURST_T0=$(date +%s%N)
  ti "burst n=5000 x0=$TBX y0=$TBY x1=250 y1=520"
  BURST_RC=$?
  BURST_T1=$(date +%s%N)
  BURST_MS=$(( (BURST_T1 - BURST_T0) / 1000000 ))
  ti "release b=1"
  # second burst: drag the window UP INTO the panel — the workarea
  # clamp must keep the title bar grabbable below the docked panel
  ti "press b=1"
  ti "burst n=2000 x0=250 y0=520 x1=300 y1=-400"
  ti "release b=1"
  # compositor liveness: a fresh frame dump must arrive promptly (a
  # wedged main loop never services the dump request)
  rm -f /tmp/vantage-wayland.ppm
  LIVE_T0=$(date +%s%N)
  kill -USR1 "$WM_PID" 2>/dev/null
  DRAG_LIVE=""
  for i in $(seq 1 40); do
    [ -s /tmp/vantage-wayland.ppm ] && { DRAG_LIVE=1; break; }
    sleep 0.05
  done
  LIVE_T1=$(date +%s%N)
  LIVE_MS=$(( (LIVE_T1 - LIVE_T0) / 1000000 ))
  [ "$BURST_RC" -eq 0 ] && [ "$BURST_MS" -lt 1500 ] \
    && ok "5000-step drag burst dispatched in ${BURST_MS}ms (event path never blocks)" \
    || bad "drag burst wedged the compositor (rc=$BURST_RC, ${BURST_MS}ms)"
  [ -n "$DRAG_LIVE" ] && [ "$LIVE_MS" -lt 400 ] \
    && ok "compositor responsive right after the burst (dump in ${LIVE_MS}ms)" \
    || bad "compositor unresponsive after the burst (dump ${LIVE_MS}ms)"
  # let the panel's 400 ms refresh tick resync, then verify the window
  # moved + title bar reachable + pager still the full 4-cell band.
  # RETRY the dump: the panel occasionally remaps one of its popups
  # (GDK "moving popups" → unmap+re-map) and a frame photographed during
  # that gap legitimately shows no bar — one bad frame is a transient,
  # not a regression; a real pager collapse reproduces on every dump.
  sleep 0.9
  DRAG_VERDICT=1
  for dtry in 1 2 3; do
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || true
  python3 - <<'PYDRAG1'
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
def near(r,g,b,R,G,B,t=6):
    return abs(r-R)<=t and abs(g-G)<=t and abs(b-B)<=t
# the window after both drags: unique color, its top must sit at the
# workarea top PLUS the SSD title band (title 26 + border 2): the
# whole frame — grab bar included — stays below the docked panel.
# Panel height follows the font (44..58px) → expected top ≈ 72..86,
# asserted with margin; a client clamped to the BARE workarea top
# would hide the title bar behind the panel (the old off-by-28 bug)
xs, ys = [], []
for y in range(0, h, 2):
    for x in range(0, w, 2):
        i = (y*w + x)*3
        if near(pix[i],pix[i+1],pix[i+2],0x9a,0x3a,0x5f,8):
            xs.append(x); ys.append(y)
top = min(ys) if ys else -1
# the dragged window's CONTENT must still exist at full size: the old
# 1x1-drag bug (a move-only _xwl_configure sent WIDTH=1 HEIGHT=1)
# collapsed the window on the first title-bar drag — the check below
# then "passed" on one stray antialiased pixel by luck. Require a
# real blob: >= 2000 sampled pixels (~300x220/4) at the clamped top.
blob_n = len(xs)
blob_w = (max(xs) - min(xs)) if xs else 0
blob_h = (max(ys) - min(ys)) if ys else 0
# pager band integrity: 4 cells of 64px + gaps ≈ 280 columns of cell
# background; the mid-drag WS misalignment used to collapse it to 1
from collections import Counter
colcnt = Counter()
for y in range(4, 54):
    for x in range(0, w):
        i = (y*w + x)*3
        if near(pix[i],pix[i+1],pix[i+2],0x47,0x75,0xc7,2) or \
           near(pix[i],pix[i+1],pix[i+2],0x2e,0x30,0x38,2):
            colcnt[x] += 1
cols = sorted(x for x, c in colcnt.items() if c >= 20)
span = (cols[-1] - cols[0]) if cols else 0  # MUST sort: Counter order is first-seen, cols[-1] was arbitrary
foc = 0
if cols:
    for y in range(4, 54):
        for x in range(cols[0], cols[-1] + 1):
            i = (y*w + x)*3
            if pix[i] >= 0xe0 and pix[i+1] >= 0xe0 and pix[i+2] >= 0xe0:
                foc += 1
print(f"drag: win-top={top} blob={blob_n}px {blob_w}x{blob_h} pager-span={span} focused-mini={foc}")
# span==0: the panel is mid-remap in THIS frame — retry the dump
# before declaring failure
if span == 0 and foc == 0:
    sys.exit(2)
sys.exit(0 if (70 <= top <= 100 and blob_n >= 2000 and blob_w >= 280
               and blob_h >= 200 and span >= 200 and foc >= 6) else 1)
PYDRAG1
  DRAG_VERDICT=$?
  [ "$DRAG_VERDICT" -eq 2 ] && { sleep 0.5; continue; }
  break
  done
  [ "$DRAG_VERDICT" -eq 0 ] && ok "window dragged: title bar reachable, pager 4-cell band intact" \
    || bad "drag result wrong (window strand/pager collapse)"
else
  bad "X11 drag window never rendered under Xwayland (composite path?)"
fi
wait "${DRAG_PID:-}" 2>/dev/null

# ------------------------------------------------- SSD edge resize
# "Native Wayland applications have black bars around their windows ...
# they appear to provide some form of window-resizing area, but the
# actual resize functionality does not seem to be implemented": the
# bars ARE the SSD frame; resizing them was bottom-right-only math
# with no cursor affordance. Pins: every edge resizes with EDGE-AWARE
# geometry (W/N move the origin), corners work, the model tracks
# mid-drag, and the resize cursor shows over the edge.
echo "== harness-wayland: SSD frame edge-resize (native Wayland windows) =="
RSZ_LOG="$WORK/ssd-resize.log"
# long-lived: the whole section (6 resizes + cursor + flow probe) takes
# ~9 s — the client's default 6 s lifetime would kill the window mid-
# section and every later read would see garbage
VT_TESTCLIENT_SECONDS=30 "$(tc vt-wayland-testclient)" --ssd 0xff5a9a3a 300 200 > "$RSZ_LOG" 2>&1 &
RSZ_PID=$!
sleep 1.2
wgeo() { "$(vb vantage-remote)" list 2>/dev/null | grep "Vantage Wayland Test" \
           | head -1 | sed -E 's/.*\t(-?[0-9]+)\t(-?[0-9]+)\t([0-9]+)\t([0-9]+)$/\1 \2 \3 \4/'; }
rsz_case() {  # name edge dx dy exp_dx exp_dy exp_dw exp_dh
  local name="$1" edge="$2" dx="$3" dy="$4" \
        edx="$5" edy="$6" edw="$7" edh="$8"
  local g; g=$(wgeo); read -r X Y WD H <<< "$g"
  local fx=$((X - 2)) fy=$((Y - 28)) fw=$((WD + 4)) fh=$((H + 28)) px py
  case "$edge" in
    E)  px=$((fx + fw - 4)); py=$((fy + 28 + H / 2)) ;;
    W)  px=$((fx + 4));      py=$((fy + 28 + H / 2)) ;;
    N)  px=$((fx + fw / 2)); py=$((fy + 4)) ;;
    S)  px=$((fx + fw / 2)); py=$((fy + fh - 4)) ;;
    SE) px=$((fx + fw - 4)); py=$((fy + fh - 4)) ;;
    NW) px=$((fx + 4));      py=$((fy + 4)) ;;
  esac
  ti "motion x=$px y=$py"
  sleep 0.15
  ti "press b=1"
  sleep 0.1
  ti "motion x=$((px + dx)) y=$((py + dy))"
  sleep 0.15
  ti "release b=1"
  sleep 0.25
  local g2; g2=$(wgeo); read -r X2 Y2 W2 H2 <<< "$g2"
  if [ $((X2 - X)) -eq $edx ] && [ $((Y2 - Y)) -eq $edy ] && \
     [ $((W2 - WD)) -eq $edw ] && [ $((H2 - H)) -eq $edh ]; then
    ok "SSD resize: $name"
  else
    bad "SSD resize $name: want d=($edx,$edy,$edw,$edh) got ($((X2-X)),$((Y2-Y)),$((W2-WD)),$((H2-H)))"
  fi
}
rsz_case "EAST edge grows width, origin fixed"    E  60  0   0  0  60  0
rsz_case "WEST edge grows width, origin moves"    W -40  0 -40  0  40  0
rsz_case "NORTH edge grows height, origin moves"  N   0 -30   0 -30  0  30
rsz_case "SOUTH edge grows height"                S   0  50   0  0   0  50
rsz_case "SE corner both axes"                    SE 30  30   0  0  30  30
rsz_case "NW corner both axes + origin"           NW -25 -25 -25 -25 25  25

# ------------------------------------------------------ fullscreen safety
# "Fullscreen/maximize must not be affected by resize decorations": a
# fullscreen window owns the ENTIRE output — no SSD pixels, no resize
# hitbox at the screen edges, and the panel is covered (raw-output
# fullscreen semantics). Edge drags MUST NOT resize it. Uses the
# --apply-configure client (a real toolkit applies the configured
# fullscreen size; the synthetic blind-ack client never would).
{
  FS_LOG="$WORK/fs-client.log"
  VT_TESTCLIENT_SECONDS=20 "$(tc vt-wayland-testclient)" \
      --apply-configure --ssd 0xff5a9a3a 300 200 > "$FS_LOG" 2>&1 &
  FS_PID=$!
  sleep 1.2
  # the fullscreen client is the NEWEST one (the resize section's
  # client is still alive with the same title)
  wgeo_fs() { "$(vb vantage-remote)" list 2>/dev/null | grep "Vantage Wayland Test" \
           | tail -1 | sed -E 's/.*\t(-?[0-9]+)\t(-?[0-9]+)\t([0-9]+)\t([0-9]+)$/\1 \2 \3 \4/'; }
  WID=$("$(vb vantage-remote)" list 2>/dev/null | grep "Vantage Wayland Test" \
        | tail -1 | cut -f1)
  if [ -n "$WID" ]; then
    "$(vb vantage-remote)" fullscreen "$WID" >/dev/null 2>&1
    sleep 1.0
    FG2=$(wgeo_fs); read -r FX1 FY1 FW1 FH1 <<< "$FG2"
    if [ "$FX1" = "0" ] && [ "$FY1" = "0" ] && [ "$FW1" = "1024" ] && \
       [ "$FH1" = "768" ]; then
      ok "fullscreen: window covers the whole output (taskbar fullscreen works)"
    else
      bad "fullscreen geometry wrong: $FG2 (want 0 0 1024 768)"
    fi
    # edge-drag at the SCREEN edge must NOT resize a fullscreen window
    ti "motion x=1020 y=400"
    sleep 0.1
    ti "press b=1"
    sleep 0.1
    ti "motion x=920 y=400"
    sleep 0.15
    ti "release b=1"
    sleep 0.3
    FG3=$(wgeo_fs); read -r FX2 FY2 FW2 FH2 <<< "$FG3"
    if [ "$FW2" = "1024" ] && [ "$FH2" = "768" ] && [ "$FX2" = "0" ] && \
       [ "$FY2" = "0" ]; then
      ok "fullscreen: screen-edge drag does NOT resize (no resize hitbox)"
    else
      bad "fullscreen edge-drag resized the window: $FG3"
    fi
    # pixel proof: the client color (0xff5a9a3a) fills the screen incl.
    # the panel band — no SSD/black/blue regions anywhere
    rm -f /tmp/vantage-wayland.ppm
    kill -USR1 "$WM_PID" 2>/dev/null
    wait_ppm || true
    if [ -s /tmp/vantage-wayland.ppm ]; then
      FS_PX=$(python3 - <<'PYFS'
data = open('/tmp/vantage-wayland.ppm','rb').read()
parts = data.split(b'\n',3)
w,h = map(int,parts[1].split())
pix = parts[3]
# sample a grid: all four edges BELOW the panel band (DE policy: the
# top-layer panel stays visible over fullscreen windows) and the body;
# skip a small cursor neighborhood (the sprite blends over content by
# design)
PANEL_H = 45
bad_px = 0; total = 0
for y in list(range(PANEL_H, h, 48)) + [h-2, PANEL_H+1]:
    for x in range(0,w,32):
        if 880 <= x <= 970 and 360 <= y <= 440:   # cursor at 920,400
            continue
        i = (y*w+x)*3
        r,g,b = pix[i], pix[i+1], pix[i+2]
        total += 1
        # client color 0x5a9a3a
        if not (70 <= r <= 105 and 140 <= g <= 170 and 45 <= b <= 70):
            bad_px += 1
print(f"{bad_px} {total}")
PYFS
)
      NBAD=$(echo "$FS_PX" | cut -d' ' -f1); NTOT=$(echo "$FS_PX" | cut -d' ' -f2)
      if [ "${NBAD:-999}" -le $(( ${NTOT:-0} / 10 )) ]; then
        ok "fullscreen: app pixels own the whole screen (no decoration/resize regions)"
      else
        bad "fullscreen: $NBAD/$NTOT sampled pixels are not app content"
      fi
    else
      bad "no frame dump for the fullscreen pixel check"
    fi
    "$(vb vantage-remote)" unfullscreen "$WID" >/dev/null 2>&1
    sleep 0.6
    FG4=$(wgeo_fs); read -r FX4 FY4 FW4 FH4 <<< "$FG4"
    # no longer fullscreen-covering, position restored (not 0,0 full)
    if [ "$FX4" != "0" ] || [ "$FY4" != "0" ] || [ "$FW4" != "1024" ] || \
       [ "$FH4" != "768" ]; then
      ok "unfullscreen leaves fullscreen state (pos ${FX4},${FY4} size ${FW4}x${FH4})"
    else
      bad "unfullscreen did nothing: $FG4"
    fi
    kill $FS_PID 2>/dev/null
    wait $FS_PID 2>/dev/null
  else
    bad "fullscreen check: testclient id not found"
  fi
}

# resize cursor over the east edge: the double-arrow sprite (white
# shaft pixels) replaces the arrow while hovering the resize zone
RG=$(wgeo); read -r X Y WD H <<< "$RG"
ti "motion x=$((X + WD - 1)) y=$((Y + H / 2))"
sleep 0.3
rm -f /tmp/vantage-wayland.ppm
kill -USR1 "$WM_PID" 2>/dev/null
wait_ppm || true
if [ -s /tmp/vantage-wayland.ppm ]; then
  python3 - /tmp/vantage-wayland.ppm "$X" "$Y" "$WD" "$H" <<'PYCUR'
import sys
ppm, X, Y, W, H = sys.argv[1], *map(int, sys.argv[2:6])
data = open(ppm, 'rb').read()
vals, pos = [], data.find(b'P6') + 2
while len(vals) < 3:
    while data[pos:pos+1].isspace(): pos += 1
    j = pos
    while not data[j:j+1].isspace(): j += 1
    vals.append(int(data[pos:j])); pos = j
pos += 1
w, h, _ = vals
pix = data[pos:]
cx, cy = X + W - 1, Y + H // 2
white = 0
for y in range(max(0, cy - 10), min(h, cy + 10)):
    for x in range(max(0, cx - 10), min(w, cx + 10)):
        i = (y * w + x) * 3
        if pix[i] > 230 and pix[i+1] > 230 and pix[i+2] > 230:
            white += 1
sys.exit(0 if white >= 25 else 1)
PYCUR
  [ $? -eq 0 ] && ok "resize cursor visible over the frame edge (discoverable affordance)" \
    || bad "no resize cursor over the frame edge"
else
  bad "no frame dump for the resize-cursor check"
fi

# continuous geometry feed during a drag (~30 fps throttle): the pager
# follows the drag live instead of catching up on release
RG=$(wgeo); read -r X Y WD H <<< "$RG"
GEOFLOW=$(python3 - "$X" "$Y" "$WD" <<'PYFLOW'
import os, socket, struct, sys, time
# The WM's IPC socket (VT_IPC_DEFAULT_SOCKET, the same endpoint
# vantage-remote connects to) — NOT a wayland-N display socket.
# Speaking the VT-IPC protocol to the compositor's display socket gets
# the connection killed as a broken Wayland client, and a closed
# socket then hot-spins in recv()==b'' below (observed as a 60 s
# harness timeout with the WM idle and healthy).
sock = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"),
                    "vantage.sock")
if not os.path.exists(sock):
    print("geo-flow: no vantage.sock IPC socket")
    sys.exit(1)
MAGIC, SUB, TIN = 0x56544352, 0x0005, 0x0031
TYPE_REQ, TYPE_RESP = 1, 2
X, Y, W = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
events = b""
def req(msg_id, payload=b""):
    """send a request; return the response, STASHING every event that
    interleaves ahead of it (they are the thing we are counting)"""
    global events
    s.sendall(struct.pack("<IIII", MAGIC, msg_id, TYPE_REQ,
                          len(payload)) + payload)
    buf = b""
    s.settimeout(5)
    while True:
        while len(buf) >= 16:
            magic, mid, mtype, plen = struct.unpack("<IIII", buf[:16])
            if magic != MAGIC:
                buf = buf[1:]
                continue
            if len(buf) < 16 + plen:
                break
            msg = buf[16:16 + plen]
            buf = buf[16 + plen:]
            if mtype == TYPE_RESP:
                return msg
            events += msg
        chunk = s.recv(65536)
        if not chunk:
            # EOF: the server closed the connection. recv() returning
            # b'' is NOT an error and would otherwise hot-spin here
            # forever — the 5 s socket timeout never fires on it.
            raise RuntimeError("IPC server closed the connection")
        buf += chunk
req(SUB)                      # subscribe (ack consumed by req)
req(TIN, b"spec=motion x=%d y=%d" % (X + W // 2, Y - 14))
req(TIN, b"spec=press b=1")
t0 = time.time()
motions = 0
nextm = t0
try:
    while time.time() - t0 < 2.0:
        now = time.time()
        if now < nextm:
            time.sleep(0.002)
            continue
        nextm = now + 0.012
        t = (time.time() - t0) / 2.0
        req(TIN, ("spec=motion x=%d y=%d" %
                  (X + W // 2 + int(220 * t), Y - 14 + int(60 * t))).encode())
        motions += 1
finally:
    # ALWAYS release: a wedged compositor with a stuck grab would
    # wedge every later check too
    try:
        req(TIN, b"spec=release b=1")
    except Exception:
        pass
geo = events.count(b"window-geometry")
print(f"geo-flow: motions={motions} geometry={geo}")
sys.exit(0 if motions > 60 and 35 <= geo <= 75 else 1)
PYFLOW
)
if [ $? -eq 0 ]; then
  ok "geometry events flow continuously during a drag ($GEOFLOW — the pager's live feed)"
else
  bad "geometry feed not continuous during drag: $GEOFLOW"
fi
kill "$RSZ_PID" 2>/dev/null
wait "$RSZ_PID" 2>/dev/null

# ===================================================================
# 0.3.5 regression suite — the user-reported desktop bugs, pinned with
# REAL-toolkit probes (the synthetic testclient acks configures
# blindly; GTK4 does not — it exposed all of these):
#   1. native Wayland resize actually applies (xdg configure handshake)
#   2. maximize keeps the top panel visible (workarea + SSD inset)
#   3. the Start Menu scrolls (seat protocol + value120 wheel encoding)
#   4. XWayland windows: serial-based surface association (no swaps)
#   5. XWayland SSD close/maximize buttons work (no NULL deref crash)
# ===================================================================
echo "== harness-wayland: REAL GTK4 client — configure handshake =="

# generic WM-model geometry by window title (wgeo above is hardcoded
# to the testclient's title)
wgeo_t() { "$(vb vantage-remote)" list 2>/dev/null \
  | awk -F'\t' -v t="$1" '$2==t{print $7,"\t",$8,"\t",$9,"\t",$10; exit}'; }

# --- helper: dump + panel-band check (the bar must stay visible)
panel_band_px() {  # prints the panel bar's pixel count in the top band
python3 - <<'PYPB'
import sys
try:
    with open('/tmp/vantage-wayland.ppm','rb') as f: d=f.read()
    v=[]; i=d.find(b'P6')+2
    while len(v)<3:
        while d[i:i+1].isspace(): i+=1
        j=i
        while not d[j:j+1].isspace(): j+=1
        v.append(int(d[i:j])); i=j
    i+=1
    w,h=v[0],v[1]; px=d[i:i+w*h*3]
    bar=0
    for y in range(2,40):
        for x in range(0,w,2):
            k=(y*w+x)*3
            if abs(px[k]-0x16)<=8 and abs(px[k+1]-0x18)<=8 and abs(px[k+2]-0x1c)<=8:
                bar+=1
    print(bar)
except Exception:
    print(0)
PYPB
}

GTK_LOG="$WORK/gtk-probe.log"
if [ -x "$(tc vt-gtk4-probe)" ]; then
  "$(tc vt-gtk4-probe)" --label "GtkProbe" > "$GTK_LOG" 2>&1 &
  GTK_PID=$!
  for i in $(seq 1 80); do
    grep -q "^mapped" "$GTK_LOG" 2>/dev/null && break
    kill -0 "$GTK_PID" 2>/dev/null || break
    sleep 0.1
  done
  sleep 1.2
  GP=$(wgeo_t "GtkProbe")
  read -r GX GY GW GH <<< "$(echo "$GP" | tr -d '\t')"
  if [ -n "$GW" ] && [ "$GW" -gt 300 ]; then
    ok "real GTK4 window mapped ${GW}x${GH}"
  else
    bad "real GTK4 window geometry wrong: '$GP'"
  fi

  # --- 1. interactive resize through the REAL input path: the model
  #     grows AND the toolkit actually allocates the new size (the old
  #     incomplete configure handshake made resizes phantom)
  ti "motion x=$((GX + GW / 2)) y=$((GY + GH / 2))"
  ti "keydown Super_L"; ti "press b=3"; sleep 0.1
  ti "motion x=$((GX + GW / 2 + 190)) y=$((GY + GH / 2 + 110))"; sleep 0.4
  ti "release b=3"; ti "keyup Super_L"
  sleep 1.2
  GP2=$(wgeo_t "GtkProbe")
  read -r GX2 GY2 GW2 GH2 <<< "$(echo "$GP2" | tr -d '\t')"
  NSIZE=$(grep -cE "^size" "$GTK_LOG")
  if [ "${GW2:-0}" -gt $((GW + 90)) ] 2>/dev/null && [ "$NSIZE" -ge 2 ]; then
    ok "GTK4 resize APPLIED (model ${GW}x${GH} -> ${GW2}x${GH2}, $(grep -E '^size' "$GTK_LOG" | tail -1))"
  else
    bad "GTK4 resize phantom (model ${GW}x${GH} -> ${GW2:-?}x${GH2:-?}, size lines=$NSIZE)"
  fi

  # --- 2. maximize through the TASKBAR path (vantage-remote maximize
  #     -> WM -> backend): workarea placement, panel stays visible, the
  #     toolkit applies it
  GID=$("$(vb vantage-remote)" list 2>/dev/null | awk -F'\t' '$2=="GtkProbe"{print $1; exit}')
  "$(vb vantage-remote)" maximize "$GID" >/dev/null 2>&1
  sleep 1.5
  GP3=$(wgeo_t "GtkProbe")
  read -r GX3 GY3 GW3 GH3 <<< "$(echo "$GP3" | tr -d '\t')"
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || true
  sleep 0.1
  BARPX=$(panel_band_px)
  if [ "${GW3:-0}" -gt 900 ] 2>/dev/null && [ "${GY3:-999}" -ge 44 ] && \
     [ "${BARPX:-0}" -gt 500 ]; then
    ok "maximize kept the panel visible (client ${GW3}x${GH3} at +${GX3}+${GY3}, bar-px=$BARPX)"
  else
    bad "maximize wrong: ${GW3:-?}x${GH3:-?} at +${GX3:-?}+${GY3:-?}, bar-px=${BARPX:-?}"
  fi
  # unmaximize restores the pre-maximize size (saved geometry)
  "$(vb vantage-remote)" unmaximize "$GID" >/dev/null 2>&1
  sleep 1.2
  GP4=$(wgeo_t "GtkProbe")
  read -r GX4 GY4 GW4 GH4 <<< "$(echo "$GP4" | tr -d '\t')"
  if [ "${GW4:-0}" -gt 300 ] && [ "${GW4:-999}" -lt 800 ]; then
    ok "unmaximize restored window size (${GW4}x${GH4})"
  else
    bad "unmaximize wrong: ${GW4:-?}x${GH4:-?}"
  fi

  # --- 2b. CSD shadow composites — the "black region" regression.
  #     The probe is a REAL GTK4 CSD window (headerbar + client-side
  #     shadow margins in the buffer, input region/window geometry
  #     excluding them). Pre-blend-fix the raw blit stamped the
  #     premultiplied shadow fringe OPAQUELY into the XRGB fb: solid
  #     near-black bands framed the window (mirage's "black resize
  #     region"). Now the buffer is alpha-blended: the strip just
  #     OUTSIDE the content rect must show background (navy gradient,
  #     shadow-darkened) — not black — and the content itself must be
  #     the probe's magenta.
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || true
  sleep 0.1
  python3 - "$GX4" "$GY4" "$GW4" "$GH4" <<'PYEOF6'
import sys
gx, gy, gw, gh = (int(v) for v in sys.argv[1:5])
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
    i = (y*w + x)*3
    return (pix[i], pix[i+1], pix[i+2])
def blackish(c):
    return c[0] <= 10 and c[1] <= 10 and c[2] <= 10
# strips just outside the content rect (shadow band), window's
# vertical middle (never the rounded corners)
y0, y1 = gy + gh//4, gy + 3*gh//4
band_r = sum(1 for y in range(y0, y1) for x in range(gx+gw+2, gx+gw+10)
             if 0 <= x < w and blackish(px(x, y)))
band_l = sum(1 for y in range(y0, y1) for x in range(gx-9, gx-1)
             if 0 <= x < w and blackish(px(x, y)))
mag = 0
my = gy + gh//2
for x in range(gx+8, gx+gw-8, 2):
    c = px(x, my)
    if abs(c[0]-158) <= 26 and abs(c[1]-46) <= 26 and abs(c[2]-117) <= 26:
        mag += 1
print(f"csd: black-px right={band_r} left={band_l} content-magenta={mag}")
# zero solid-black pixels outside the content rect; the window's own
# row must be dominated by the probe's magenta fill
sys.exit(0 if (band_r == 0 and band_l == 0 and mag > 40) else 1)
PYEOF6
  [ $? -eq 0 ] && ok "CSD shadow blended (no black bands around the window)" \
    || bad "CSD window has black bands (raw blit regression)"

  # --- 2c. WINDOW model = the toolkit's window rect, NOT the CSD
  #     buffer. GTK4 CSD commits a buffer ~28px larger than the
  #     window (shadow margins + client-side resize grips, which GDK
  #     legitimately includes in the surface input region — band
  #     clicks DO reach the client and that is correct). What must
  #     hold: the WM model reports exactly what the toolkit itself
  #     allocated ("size WxH" from the probe) — the buffer rect made
  #     the pager/taskbar over-report every CSD window. A click on
  #     the bare desktop must reach no client at all.
  # the probe prints "size WxH" (one token) — split on the x
  GTK_OWN=$(grep -E "^size " "$GTK_LOG" | tail -1 | \
            sed -E 's/^size ([0-9]+)x([0-9]+)/\1 \2/')
  read -r GTK_W GTK_H <<< "$GTK_OWN"
  if [ -n "$GTK_W" ] && [ "${GW4:-0}" -gt 0 ] && \
     [ "$((GW4 > GTK_W ? GW4 - GTK_W : GTK_W - GW4))" -le 2 ] && \
     [ "$((GH4 > GTK_H ? GH4 - GTK_H : GTK_H - GH4))" -le 2 ]; then
    ok "WM model = toolkit window rect (${GW4}x${GH4} == GTK's ${GTK_W}x${GTK_H}, not the CSD buffer)"
  else
    bad "WM model reports the CSD buffer (model ${GW4:-?}x${GH4:-?} vs GTK's ${GTK_W:-?}x${GTK_H:-?})"
  fi
  PRESSES_BEFORE=$(grep -c "^button 1 press" "$GTK_LOG")
  ti "click $((GX4 + GW4 / 2)),$((GY4 + GH4 / 2))"
  sleep 0.4
  PRESSES_IN=$(grep -c "^button 1 press" "$GTK_LOG")
  RELEASES_IN=$(grep -c "^button 1 release" "$GTK_LOG")
  if [ "${PRESSES_IN:-0}" -gt "${PRESSES_BEFORE:-0}" ] && \
     [ "${RELEASES_IN:-0}" -ge "${PRESSES_IN:-0}" ]; then
    ok "click inside content reached the client (press+release paired)"
  else
    bad "click inside content never reached the client ($PRESSES_BEFORE -> $PRESSES_IN presses, $RELEASES_IN releases)"
  fi
  ti "click 30,700"
  sleep 0.3
  PRESSES_DESK=$(grep -c "^button 1 press" "$GTK_LOG")
  if [ "${PRESSES_DESK:-0}" -eq "${PRESSES_IN:-1}" ]; then
    ok "click on the bare desktop reached no client"
  else
    bad "desktop click leaked to the client ($PRESSES_IN -> $PRESSES_DESK presses)"
  fi

  # --- 1b. PLAIN edge drag on the CSD margin (NO Super): the native
  #     affordance. GTK4 CSD windows grab their own shadow margins and
  #     call xdg_toplevel.resize — the compositor must (a) deliver the
  #     press inside the margin band (input-region aware) and (b) honor
  #     the resize request. A press 6px past the content edge is inside
  #     the margin but outside the window geometry.
  #     Runs AFTER the click/pairing checks on purpose: GDK consumes
  #     the drag-ending release internally (begin_resize), so the
  #     probe's button accounting keeps that press "down" — a later
  #     click would read as a double-press. The wire itself is correct
  #     (verified: enter→press→motion→release all delivered); the
  #     order keeps the strict stray/double canaries meaningful.
  GP2B0=$(wgeo_t "GtkProbe")
  read -r GX2B0 GY2B0 GW2B0 GH2B0 <<< "$(echo "$GP2B0" | tr -d '\t')"
  ti "motion x=$((GX2B0 + GW2B0 + 6)) y=$((GY2B0 + GH2B0 / 2))"
  sleep 0.2
  ti "press b=1"; sleep 0.12
  ti "motion x=$((GX2B0 + GW2B0 + 6 + 90)) y=$((GY2B0 + GH2B0 / 2))"; sleep 0.3
  ti "release b=1"; sleep 1.2
  GP2B=$(wgeo_t "GtkProbe")
  read -r GX2B GY2B GW2B GH2B <<< "$(echo "$GP2B" | tr -d '\t')"
  if [ "${GW2B:-0}" -gt $((GW2B0 + 40)) ] 2>/dev/null; then
    ok "GTK4 CSD EDGE resize works plain (no Super): ${GW2B0}x${GH2B0} -> ${GW2B}x${GH2B}"
  else
    bad "CSD edge drag did not resize: ${GW2B0:-?}x${GH2B0:-?} -> ${GW2B:-?}x${GH2B:-?}"
  fi


  # --- 2d. TASKBAR dropdown (right-click context menu): must stay
  #     open across WM poll cycles (the old full rebuild destroyed
  #     the parent button 2.5x/s — the menu vanished and hover state
  #     flickered), and a LEFT click on its Close row must activate
  #     (the stray-release class could eat the click). Closing the
  #     window through the menu is also the GHOST-WINDOW check: the
  #     model and the taskbar must both drop the window.
  #     The button is located by its TEAL icon (fixture above).
  BTN_X=""
  for try in 1 2 3; do
    rm -f /tmp/vantage-wayland.ppm
    kill -USR1 "$WM_PID" 2>/dev/null
    wait_ppm || continue
    BTN_X=$(python3 - <<'PYEOF7'
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
xs = []
for y in range(4, 44):
    for x in range(0, w):
        i = (y*w + x)*3
        if abs(pix[i]-0x20) <= 26 and abs(pix[i+1]-0x9a) <= 26 \
           and abs(pix[i+2]-0x8a) <= 26:
            xs.append(x)
print(sum(xs)//len(xs) if xs else -1)
PYEOF7
)
    [ "${BTN_X:-0}" -gt 0 ] && break
    sleep 0.4
  done
  if [ "${BTN_X:-0}" -gt 0 ]; then
    ok "taskbar button found for the GTK4 window (icon by app_id, x=$BTN_X)"
    ti "motion x=$BTN_X y=22"
    ti "press b=3"; sleep 0.15; ti "release b=3"
    # span >=3 WM poll cycles (400 ms) with the menu open
    sleep 1.5
    rm -f /tmp/vantage-wayland.ppm
    kill -USR1 "$WM_PID" 2>/dev/null
    wait_ppm || true
    MENU_ROWS=$(python3 - "$BTN_X" <<'PYEOF8'
import sys
bx = int(sys.argv[1])
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
# light text rows below the bar, near the button's column
rows = {}
for y in range(50, 170):
    light = 0
    for x in range(max(0, bx-40), min(w, bx+140)):
        i = (y*w + x)*3
        if pix[i] >= 0x90 and pix[i+1] >= 0x90 and pix[i+2] >= 0x90:
            light += 1
    rows[y] = light
# cluster consecutive rows with text into bands
bands, cur = [], None
for y in sorted(rows):
    if rows[y] >= 2:
        if cur and y - cur[1] <= 3:
            cur[1] = y
        else:
            cur = [y, y]
            bands.append(cur)
    else:
        cur = None
real = [b for b in bands if b[1] - b[0] >= 4]
if len(real) >= 2:
    # Close is the second row: print its center
    print(f"{(real[1][0]+real[1][1])//2}")
elif len(real) == 1:
    print(f"{(real[0][0]+real[0][1])//2}")
else:
    print("-1")
PYEOF8
)
    if [ "${MENU_ROWS:-0}" -gt 0 ]; then
      ok "taskbar dropdown stayed open across poll cycles (no rebuild flicker)"
      # LEFT click on the Close row (second text band); the walk
      # brackets the scan result with a generous spread (the menu's
      # row pitch follows the machine's font metrics)
      CLOSED=""
      for y in "$MENU_ROWS" "$((MENU_ROWS+6))" "$((MENU_ROWS-6))" \
               "$((MENU_ROWS+12))" "$((MENU_ROWS-12))" 114 104 94; do
        [ -z "$y" ] && continue
        [ "$y" -gt 0 ] || continue
        [ -n "$CLOSED" ] && break
        ti "click $((BTN_X + 30)),$y"
        for i in $(seq 1 15); do
          [ -z "$(wgeo_t "GtkProbe")" ] && { CLOSED=1; break; }
          sleep 0.1
        done
      done
      if [ -n "$CLOSED" ]; then
        ok "dropdown Close item closed the window on left click"
        # ghost check, part 2: the model stays clean 2s later
        sleep 2
        if [ -z "$(wgeo_t "GtkProbe")" ]; then
          ok "no ghost window in the WM model after close"
        else
          bad "ghost window: model still lists GtkProbe after close"
        fi
        # ghost check, part 3: the taskbar BUTTON is gone (teal icon
        # vanished from the bar — dead entries were the user report)
        sleep 0.5
        rm -f /tmp/vantage-wayland.ppm
        kill -USR1 "$WM_PID" 2>/dev/null
        wait_ppm || true
        TEAL_LEFT=$(python3 - <<'PYEOF9'
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
n = sum(1 for y in range(4, 44) for x in range(0, w)
        if abs(pix[(y*w+x)*3]-0x20) <= 26
        and abs(pix[(y*w+x)*3+1]-0x9a) <= 26
        and abs(pix[(y*w+x)*3+2]-0x8a) <= 26)
print(n)
PYEOF9
)
        if [ "${TEAL_LEFT:-1}" -eq 0 ]; then
          ok "taskbar button removed after close (no ghost entry)"
        else
          bad "taskbar button survived the window close (ghost entry)"
        fi
      else
        bad "dropdown Close item did not close the window (left-click eaten)"
      fi
    else
      bad "taskbar dropdown did not stay open (rebuild/destroy regression)"
    fi
  else
    bad "taskbar button with teal icon not found (icon lookup by app_id?)"
  fi

  # --- 2e. button-event accounting: after ALL gestures above (the
  #     compositor-consumed Super+drag, SSD-style frame clicks,
  #     forwarded clicks, band clicks, dropdown interaction) the
  #     client must have seen ZERO stray releases and ZERO double
  #     presses — the implicit-grab contract (foot's "stray button
  #     release event (compositor bug?)" class).
  STRAY=$(grep -c "STRAY-RELEASE" "$GTK_LOG")
  DBLP=$(grep -c "DOUBLE-PRESS" "$GTK_LOG")
  if [ "${STRAY:-0}" -eq 0 ] && [ "${DBLP:-0}" -eq 0 ]; then
    ok "no stray releases / double presses across all gestures"
  else
    bad "input pairing broken: stray-releases=$STRAY double-presses=$DBLP"
  fi

  kill "$GTK_PID" 2>/dev/null
  wait "$GTK_PID" 2>/dev/null
  # window already closed through the dropdown → the app exits on its
  # own; if it is still alive (dropdown path failed), the kill is the
  # cleanup. Either way the model must not keep a stale entry.
  sleep 0.6
  if [ -n "$(wgeo_t "GtkProbe")" ]; then
    bad "window still in model after app exit (ghost)"
  fi
else
  echo "  (vt-gtk4-probe not built — real-toolkit checks skipped)"
fi

# --- 3. Start Menu scrolling: open the menu (61 entries in
#     Accessories), wheel-scroll 15 detents over the app list, verify
#     the content moved (region change or displacement — a hover only
#     touches ~3% of the region, a real scroll swaps the row text)
echo "== harness-wayland: Start Menu scrolling =="
ti "click 60,22"
MENU_OPEN2=""
for t in 1 2 3 4 5 6; do
  sleep 0.5
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || continue
  L=$(python3 -c '
try:
    with open("/tmp/vantage-wayland.ppm","rb") as f: d=f.read()
    v=[]; i=d.find(b"P6")+2
    while len(v)<3:
        while d[i:i+1].isspace(): i+=1
        j=i
        while not d[j:j+1].isspace(): j+=1
        v.append(int(d[i:j])); i=j
    i+=1; w,h=v[0],v[1]; px=d[i:i+w*h*3]
    n=0
    for y in range(60,400,2):
        for x in range(8,600,2):
            k=(y*w+x)*3
            if px[k]>=0x90 and px[k+1]>=0x90 and px[k+2]>=0x90: n+=1
    print(n)
except Exception: print(0)')
  [ "${L:-0}" -gt 300 ] 2>/dev/null && { MENU_OPEN2=1; break; }
done
if [ -n "$MENU_OPEN2" ]; then
  cp /tmp/vantage-wayland.ppm "$WORK/menu-before.ppm"
  ti "motion x=300 y=200"
  for i in $(seq 1 15); do ti "axis d=1"; sleep 0.05; done
  sleep 0.5
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || true
  SCROLL_VERDICT=$(python3 - "$WORK/menu-before.ppm" <<'PYMS'
import sys
def load(p):
    with open(p,'rb') as f: d=f.read()
    v=[]; i=d.find(b'P6')+2
    while len(v)<3:
        while d[i:i+1].isspace(): i+=1
        j=i
        while not d[j:j+1].isspace(): j+=1
        v.append(int(d[i:j])); i=j
    i+=1
    return v[0],v[1],d[i:i+v[0]*v[1]*3]
try:
    w,h,a=load(sys.argv[1])
    with open('/tmp/vantage-wayland.ppm','rb') as f: d=f.read()
    v=[]; i=d.find(b'P6')+2
    while len(v)<3:
        while d[i:i+1].isspace(): i+=1
        j=i
        while not d[j:j+1].isspace(): j+=1
        v.append(int(d[i:j])); i=j
    i+=1; _,_,b=w,h,d[i:i+w*h*3]
    ya=[sum(a[(y*w+x)*3] for x in range(60,560,4)) for y in range(80,460)]
    yb=[sum(b[(y*w+x)*3] for x in range(60,560,4)) for y in range(80,460)]
    best,bestd=0,1e18
    for shift in range(-800,801,2):
        s=0; n=0
        for i2 in range(len(ya)):
            j=i2+shift
            if 0<=j<len(yb): s+=abs(ya[i2]-yb[j]); n+=1
        if n>50:
            dd=s/n
            if dd<bestd: bestd=dd; best=shift
    tot=0; dif=0
    for y in range(180,460,2):
        for x in range(200,560,2):
            k=(y*w+x)*3
            tot+=1
            if a[k:k+3]!=b[k:k+3]: dif+=1
    frac=dif/tot if tot else 0
    print(f"{best}|{frac:.3f}")
    sys.exit(0 if (abs(best)>=40 or frac>0.10) else 1)
except Exception as e:
    print(f"ERR|{e}"); sys.exit(1)
PYMS
)
  RC=$?
  ti "motion x=12 y=740"; sleep 0.3; ti "click 12,740"; sleep 0.4
  if [ $RC -eq 0 ]; then
    ok "Start Menu scrolls ($SCROLL_VERDICT)"
  else
    bad "Start Menu did not scroll ($SCROLL_VERDICT)"
  fi
else
  bad "Start Menu never opened for the scrolling check"
  ti "motion x=12 y=740" 2>/dev/null; sleep 0.2
fi

# --- 4/5. XWayland: serial-based association + working SSD buttons
echo "== harness-wayland: XWayland association + SSD buttons =="
if [ -n "$XWL_DISPLAY1" ] && [ -n "$XWL_AUTH1" ]; then
  # racing windows: leader + two toplevels in one burst — a recency
  # guess swaps their surfaces (each window renders at ANOTHER's
  # geometry); the serial match must land each on its own
  RACE_LOG="$WORK/race.log"
  DISPLAY="$XWL_DISPLAY1" XAUTHORITY="$XWL_AUTH1" \
    "$(tc vt-x11-testclient)" --seconds 8 --race-probe > "$RACE_LOG" 2>&1 &
  RACE_PID=$!
  for i in $(seq 1 40); do
    grep -q "^mapped" "$RACE_LOG" 2>/dev/null && break
    kill -0 "$RACE_PID" 2>/dev/null || break
    sleep 0.1
  done
  sleep 1.2
  RACE_MODEL=$(wgeo_t "RaceA")
  read -r RAX RAY RAW RAH <<< "$(echo "$RACE_MODEL" | tr -d '\t')"
  rm -f /tmp/vantage-wayland.ppm
  kill -USR1 "$WM_PID" 2>/dev/null
  wait_ppm || true
  RACE_OK=$(python3 <<'PYRC'
import sys
with open('/tmp/vantage-wayland.ppm','rb') as f: d=f.read()
v=[]; i=d.find(b'P6')+2
while len(v)<3:
    while d[i:i+1].isspace(): i+=1
    j=i
    while not d[j:j+1].isspace(): j+=1
    v.append(int(d[i:j])); i=j
i+=1
w,h=v[0],v[1]; px=d[i:i+w*h*3]
def near(r,g,b,R,G,B,t=10): return abs(r-R)<=t and abs(g-G)<=t and abs(b-B)<=t
def blob(R,G,B):
    xs,ys=[],[]
    for y in range(0,h,2):
        for x in range(0,w,2):
            k=(y*w+x)*3
            if near(px[k],px[k+1],px[k+2],R,G,B): xs.append(x); ys.append(y)
    if not xs: return None
    return (max(xs)-min(xs), max(ys)-min(ys), (min(xs)+max(xs))//2)
a=blob(0x3a,0x5f,0x9a); b=blob(0x9a,0x3a,0x5f)
ok = bool(a and b) and a[0]>420 and a[1]>330 and b[0]>220 and b[1]>140 and a[2]<520 and b[2]>600
print(f"A={a} B={b}")
sys.exit(0 if ok else 1)
PYRC
)
  if [ $? -eq 0 ] && [ "${RAW:-0}" -eq 500 ]; then
    ok "racing XWayland windows pair correctly ($RACE_OK)"
  else
    bad "racing XWayland windows swapped (model '$RACE_MODEL', $RACE_OK)"
  fi
  wait "$RACE_PID" 2>/dev/null

  # SSD buttons on an XWayland window: close must not crash the
  # compositor (NULL xdg resource) and must actually close the window
  XBTN_LOG="$WORK/xwl-btn.log"
  DISPLAY="$XWL_DISPLAY1" XAUTHORITY="$XWL_AUTH1" \
    "$(tc vt-x11-testclient)" --seconds 14 --title "XwlBtn" > "$XBTN_LOG" 2>&1 &
  XBTN_PID=$!
  for i in $(seq 1 40); do
    grep -q "^mapped" "$XBTN_LOG" 2>/dev/null && break
    kill -0 "$XBTN_PID" 2>/dev/null || break
    sleep 0.1
  done
  sleep 0.8
  XB=$(wgeo_t "XwlBtn")
  read -r XX XY XW XH <<< "$(echo "$XB" | tr -d '\t')"
  if [ -n "$XW" ] && [ "$XW" -gt 100 ]; then
    XFX=$((XX - 2)); XFY=$((XY - 28)); XFW=$((XW + 4))
    ti "click $((XFX + XFW - 2 - 22 + 10)),$((XFY + 13))"
    sleep 1.5
    if kill -0 "$WM_PID" 2>/dev/null; then
      ok "compositor survived XWayland SSD close-click"
      if [ -z "$(wgeo_t "XwlBtn")" ]; then
        ok "XWayland window closed via the SSD × button"
      else
        bad "XWayland window survived the SSD × button"
        XID=$(wgeo_t "XwlBtn" | awk -F'\t' '{print $1}')
        [ -n "$XID" ] && "$(vb vantage-remote)" close "$XID" >/dev/null 2>&1
      fi
    else
      bad "compositor CRASHED on XWayland SSD close-click (regression)"
      # restart the world so later phases still run
      wait "$XBTN_PID" 2>/dev/null
    fi
  else
    bad "XWayland button-probe window missing: '$XB'"
    kill "$XBTN_PID" 2>/dev/null
  fi
  wait "$XBTN_PID" 2>/dev/null

  # XWayland maximize via the SSD button: the frame fills the WORKAREA
  # (panel visible, client top below the panel + title band)
  XBM_LOG="$WORK/xwl-max.log"
  DISPLAY="$XWL_DISPLAY1" XAUTHORITY="$XWL_AUTH1" \
    "$(tc vt-x11-testclient)" --seconds 12 --title "XwlMax" > "$XBM_LOG" 2>&1 &
  XBM_PID=$!
  for i in $(seq 1 40); do
    grep -q "^mapped" "$XBM_LOG" 2>/dev/null && break
    kill -0 "$XBM_PID" 2>/dev/null || break
    sleep 0.1
  done
  sleep 0.8
  XM=$(wgeo_t "XwlMax")
  read -r MX MY MW MH <<< "$(echo "$XM" | tr -d '\t')"
  if [ -n "$MW" ] && [ "$MW" -gt 100 ]; then
    MFX=$((MX - 2)); MFY=$((MY - 28)); MFW=$((MW + 4))
    ti "click $((MFX + MFW - 2 - 22 - 24 + 11)),$((MFY + 13))"
    sleep 1.2
    XM2=$(wgeo_t "XwlMax")
    read -r MX2 MY2 MW2 MH2 <<< "$(echo "$XM2" | tr -d '\t')"
    rm -f /tmp/vantage-wayland.ppm
    kill -USR1 "$WM_PID" 2>/dev/null
    wait_ppm || true
    BARPX2=$(panel_band_px)
    if [ "${MW2:-0}" -gt 900 ] && [ "${MY2:-999}" -ge 70 ] && [ "${BARPX2:-0}" -gt 500 ]; then
      ok "XWayland maximize fills the workarea, panel visible (${MW2}x${MH2} at +${MX2}+${MY2})"
    else
      bad "XWayland maximize wrong: ${MW2:-?}x${MH2:-?} at +${MX2:-?}+${MY2:-?} bar-px=${BARPX2:-?}"
    fi
    XID2=$(wgeo_t "XwlMax" | awk -F'\t' '{print $1}')
    [ -n "$XID2" ] && "$(vb vantage-remote)" close "$XID2" >/dev/null 2>&1
  else
    bad "XWayland maximize-probe window missing: '$XM'"
  fi
  wait "$XBM_PID" 2>/dev/null
else
  bad "Xwayland not available for the association/button checks"
fi

# ------------------------------------------------- real-app protocol check
# foot is the reference native Wayland terminal: run it briefly and
# verify the compositor implements the protocols it PROBES for. Three
# warnings are strict failures (activation, fractional scaling,
# server-side cursors). The toplevel-icon warning is only strict in
# foot >= 1.23 wording ("xdg-toplevel-icon"); foot <= 1.22 matched the
# WRONG global name upstream (fixed in their 1.23) and warns on every
# compositor — that wording ("XDG toplevel icon") is informational.
echo "== harness-wayland: foot protocol warnings (real app) =="
FOOT_BIN=""
FOOT_ENV=""
if command -v foot >/dev/null 2>&1; then
  FOOT_BIN="foot"
elif [ -x /tmp/foot-root/usr/bin/foot ]; then
  FOOT_BIN="/tmp/foot-root/usr/bin/foot"
  FOOT_ENV="LD_LIBRARY_PATH=/tmp/foot-root/usr/lib/x86_64-linux-gnu"
fi
if [ -n "$FOOT_BIN" ]; then
  FOOT_LOG="$WORK/foot.log"
  env $FOOT_ENV TERM=xterm-256color timeout 5 "$FOOT_BIN" > "$FOOT_LOG" 2>&1
  NMAPPED=$(grep -cE "window 0x[0-9]+ 'foot' mapped" "$WORK/wm.log" || true)
  if [ "${NMAPPED:-0}" -lt 1 ]; then
    bad "foot never mapped a window (protocol checks below would be vacuous): $(tail -3 "$FOOT_LOG")"
  else
    ok "foot mapped its window ($NMAPPED) and ran to completion"
  fi
  if grep -q "does not implement XDG activation" "$FOOT_LOG"; then
    bad "foot still warns: XDG activation missing"
  else
    ok "foot: xdg-activation implemented (no warning)"
  fi
  if grep -q "does not implement fractional scaling" "$FOOT_LOG"; then
    bad "foot still warns: fractional scaling missing"
  else
    ok "foot: fractional-scale implemented (no warning)"
  fi
  if grep -q "does not implement server-side cursors" "$FOOT_LOG"; then
    bad "foot still warns: server-side cursors missing"
  else
    ok "foot: cursor-shape implemented (no warning)"
  fi
  if grep -q "does not implement the xdg-toplevel-icon protocol" "$FOOT_LOG"; then
    bad "foot (>=1.23, correct check) still warns: toplevel-icon missing"
  elif grep -q "does not implement the XDG toplevel icon protocol" "$FOOT_LOG"; then
    ok "foot <=1.22 known upstream detection bug (toplevel icon) — informational"
  else
    ok "foot: xdg-toplevel-icon implemented (no warning)"
  fi
  if grep -qiE "segv|assert|aborted" "$FOOT_LOG"; then
    bad "foot crashed against the compositor"
  fi
else
  echo "  (foot not installed — real-app protocol check skipped)"
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

# ------------------------------------------------- session children
# The session manager supervises the panel as a real child (the panel
# is no longer compositor code). The session-spawned compositor ALSO
# runs Xwayland — an X11 app launched through it must map and become
# a taskbar-visible window in the WM model.
grep -q "started 'panel'" "$SESS_LOG" \
  && ok "session spawned the GTK4 panel child" \
  || bad "session did not spawn the panel child"
XWL_DISPLAY=$(grep -o 'xwayland: ready — DISPLAY=:[0-9]*' "$SESS_LOG" \
              | head -1 | grep -o ':[0-9]*$')
if [ -n "$XWL_DISPLAY" ]; then
  ok "Xwayland running under the session (DISPLAY=$XWL_DISPLAY)"
  XWL_LOG="$WORK/xwl-app.log"
  XAUTH_FILE=$(grep -o 'auth [^ )]*' "$SESS_LOG" | head -1 | cut -d' ' -f2)
  # the client must stay ALIVE while we query the WM list: the old flow
  # waited for its exit, the windows were destroyed and the list was
  # empty by definition. Background it, poll, then check.
  timeout 10 env DISPLAY="$XWL_DISPLAY" XAUTHORITY="$XAUTH_FILE" \
    "$(tc vt-x11-testclient)" --title "XwlProbe" --seconds 8 \
    > "$XWL_LOG" 2>&1 &
  XWL_PID=$!
  for i in $(seq 1 60); do
    grep -q "^mapped" "$XWL_LOG" 2>/dev/null && break
    kill -0 "$XWL_PID" 2>/dev/null || break
    sleep 0.1
  done
  if grep -q "^connected" "$XWL_LOG" && grep -q "^mapped" "$XWL_LOG"; then
    ok "X11 app launched through Xwayland (connected + mapped)"
  else
    bad "X11 app did not launch through Xwayland: $(tail -3 "$XWL_LOG")"
  fi
  WM_LIST=$(WAYLAND_DISPLAY="$SOCK2" "$(vb vantage-remote)" list 2>/dev/null \
            | grep -c "XwlProbe")
  [ "${WM_LIST:-0}" -ge 1 ] \
    && ok "Xwayland window visible to the WM/taskbar model" \
    || bad "Xwayland window missing from the WM window list"
  kill "$XWL_PID" 2>/dev/null
  wait "$XWL_PID" 2>/dev/null
else
  bad "Xwayland did not start under the session"
fi

# ------------------------------------------- session panel coverage
# The session's panel must actually DOCK — not just spawn. On a fast
# machine the harness used to log out before the panel's layer
# surface ever mapped (observed: the whole session phase ran in
# 0.4 s while the panel needs ~2 s to its first frame), so the
# logout round-trip below never exercised a docked panel at all.
SESS_PANEL_MAPPED=""
for i in $(seq 1 100); do
  grep -q "layer surface 'vantage-panel' mapped" "$SESS_LOG" 2>/dev/null \
    && { SESS_PANEL_MAPPED=1; break; }
  kill -0 "$SESS_PID" 2>/dev/null || break
  sleep 0.1
done
[ -n "$SESS_PANEL_MAPPED" ] \
  && ok "session panel docked through the compositor (layer surface mapped)" \
  || bad "session panel never docked its layer surface"

# ------------------------------------------- panel session-menu logout
# The REAL user path: the session menu button → Log Out row → the
# confirm view INSIDE the popover. The old code opened a GtkAlertDialog
# — a modal xdg toplevel transient for a layer-shell parent that has
# no xdg_toplevel, so it NEVER MAPPED: clicking Log Out did nothing
# ("i cant log out of the DE in wayland"). The confirm view is pinned
# by driving the actual pixels: open the menu, click the first action
# row, click the blue confirm button, expect the session to END.
echo "== harness-wayland: panel session-menu logout (user path) =="
UI_LOGOUT_CLICKED=""
if [ -n "${SESS_PID:-}" ] && kill -0 "$SESS_PID" 2>/dev/null; then
  WM_CHILD2=$(grep -o "started 'wm' pid=[0-9]*" "$SESS_LOG" 2>/dev/null \
              | head -1 | cut -d= -f2)
  if [ -n "${WM_CHILD2:-}" ] && kill -0 "$WM_CHILD2" 2>/dev/null; then
    # detection helpers, polled with fresh frame dumps: under ASan the
    # panel renders 5-10x slower, and a fixed sleep photographed an
    # empty popover (the same lesson the Programs-menu check learned)
    cat > "$WORK/find-row.py" <<'PYSESS1'
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
rows = []
for y in range(44, 280):
    light = 0
    for x in range(840, 1016):
        i = (y*w + x)*3
        if pix[i] >= 0x90 and pix[i+1] >= 0x90 and pix[i+2] >= 0x90:
            light += 1
    rows.append((y, light))
bands, cur = [], []
for y, light in rows:
    if light >= 3:
        cur.append(y)
    elif cur:
        bands.append(cur); cur = []
if cur: bands.append(cur)
if bands:
    band = bands[0]
    ycen = (band[0] + band[-1]) // 2
    xs = [x for x in range(840, 1016)
          for yy in (band[0], band[len(band)//2], band[-1])
          if pix[(yy*w + x)*3] >= 0x90]
    xcen = (min(xs) + max(xs)) // 2 if xs else 940
    print(f"{xcen},{ycen}")
PYSESS1
    cat > "$WORK/find-ok.py" <<'PYSESS2'
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
xs, ys = [], []
for y in range(40, 300):
    for x in range(700, 1020):
        i = (y*w + x)*3
        if abs(pix[i]-0x4f) <= 22 and abs(pix[i+1]-0x9a) <= 22 \
           and abs(pix[i+2]-0xdc) <= 22:
            xs.append(x); ys.append(y)
if xs and len(xs) > 200:   # a real button, not antialias debris
    print(f"{(min(xs)+max(xs))//2},{(min(ys)+max(ys))//2}")
PYSESS2
    # the session button is the LAST bar element: right-anchored, so a
    # click 49px from the right edge lands inside it for any username
    ti "click 975,22"
    # first action row of the right-hand popover = Log Out: light text
    # on the dark sheet, clustered into bands; click band 1's center
    SESS_ROW=""
    for try in 1 2 3 4 5 6; do
      sleep 0.5
      rm -f /tmp/vantage-wayland.ppm
      kill -USR1 "$WM_CHILD2" 2>/dev/null
      wait_ppm || continue
      [ -s /tmp/vantage-wayland.ppm ] || continue
      SESS_ROW=$(python3 "$WORK/find-row.py" 2>/dev/null)
      [ -n "$SESS_ROW" ] && break
    done
    if [ -n "$SESS_ROW" ]; then
      ti "click ${SESS_ROW%,*},${SESS_ROW#*,}"
      # the confirm view: question + [Cancel] [Log Out]; the confirm
      # button is the ONLY blue object on screen (#4f9adc) — find it
      OKBTN=""
      for try in 1 2 3 4 5 6; do
        sleep 0.5
        rm -f /tmp/vantage-wayland.ppm
        kill -USR1 "$WM_CHILD2" 2>/dev/null
        wait_ppm || continue
        [ -s /tmp/vantage-wayland.ppm ] || continue
        OKBTN=$(python3 "$WORK/find-ok.py" 2>/dev/null)
        [ -n "$OKBTN" ] && break
      done
      if [ -n "$OKBTN" ]; then
        ti "click ${OKBTN%,*},${OKBTN#*,}"
        UI_LOGOUT_CLICKED=1
        # the panel's OWN action must end the session — graceful
        # SIGTERM policy, exit 0, no stragglers (asserted below by
        # the shared round-trip section)
        for i in $(seq 1 60); do
          kill -0 "$SESS_PID" 2>/dev/null || break
          sleep 0.1
        done
        if ! kill -0 "$SESS_PID" 2>/dev/null; then
          ok "panel session menu logged the DE out (popover confirm flow)"
        else
          bad "panel logout confirm clicked but session survived"
        fi
      else
        bad "session confirm view never showed its Log Out button"
      fi
    else
      bad "session menu popover did not show action rows"
    fi
  else
    bad "no WM child for the panel-logout check"
  fi
fi

# ------------------------------------------------- logout round-trip
# The panel's session menu sends WM_LOGOUT to the session IPC socket
# (vp_session_action); vantage-remote logout drives the SAME socket.
# Ending the session must use the graceful SIGTERM policy (no SIGKILL,
# no restart).
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
STRAGGLERS=$(pgrep -f "$(vb vantage-wm)|$(vb vantage-session)|$(vb vantage-panel)" 2>/dev/null | wc -l)
if [ "${STRAGGLERS:-0}" -eq 0 ]; then
  ok "no session/WM stragglers left"
else
  bad "$STRAGGLERS process(es) survived session shutdown"
  pgrep -af "$(vb vantage-wm)|$(vb vantage-session)|$(vb vantage-panel)" 2>/dev/null | head -5
fi

rm -f /tmp/vantage-wayland.ppm
echo
echo "============================================"
echo "harness-wayland: $PASS passed, $FAIL failed"
echo "artifacts: $WORK"
echo "============================================"
[ "$FAIL" -eq 0 ] || {
  echo "---- panel.log ----"; tail -30 "$PANEL_LOG" 2>/dev/null
  echo "---- ui.log (test-input) ----"; tail -10 "$WORK/ui.log" 2>/dev/null
  echo "---- wm.log ----"; cat "$WORK/wm.log"; \
                        echo "---- session.log ----"; cat "$SESS_LOG"; \
                        echo "---- failed checks ----"; cat "$FAILS"; }
[ "$FAIL" -eq 0 ]
