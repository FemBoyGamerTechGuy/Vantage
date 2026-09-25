#!/bin/bash
# package.sh — Vantage's native Arch Linux packager
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Invoked by `./build packages arch` (never run by hand with makepkg):
#
#   package.sh STAGE OUTDIR VERSION [PKGREL]
#
#   STAGE   DESTDIR staging tree (already installed by meson, prefix /usr)
#   OUTDIR  where *.pkg.tar.zst artifacts are written
#   VERSION project version (single source: meson.build)
#   PKGREL  package release (default 1)
#
# Produces, without root and without makepkg:
#   vantage-$VERSION-$PKGREL-$ARCH.pkg.tar.zst        (the desktop)
#   vantage-devel-$VERSION-$PKGREL-$ARCH.pkg.tar.zst  (C headers)
#
# Package layout rationale: the runtime desktop is one coherent unit
# (WM + panel + desktop + tools + themes share the same IPC protocol and
# data files), so it is not split further; development headers go into
# vantage-devel. Dependencies are computed from the actual link set
# (ldd) of the staged ELF files — nothing is guessed or faked.

set -euo pipefail

STAGE="$(readlink -f "${1:?usage: package.sh STAGE OUTDIR VERSION [PKGREL]}")"
OUTDIR="${2:?missing OUTDIR}"
VERSION="${3:?missing VERSION}"
PKGREL="${4:-1}"
ARCH="$(uname -m)"
SRC="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/../.." >/dev/null 2>&1 && pwd)"

say()  { printf '\033[1;34m[arch-packager]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[arch-packager] error:\033[0m %s\n' "$*" >&2; exit 1; }

command -v bsdtar >/dev/null 2>&1 || die "bsdtar (libarchive) is required"
command -v zstd  >/dev/null 2>&1 || die "zstd is required"

[ -x "$STAGE/usr/bin/vantage-session" ] \
    || die "staging tree has no /usr/bin/vantage-session (./build stage failed?)"

mkdir -p "$OUTDIR"

# --------------------------------------------------------------- deps
# SONAME (as reported by ldd) -> Arch package. Only dependencies that
# are ACTUALLY linked end up in .PKGINFO; unknown SONAMEs are reported.
declare -A SONAME_MAP=(
    [libc.so.6]=glibc        [libm.so.6]=glibc         [libpthread.so.0]=glibc
    [libdl.so.2]=glibc       [librt.so.1]=glibc        [libresolv.so.2]=glibc
    [libgcc_s.so.1]=gcc-libs [libstdc++.so.6]=gcc-libs
    [libX11.so.6]=libx11     [libX11-xcb.so.1]=libx11
    [libxcb.so.1]=libxcb
    [libxcb-util.so.1]=xcb-util
    [libxcb-ewmh.so.2]=xcb-util-wm
    [libxcb-icccm.so.4]=xcb-util-wm
    [libXrandr.so.2]=libxrandr   [libXi.so.6]=libxi
    [libXrender.so.1]=libxrender
    [libXcomposite.so.1]=libxcomposite
    [libXdamage.so.1]=libxdamage
    [libXfixes.so.3]=libxfixes   [libXinerama.so.1]=libxinerama
    [libXcursor.so.1]=libxcursor
    [libXext.so.6]=libxext       [libXft.so.2]=libxft
    [libXau.so.6]=libxau         [libXdmcp.so.6]=libxdmcp
    [libwayland-client.so.0]=wayland
    [libwayland-server.so.0]=wayland
    [libwayland-cursor.so.0]=wayland
    [libxkbcommon.so.0]=libxkbcommon
    [libxkbcommon-x11.so.0]=libxkbcommon
    [libEGL.so.1]=libglvnd       [libGLESv2.so.2]=libglvnd
    [libGL.so.1]=libglvnd        [libOpenGL.so.0]=libglvnd
    [libgbm.so.1]=mesa           [libdrm.so.2]=libdrm
    [libasound.so.2]=alsa-lib
    [libpulse.so.0]=libpulse
    [libpipewire-0.3.so.0]=pipewire
    [libavcodec.so.61]=ffmpeg    [libavformat.so.61]=ffmpeg
    [libavutil.so.60]=ffmpeg     [libswscale.so.8]=ffmpeg
    [libswresample.so.5]=ffmpeg
    [libavcodec.so.60]=ffmpeg    [libavformat.so.60]=ffmpeg
    [libavutil.so.59]=ffmpeg     [libswscale.so.7]=ffmpeg
    [libswresample.so.4]=ffmpeg
    [libavcodec.so.59]=ffmpeg    [libavformat.so.59]=ffmpeg
    [libavutil.so.58]=ffmpeg     [libswscale.so.6]=ffmpeg
    [libswresample.so.3]=ffmpeg
    [libjson-c.so.5]=json-c
    [libpng16.so.16]=libpng
    [libjpeg.so.8]=libjpeg-turbo [libjpeg.so.62]=libjpeg-turbo
    [libgdk_pixbuf-2.0.so.0]=gdk-pixbuf2
    [libgobject-2.0.so.0]=glib2  [libglib-2.0.so.0]=glib2
    [libgio-2.0.so.0]=glib2
    [libsystemd.so.0]=systemd-libs  [libudev.so.1]=systemd-libs
    [libelogind.so.0]=elogind
    [libdbus-1.so.3]=dbus
    [libinput.so.10]=libinput
    [libnm.so.0]=networkmanager
    [libupower-glib.so.3]=upower
)

