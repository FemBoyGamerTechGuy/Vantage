#!/bin/bash
# harness-cli.sh — Vantage CLI & distribution-model integration harness
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Verifies the session-manager command line and that Vantage runs both
# from the build tree and from an installed staging prefix:
#
#   1.  --help       exit 0, documents --wayland and --x11
#   2.  --version    exit 0, prints "Vantage <version>" (single source:
#                   the meson project version)
#   3.  invalid flag nonzero exit + useful stderr
#   4.  --x11 without $DISPLAY        clear error, nonzero exit
#   5.  --wayland inside a Wayland session (WAYLAND_DISPLAY set)
#                   refuses to nest, nonzero exit
#   6.  repo-root launch symlinks     ./vantage-session --version
#   7.  installed execution           DESTDIR-stage install into a temp
#                                   prefix; staged binaries answer
#                                   --version/--help and reject bad flags
#   8.  vantage-wm parity            same CLI surface as the session
#
# Usage: harness-cli.sh [build-dir] [source-root]
# (meson passes both; defaults work for manual runs from tests/)

set -u
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

BUILD="${1:-${VT_BUILD_DIR:-$(cd "$(dirname "$0")/.." && pwd)/builddir}}"
SRC="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="$BUILD/src/tools"
SESSION="$BIN/vantage-session"
WM="$BIN/vantage-wm"
[ -x "$SESSION" ] || { echo "vantage-session not built ($SESSION)"; exit 77; }

# version single source: meson.build
VERSION="$(sed -n "s/^  version *: *'\(.*\)',$/\1/p" "$SRC/meson.build" | head -n1)"
[ -n "$VERSION" ] || { echo "cannot parse version from meson.build"; exit 77; }

WORK="$(mktemp -d /tmp/vantage-cli.XXXXXX)"

# run in an environment with no display-transport or backend selection
run_clean() {
  env -u DISPLAY -u WAYLAND_DISPLAY -u VANTAGE_BACKEND \
      XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}" "$@"
}

echo "== harness-cli: --help =="
OUT="$(run_clean "$SESSION" --help 2>&1)"; RC=$?
[ $RC -eq 0 ] && ok "--help exits 0" || bad "--help exit code $RC"
echo "$OUT" | grep -q -- "--wayland" && ok "--help documents --wayland" \
  || bad "--help does not mention --wayland"
echo "$OUT" | grep -q -- "--x11" && ok "--help documents --x11" \
  || bad "--help does not mention --x11"
echo "$OUT" | grep -qi "usage" && ok "--help shows usage" \
  || bad "--help shows no usage"

echo "== harness-cli: --version =="
OUT="$(run_clean "$SESSION" --version 2>&1)"; RC=$?
[ $RC -eq 0 ] && ok "--version exits 0" || bad "--version exit code $RC"
echo "$OUT" | grep -qx "Vantage $VERSION" \
  && ok "--version prints 'Vantage $VERSION' (meson single source)" \
  || bad "--version output: '$OUT' (expected 'Vantage $VERSION')"

echo "== harness-cli: invalid option =="
OUT="$(run_clean "$SESSION" --something-invalid 2>&1)"; RC=$?
[ $RC -ne 0 ] && ok "invalid option exits nonzero ($RC)" \
  || bad "invalid option exited 0"
echo "$OUT" | grep -q -- "--something-invalid" \
  && ok "error names the bad option" || bad "error does not name the option: '$OUT'"
echo "$OUT" | grep -qi "help" && ok "error points at --help" \
  || bad "error does not mention --help"

echo "== harness-cli: --x11 without a display =="
OUT="$(run_clean "$SESSION" --x11 2>&1)"; RC=$?
[ $RC -ne 0 ] && ok "--x11 without DISPLAY exits nonzero ($RC)" \
  || bad "--x11 without DISPLAY exited 0"
echo "$OUT" | grep -qi "DISPLAY" && ok "error explains the DISPLAY problem" \
  || bad "error does not mention DISPLAY: '$OUT'"

echo "== harness-cli: --wayland inside a Wayland session =="
OUT="$(env -u DISPLAY -u VANTAGE_BACKEND WAYLAND_DISPLAY=wayland-0 \
      XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}" "$SESSION" --wayland 2>&1)"; RC=$?
