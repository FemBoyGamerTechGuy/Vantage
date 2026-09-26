#!/bin/bash
# package.sh — Vantage's native Arch Linux packager
#
# SPDX-License-Identifier: LicenseRef-Vantage-Proprietary
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
# (readelf DT_NEEDED) of the staged ELF files — nothing is guessed or
# faked.
#
# Channel discipline: stdout of this script carries PACKAGE DATA only
# (the dependency list travels through command substitution); every
# diagnostic goes to stderr. .PKGINFO is additionally validated at
# write time, so a diagnostic string can never become metadata.

set -euo pipefail

STAGE="$(readlink -f "${1:?usage: package.sh STAGE OUTDIR VERSION [PKGREL]}")"
OUTDIR="${2:?missing OUTDIR}"
VERSION="${3:?missing VERSION}"
PKGREL="${4:-1}"
ARCH="$(uname -m)"
SRC="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")/../.." >/dev/null 2>&1 && pwd)"

# Diagnostics ALWAYS go to stderr. The dependency list is captured with
# command substitution (`DEPS="$(soname_deps ...)"`), which grabs stdout —
# so a diagnostic printed to stdout from inside that function would end up
# in $DEPS and be written to .PKGINFO as a `depend =` entry. That is
# exactly the bug that once made pacman try to install
# "[arch-packager] WARNING: unmapped SONAMEs ..." as a dependency.
say()  { printf '\033[1;34m[arch-packager]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[arch-packager] WARNING:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[arch-packager] error:\033[0m %s\n' "$*" >&2; exit 1; }

command -v bsdtar >/dev/null 2>&1 || die "bsdtar (libarchive) is required"
command -v zstd  >/dev/null 2>&1 || die "zstd is required"

[ -x "$STAGE/usr/bin/vantage-session" ] \
    || die "staging tree has no /usr/bin/vantage-session (./build stage failed?)"

mkdir -p "$OUTDIR"

