#!/usr/bin/env bash
# Build reaper_ambix for Linux and package it as a .tar.gz.
#
# Usage:  ./scripts/build_linux.sh
#
# REAPER has no standard plugin installer on Linux, so we ship a tarball
# containing the .so plus a small install.sh that copies it into the user's
# REAPER UserPlugins folder.
#
# Output:
#   _LINUX_RELEASE/reaper_ambix_vX.Y.Z_linux_<arch>.tar.gz
#     |-- reaper_ambix.so       (statically linked: libambix + WavPack + WDL)
#     |-- install.sh            (copies .so into ~/.config/REAPER/UserPlugins)
#     |-- README.md
#     |-- COPYING               (plugin, LGPL)
#     `-- libambix-COPYING      (libambix, LGPL)

set -e

ROOT=$(cd "$(dirname "$0")/.."; pwd)
BUILD_DIR="$ROOT/_build"
PLUGIN_NAME="reaper_ambix.so"
VERSION=$(<"$ROOT/VERSION")
ARCH=$(uname -m)
PKG_NAME="reaper_ambix_v${VERSION}_linux_${ARCH}"

echo ""; echo "=== reaper_ambix v$VERSION — Linux tarball build ($ARCH) ==="

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$ROOT/_LINUX_RELEASE"

echo ""; echo "=== Configuring (Release) ==="
cmake -S "$ROOT" -B "$BUILD_DIR" \
      -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DREAPER_AMBIX_INSTALL_USER_PLUGINS=OFF \
      -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD_DIR/out"

echo ""; echo "=== Building ==="
cmake --build "$BUILD_DIR" -j "$(nproc)"

PLUGIN_BUILT="$BUILD_DIR/out/$PLUGIN_NAME"
if [ ! -f "$PLUGIN_BUILT" ]; then
    echo "Error: build did not produce $PLUGIN_BUILT"
    exit 1
fi

echo ""; echo "=== Resolved dep list (should be system libs only) ==="
ldd "$PLUGIN_BUILT" | sed 's/^/  /' || true

# =========================================================
# Assemble tarball staging dir
# =========================================================
STAGE="$BUILD_DIR/$PKG_NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$PLUGIN_BUILT"          "$STAGE/$PLUGIN_NAME"
cp "$ROOT/README.md"        "$STAGE/README.md"
cp "$ROOT/COPYING"          "$STAGE/COPYING"
cp "$ROOT/libambix/COPYING" "$STAGE/libambix-COPYING"
cp "$ROOT/scripts/installer/linux_install.sh" "$STAGE/install.sh"
chmod +x "$STAGE/install.sh"

TARBALL="$ROOT/_LINUX_RELEASE/${PKG_NAME}.tar.gz"
echo ""; echo "=== Creating $(basename "$TARBALL") ==="
tar -czf "$TARBALL" -C "$BUILD_DIR" "$PKG_NAME"

echo ""; echo "Done!"
ls -la "$TARBALL"
