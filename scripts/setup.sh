#!/usr/bin/env bash
# One-time setup: clone git submodules (WDL, WavPack, libambix) and run the
# initial CMake configure. Re-run when submodule refs change.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ -d .git ]; then
    echo ">>> Initialising git submodules (WDL, WavPack, libambix)..."
    git -c protocol.file.allow=always submodule update --init --recursive
else
    echo "note: not a git repo; skipping submodule init. Ensure WDL/, WavPack/, libambix/ are populated." >&2
fi

# A non-Apple platform might prefer Ninja; macOS uses Unix Makefiles by
# default so the build_osx.sh + Xcode workflows both work without a
# generator switch. Honor CMAKE_GENERATOR if the caller set it.
if [ "$(uname)" = "Darwin" ]; then
    GENERATOR="${CMAKE_GENERATOR:-Unix Makefiles}"
else
    GENERATOR="${CMAKE_GENERATOR:-Ninja}"
fi

BUILD_DIR="${BUILD_DIR:-build-dev}"

cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=MinSizeRel

echo ">>> Done."
echo ">>> Dev rebuild (auto-installs into REAPER UserPlugins):"
echo "      cmake --build $BUILD_DIR -j"
echo ">>> Signed installer:"
echo "      ./scripts/build_osx.sh           (macOS .pkg)"
echo "      scripts\\build_win.bat           (Windows .exe)"