# --------------------------------------------------------------- deps
# Library name (SONAME without its version suffix) -> Arch package.
#
# Matching is deliberately version-independent: on Arch the owning
# package does not change when a library bumps its SONAME — ffmpeg owns
# libavcodec.so.59, .61, .63 and every future version alike — so a table
# keyed by exact SONAME versions silently rots as the build host's
# libraries move on (that is precisely why a newer FFmpeg was reported
# "unmapped" although the owning package never changed).
#
# Resolution order per SONAME (see resolve_soname / resolve_via_pacman):
#   1. exact versioned key in SONAME_MAP (explicit override, if any)
#   2. unversioned prefix in SONAME_MAP (libavcodec.so.63 -> ffmpeg)
#   3. on Arch build hosts: locate the file via the linker cache and ask
#      pacman which installed package owns it (fully machine-driven)
#   4. otherwise: report via stderr and keep it OUT of the metadata
#
# Only dependencies that are ACTUALLY linked end up in .PKGINFO.
declare -A SONAME_MAP=(
    # glibc / toolchain runtime
    [libc.so]=glibc        [libm.so]=glibc          [libpthread.so]=glibc
    [libdl.so]=glibc       [librt.so]=glibc         [libresolv.so]=glibc
    [libutil.so]=glibc     [libanl.so]=glibc
    [ld-linux-x86-64.so]=glibc  [ld-linux-x86.so]=glibc
    [ld-linux-aarch64.so]=glibc
    [libgcc_s.so]=gcc-libs [libstdc++.so]=gcc-libs
    # X11 core
    [libX11.so]=libx11     [libX11-xcb.so]=libx11
    [libxcb.so]=libxcb
    # xcb utility libraries ship as separate Arch packages
    [libxcb-util.so]=xcb-util           [libxcb-ewmh.so]=xcb-util-wm
    [libxcb-icccm.so]=xcb-util-wm       [libxcb-keysyms.so]=xcb-util-keysyms
    [libxcb-image.so]=xcb-util-image    [libxcb-render-util.so]=xcb-util-renderutil
    [libxcb-cursor.so]=xcb-util-cursor
    # (every other libxcb-*.so sublibrary ships inside libxcb itself;
    #  see the family fallback in resolve_soname)
    [libXrandr.so]=libxrandr   [libXi.so]=libxi        [libXrender.so]=libxrender
    [libXcomposite.so]=libxcomposite
    [libXdamage.so]=libxdamage [libXfixes.so]=libxfixes
    [libXinerama.so]=libxinerama
    [libXcursor.so]=libxcursor [libXext.so]=libxext    [libXft.so]=libxft
    [libXau.so]=libxau         [libXdmcp.so]=libxdmcp
    # Wayland
    [libwayland-client.so]=wayland [libwayland-server.so]=wayland
    [libwayland-cursor.so]=wayland [libwayland-egl.so]=wayland
    [libxkbcommon.so]=libxkbcommon [libxkbcommon-x11.so]=libxkbcommon
    [libxkbregistry.so]=libxkbcommon
    # graphics stack
    [libEGL.so]=libglvnd   [libGLESv2.so]=libglvnd  [libGL.so]=libglvnd
    [libOpenGL.so]=libglvnd [libGLX.so]=libglvnd    [libGLdispatch.so]=libglvnd
    [libgbm.so]=mesa       [libdrm.so]=libdrm
    [libvulkan.so]=vulkan-icd-loader
    # audio
    [libasound.so]=alsa-lib  [libpulse.so]=libpulse
    [libpipewire-0.2.so]=pipewire [libpipewire-0.3.so]=pipewire
    # multimedia — every libav* library ships in the single ffmpeg package
    [libavcodec.so]=ffmpeg   [libavformat.so]=ffmpeg  [libavutil.so]=ffmpeg
    [libswscale.so]=ffmpeg   [libswresample.so]=ffmpeg
    [libavfilter.so]=ffmpeg  [libavdevice.so]=ffmpeg  [libpostproc.so]=ffmpeg
    # data / image / GLib
    [libjson-c.so]=json-c  [libpng16.so]=libpng     [libpng.so]=libpng
    [libfreetype.so]=freetype2 [libfontconfig.so]=fontconfig
    [libjpeg.so]=libjpeg-turbo [libturbojpeg.so]=libjpeg-turbo
    [libgdk_pixbuf-2.0.so]=gdk-pixbuf2
    [libgobject-2.0.so]=glib2 [libglib-2.0.so]=glib2 [libgio-2.0.so]=glib2
    [libgmodule-2.0.so]=glib2 [libgthread-2.0.so]=glib2
    # system services
    [libsystemd.so]=systemd-libs [libudev.so]=systemd-libs
    [libelogind.so]=elogind   [libdbus-1.so]=dbus
    [libinput.so]=libinput    [libnm.so]=networkmanager
    [libseat.so]=seatd
    [libupower-glib.so]=upower
)

# Strip the version suffix from a SONAME: libavcodec.so.63 -> libavcodec.so
soname_base() {
    local so="$1"
    if [[ $so =~ ^(.*)\.so\.[0-9]+(\.[0-9]+)*$ ]]; then
        printf '%s.so\n' "${BASH_REMATCH[1]}"
    else
        printf '%s\n' "$so"
    fi
}

resolve_soname() {   # $1 SONAME -> package name on stdout; rc!=1 if unknown
    local so="$1" base
    # 1. exact (versioned) key — an explicit per-version override, if ever
    #    one is needed because a library genuinely changed owners
    if [ -n "${SONAME_MAP[$so]:-}" ]; then
        printf '%s\n' "${SONAME_MAP[$so]}"
        return 0
    fi
    # 2. version-independent prefix: any libavcodec.so.N -> ffmpeg
    base="$(soname_base "$so")"
    if [ "$base" != "$so" ] && [ -n "${SONAME_MAP[$base]:-}" ]; then
        printf '%s\n' "${SONAME_MAP[$base]}"
        return 0
    fi
    # 3. library families whose unlisted members share one owner
    case "$base" in
        libxcb-*)      printf 'libxcb\n'   ;;
        libpipewire-*) printf 'pipewire\n' ;;
        *) return 1 ;;
    esac
}

