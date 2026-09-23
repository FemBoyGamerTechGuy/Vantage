#!/bin/bash
#
# Generic install script for Vantage — runs `meson install` into PREFIX.
# Useful for distros without native packaging.
#
set -e

PREFIX="${1:-/usr}"
SRC="$(dirname "$(readlink -f "$0")")/.."

cd "$SRC"
if [ ! -d build ]; then
    meson setup build --prefix="$PREFIX" --buildtype=release
fi
ninja -C build
sudo ninja -C build install

echo ""
echo "Vantage installed to $PREFIX."
echo "To start Vantage:"
echo "  vantage-session"
echo ""
echo "Or select Vantage from your display manager (gdm/sddm/lightdm)."
