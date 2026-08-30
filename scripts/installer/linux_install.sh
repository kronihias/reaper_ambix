#!/usr/bin/env bash
# Install reaper_ambix into the user's REAPER UserPlugins folder.
# Run from inside the extracted tarball:  ./install.sh
#
# Override the destination with REAPER_USERPLUGINS=/path ./install.sh

set -e

HERE=$(cd "$(dirname "$0")"; pwd)
PLUGIN="reaper_ambix.so"
DEST="${REAPER_USERPLUGINS:-$HOME/.config/REAPER/UserPlugins}"

if [ ! -f "$HERE/$PLUGIN" ]; then
    echo "Error: $PLUGIN not found next to this script." >&2
    exit 1
fi

mkdir -p "$DEST"
cp "$HERE/$PLUGIN" "$DEST/$PLUGIN"

echo "Installed $PLUGIN to:"
echo "  $DEST/$PLUGIN"
echo
echo "Restart REAPER to load the extension."