[ $RC -ne 0 ] && ok "--wayland with WAYLAND_DISPLAY set refuses to nest ($RC)" \
  || bad "--wayland nested silently (exit 0)"
echo "$OUT" | grep -qi "nest" && ok "refusal explains nesting" \
  || bad "no nesting explanation: '$OUT'"

echo "== harness-cli: vantage-wm CLI parity =="
OUT="$(run_clean "$WM" --version 2>&1)"; RC=$?
[ $RC -eq 0 ] && echo "$OUT" | grep -qx "Vantage $VERSION" \
  && ok "vantage-wm --version matches session version" \
  || bad "vantage-wm --version: '$OUT' (rc=$RC)"
OUT="$(run_clean "$WM" --help 2>&1)"; RC=$?
[ $RC -eq 0 ] && echo "$OUT" | grep -q -- "--x11" \
  && ok "vantage-wm --help documents backends" \
  || bad "vantage-wm --help missing backends (rc=$RC)"
OUT="$(run_clean "$WM" --nope 2>&1)"; RC=$?
[ $RC -ne 0 ] && ok "vantage-wm rejects invalid flags" \
  || bad "vantage-wm accepted --nope"

echo "== harness-cli: development-tree execution (repo-root symlinks) =="
if [ -x "$SRC/vantage-session" ]; then
  OUT="$(run_clean "$SRC/vantage-session" --version 2>&1)"; RC=$?
  [ $RC -eq 0 ] && echo "$OUT" | grep -qx "Vantage $VERSION" \
    && ok "./vantage-session --version works from the source tree" \
    || bad "./vantage-session --version: '$OUT' (rc=$RC)"
  OUT="$(run_clean "$SRC/vantage-session" --nope 2>&1)"; RC=$?
  [ $RC -ne 0 ] && ok "./vantage-session rejects invalid flags" \
    || bad "./vantage-session accepted --nope"
else
  echo "  SKIP: no repo-root symlinks (created by ./build build)"
fi

echo "== harness-cli: installed execution (temporary staging prefix) =="
STAGE="$WORK/stage"
if DESTDIR="$STAGE" ninja -C "$BUILD" install >/dev/null 2>&1; then
  INST_SESS="$(find "$STAGE" -type f -name vantage-session -perm -111 | head -n1)"
  if [ -n "$INST_SESS" ]; then
    INST_DIR="$(dirname "$INST_SESS")"
    ok "installed vantage-session at ${INST_SESS#$STAGE}"
    OUT="$(run_clean "$INST_SESS" --version 2>&1)"; RC=$?
    [ $RC -eq 0 ] && echo "$OUT" | grep -qx "Vantage $VERSION" \
      && ok "installed binary answers --version" \
      || bad "installed --version: '$OUT' (rc=$RC)"
    OUT="$(run_clean "$INST_SESS" --help 2>&1)"; RC=$?
    [ $RC -eq 0 ] && echo "$OUT" | grep -q -- "--wayland" \
      && ok "installed binary answers --help" \
      || bad "installed --help rc=$RC"
    OUT="$(run_clean "$INST_SESS" --bogus 2>&1)"; RC=$?
    [ $RC -ne 0 ] && ok "installed binary rejects bad flags" \
      || bad "installed binary accepted --bogus"
    # a sibling installed binary (binary discovery smoke test)
    INST_WM="$(find "$STAGE" -type f -name vantage-wm -perm -111 | head -n1)"
    if [ -n "$INST_WM" ]; then
      OUT="$(run_clean "$INST_WM" --version 2>&1)"; RC=$?
      [ $RC -eq 0 ] && ok "installed vantage-wm answers --version" \
        || bad "installed vantage-wm --version rc=$RC"
    fi
  else
    bad "staged install contains no vantage-session"
  fi
else
  bad "DESTDIR staging install failed"
fi

# ------------------------------------------------------------- summary
echo
echo "=========================================="
echo "harness-cli: $PASS passed, $FAIL failed"
echo "artifacts: $WORK"
echo "=========================================="
[ "$FAIL" -eq 0 ]