resolve_via_pacman() {   # $1 SONAME -> owning package on stdout (Arch hosts)
    local so="$1" path pkg
    # On a real Arch build host the truth is the package database itself:
    # locate the library through the dynamic-linker cache, then ask
    # pacman which installed package owns that file. This resolves ANY
    # library the static table does not know — no version enumeration
    # and no hardcoded package list can rot this way.
    command -v pacman >/dev/null 2>&1 || return 1
    path="$(ldconfig -p 2>/dev/null | awk -v s="$so" '$1 == s { print $NF; exit }')"
    [ -n "$path" ] && [ -f "$path" ] || return 1
    pkg="$(pacman -Qqo "$path" 2>/dev/null)" || return 1
    [ -n "$pkg" ] || return 1
    printf '%s\n' "$pkg"
}

# A dependency value that may safely be written to .PKGINFO: an Arch
# package name, optionally with a =version constraint. Anything else —
# in particular diagnostic output accidentally captured into the
# dependency list — aborts packaging instead of reaching the metadata.
valid_dep() {
    [[ $1 =~ ^[A-Za-z0-9@._+-]+(=[A-Za-z0-9._:+-]+)?$ ]]
}

soname_deps() {   # $@: ELF files -> Arch package names on stdout (sorted, unique)
    local files=("$@") sonames=() unknown=() so pkg
    [ ${#files[@]} -gt 0 ] || return 0
    # Direct DT_NEEDED entries only (what the binaries actually link
    # against) — NOT ldd's transitive closure, which would pull in
    # dependencies-of-dependencies owned by other packages.
    while read -r so; do
        [ -n "$so" ] || continue
        pkg="$(resolve_soname "$so" || true)"
        if [ -z "$pkg" ]; then
            pkg="$(resolve_via_pacman "$so" || true)"
        fi
        if [ -n "$pkg" ]; then
            sonames+=("$pkg")
        else
            unknown+=("$so")
        fi
    done < <(readelf -d "${files[@]}" 2>/dev/null \
                | awk '/NEEDED/ { gsub(/.*\[|\].*/, "", $0); print }' \
                | sort -u)
    # Diagnostics on stderr ONLY — stdout is the data channel feeding
    # .PKGINFO through command substitution.
    if [ ${#unknown[@]} -gt 0 ]; then
        warn "unmapped SONAMEs (not added to depends): ${unknown[*]}"
    fi
    printf '%s\n' "${sonames[@]}" | sort -u | sed '/^$/d'
}

# ------------------------------------------------------- package builder
make_pkginfo() {  # $1=pkgname $2=pkgdesc $3=destdir [$4=conflict-with]; depends on stdin
    local name="$1" desc="$2" dest="$3" conflict_with="${4:-}" size dep
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
        echo "license = LicenseRef-Vantage-Proprietary"
        while read -r dep; do
            [ -n "$dep" ] || continue
            valid_dep "$dep" || die "invalid dependency '$dep' would corrupt "\
                "metadata (diagnostics leaked into the dependency list?)"
            echo "depend = $dep"
        done
        echo "provides = $name=$VERSION"
        # .PKGINFO spells the singular key `conflict` — the plural
        # `conflicts` is PKGBUILD syntax and pacman rejects it as an
        # unknown key. Value: the -git build of this package; released
        # and git builds of the same desktop must not coexist. A package
        # never conflicts with its own name (pacman ignores that noise).
        if [ -n "$conflict_with" ]; then
            echo "conflict = $conflict_with"
        fi
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
    "$PKGMAIN" "vantage-git" <<<"$DEPS"
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
        "$PKGDEV" "vantage-devel-git" <<<"vantage=$VERSION"
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
