#!/usr/bin/env bash
# Regenerate the ReaPack index (index.xml at the repo root) from the package
# metadata in Extensions/*.ext plus the git history.
#
# Requires cfillion's reapack-index tool:
#   gem install reapack-index
#   # or: brew tap cfillion/reapack && brew install reapack-index
#
# Usage: run AFTER committing a @version bump in
#        Extensions/reaper_ambix.ext, then commit the regenerated index.xml.
#        reapack-index reads committed history, so the metadata must already be
#        committed for the new version to appear.

set -e

ROOT=$(cd "$(dirname "$0")/.."; pwd)
cd "$ROOT"

if ! command -v reapack-index >/dev/null 2>&1; then
    echo "Error: reapack-index not found."
    echo "Install it with:  gem install reapack-index"
    echo "             or:  brew tap cfillion/reapack && brew install reapack-index"
    exit 1
fi

reapack-index --name 'reaper_ambix'

echo ""
echo "index.xml regenerated - review the diff and commit it."