soname_deps() {   # $@: ELF files -> echo Arch package names (sorted, unique)
    local files=("$@") sonames=() unknown=()
    [ ${#files[@]} -gt 0 ] || return 0
    # Direct DT_NEEDED entries only (what the binaries actually link
    # against) — NOT ldd's transitive closure, which would pull in
    # dependencies-of-dependencies owned by other packages.
    while read -r so; do
        [ -n "$so" ] || continue
        if [ -n "${SONAME_MAP[$so]:-}" ]; then
            sonames+=("${SONAME_MAP[$so]}")
        else
            unknown+=("$so")
        fi
    done < <(readelf -d "${files[@]}" 2>/dev/null \
                | awk '/NEEDED/ { gsub(/.*\[|\].*/, "", $0); print }' \
                | sort -u)
    if [ ${#unknown[@]} -gt 0 ]; then
        say "WARNING: unmapped SONAMEs (not added to depends): ${unknown[*]}"
    fi
    printf '%s\n' "${sonames[@]}" | sort -u | sed '/^$/d'
}

# ------------------------------------------------------- package builder
make_pkginfo() {  # $1=pkgname $2=pkgdesc $3=destdir; depends on stdin
    local name="$1" desc="$2" dest="$3" size
    size="$(du -sb "$dest" | cut -f1)"
    {
        echo "pkgname = $name"
        echo "pkgver = $VERSION-$PKGREL"
        echo "pkgdesc = $desc"
        echo "url = https://github.com/FemBoyGamerTechGuy/Vantage"
        echo "builddate = $(date -u +%s)"
        echo "packager = Vantage Build System <vantage@localhost>"
        echo "size = $size"
        echo "arch = $ARCH"
        echo "license = GPL-2.0-or-later"
        local dep
        while read -r dep; do
            [ -n "$dep" ] && echo "depend = $dep"
        done
        echo "provides = $name=$VERSION"
        echo "conflicts = $name"
    } > "$dest/.PKGINFO"
}

make_mtree() {    # $1=destdir (writes $1/.MTREE, gzipped BSD mtree)
    ( cd "$1" && bsdtar --format=mtree \
        --options '!all,use-set,mode,time,size,sha256digest,type' \
        --exclude='.MTREE*' \
        -cf - . ) | gzip -9n > "$1/.MTREE"
}

make_buildinfo() {  # $1=pkgname $2=destdir
    {
        echo "format = 2"
        echo "pkgname = $1"
        echo "pkgver = $VERSION-$PKGREL"
        echo "arch = $ARCH"
        echo "buildtool = vantage-build"
        echo "builddate = $(date -u +%s)"
    } > "$2/.BUILDINFO"
}

pack_dir() {      # $1=destdir $2=outfile
    local dest="$1" out="$2" entries
    rm -f "$out"
    # top-level payload dirs present in this package (usr, etc, ...)
    entries="$(cd "$dest" && find . -mindepth 1 -maxdepth 1 -type d \
                ! -name '.*' -printf '%P\n' | LC_ALL=C sort | tr '\n' ' ')"
    ( cd "$dest" && \
      bsdtar -cf - .PKGINFO .MTREE .BUILDINFO $entries ) \
        | zstd -q -T0 -19 -o "$out" -
    rm -f "$dest/.PKGINFO" "$dest/.MTREE" "$dest/.BUILDINFO"
}

# ------------------------------------------------ package: vantage
PKGMAIN="$(mktemp -d /tmp/vantage-pkg.XXXXXX)"
PKGDEV=""
trap 'rm -rf "$PKGMAIN" ${PKGDEV:+"$PKGDEV"}' EXIT

say "assembling vantage (runtime desktop)"
mkdir -p "$PKGMAIN/usr"
cp -a "$STAGE/usr/bin"     "$PKGMAIN/usr/"
[ -d "$STAGE/usr/share" ] && cp -a "$STAGE/usr/share" "$PKGMAIN/usr/"
[ -d "$STAGE/usr/lib" ]   && cp -a "$STAGE/usr/lib"   "$PKGMAIN/usr/"
[ -d "$STAGE/etc" ]       && cp -a "$STAGE/etc"       "$PKGMAIN/"
# vantage-devel payload does not belong here
rm -rf "$PKGMAIN/usr/include"
# distro license + docs
mkdir -p "$PKGMAIN/usr/share/licenses/vantage"
cp "$SRC/LICENSE" "$PKGMAIN/usr/share/licenses/vantage/LICENSE"

ELFS=()
while IFS= read -r -d '' f; do ELFS+=("$f"); done \
    < <(find "$PKGMAIN" -type f -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print0 2>/dev/null)
DEPS="$(soname_deps "${ELFS[@]:-}")"
NDEPS="$(printf '%s' "$DEPS" | grep -c . || true)"
say "runtime depends ($NDEPS): $(printf '%s' "$DEPS" | tr '\n' ' ')"

make_pkginfo  "vantage" \
    "Lightweight raw-C desktop environment: Wayland compositor + X11 backend, EWMH WM, panel, desktop, theming" \
    "$PKGMAIN" <<<"$DEPS"
make_mtree    "$PKGMAIN"
make_buildinfo "vantage" "$PKGMAIN"
MAIN_OUT="$OUTDIR/vantage-$VERSION-$PKGREL-$ARCH.pkg.tar.zst"
pack_dir "$PKGMAIN" "$MAIN_OUT"
say "wrote $MAIN_OUT"

# ------------------------------------------------ package: vantage-devel
if [ -d "$STAGE/usr/include/vantage-"* ]; then
    say "assembling vantage-devel (development headers)"
    PKGDEV="$(mktemp -d /tmp/vantage-pkg-devel.XXXXXX)"
    mkdir -p "$PKGDEV/usr"
    cp -a "$STAGE/usr/include" "$PKGDEV/usr/"
    make_pkginfo "vantage-devel" \
        "Development headers for the Vantage desktop environment" \
        "$PKGDEV" <<<""
    echo "depend = vantage=$VERSION" >> "$PKGDEV/.PKGINFO"
    make_mtree    "$PKGDEV"
    make_buildinfo "vantage-devel" "$PKGDEV"
    DEV_OUT="$OUTDIR/vantage-devel-$VERSION-$PKGREL-$ARCH.pkg.tar.zst"
    pack_dir "$PKGDEV" "$DEV_OUT"
    say "wrote $DEV_OUT"
else
    say "no headers staged; skipping vantage-devel"
fi

# ------------------------------------------------------------- summary
say "artifacts:"
ls -la "$OUTDIR"/*.pkg.tar.zst
