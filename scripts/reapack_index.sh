#!/usr/bin/env bash
# Regenerate the ReaPack index (index.xml at the repo root) from the package
# metadata in Extensions/*.ext plus the git history.
#
# Requires cfillion's reapack-index tool, which needs Ruby >= 3.2 (newer than
# the Ruby macOS ships) and pandoc for the RTF description:
#   brew install ruby pandoc
#   /opt/homebrew/opt/ruby/bin/gem install reapack-index
# The cfillion/reapack Homebrew tap referenced by older docs no longer exists.
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
    echo "Install it with (needs Ruby >= 3.2, newer than macOS ships):"
    echo "    brew install ruby pandoc"
    echo "    /opt/homebrew/opt/ruby/bin/gem install reapack-index"
    echo "then make sure its bin directory is on your PATH, e.g.:"
    echo "    export PATH=\"\$(brew --prefix ruby)/../../lib/ruby/gems/4.0.0/bin:\$PATH\""
    exit 1
fi

# --no-commit: this repo commits index.xml by hand (see README, "Cutting a
# release"), and the interactive prompt reapack-index shows otherwise dies
# with Errno::ENODEV when stdin is not a terminal.
reapack-index --name 'reaper_ambix' --no-commit

echo ""
echo "index.xml regenerated - review the diff and commit it."
