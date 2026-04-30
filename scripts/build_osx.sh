#!/bin/bash
# Build, codesign, notarize, and package reaper_ambix for macOS distribution.
#
# Usage:  ./scripts/build_osx.sh [--no-sign]
#
# Output: signed + notarized .pkg installer in _OSX_RELEASE/ that drops
#   /Library/Application Support/REAPER/UserPlugins/reaper_ambix.dylib
#         (statically linked: libambix + WavPack + WDL — no external deps)
#
# Pipeline (mirrors sonolink/scripts/build_osx.sh):
#   1. cmake configure (REAPER_AMBIX_INSTALL_USER_PLUGINS=OFF) + parallel build
#   2. recursively bundle any non-system dynamic deps (defensive — currently
#      none are expected since libambix and WavPack are statically linked)
#   3. codesign the plugin (and any bundled deps) with Developer ID Application
#      + hardened runtime
#   4. pkgbuild the staging tree as a flat installer
#   5. productsign with Developer ID Installer
#   6. notarytool submit (--wait) + stapler staple
#   7. spctl assess for sanity

set -e

ROOT=$(cd "$(dirname "$0")/.."; pwd)
BUILD_DIR="$ROOT/_build"
STAGE_DIR="$BUILD_DIR/stage"
LIBS_SUBDIR="reaper_ambix-libs"
PLUGIN_NAME="reaper_ambix.dylib"
INSTALL_PARENT="/Library/Application Support/REAPER/UserPlugins"
VERSION=$(<"$ROOT/VERSION")

# Parse arguments
SKIP_SIGN=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-sign) SKIP_SIGN=true ;;
        *) echo "Unknown option: $1"; echo "Usage: $0 [--no-sign]"; exit 1 ;;
    esac
    shift
done

# Load codesigning credentials (skip if --no-sign)
if ! $SKIP_SIGN; then
    CODESIGN_ENV="$ROOT/scripts/codesign.env"
    if [ ! -f "$CODESIGN_ENV" ]; then
        echo "Error: $CODESIGN_ENV not found. Create it with your codesigning credentials."
        exit 1
    fi
    # shellcheck disable=SC1090
    source "$CODESIGN_ENV"
fi

# =========================================================
# Helper: recursively bundle a dylib's @rpath / Homebrew dependencies into a
# flat directory, rewriting their LC_LOAD_DYLIB entries to @loader_path/. The
# plugin (which does NOT live in the same dir as the dylibs) gets its
# load command rewritten to @loader_path/<LIBS_SUBDIR>/<libname>.
# =========================================================

# Heuristic: do we want to bundle THIS dylib?
#   yes  - non-system 3rd-party libs (Homebrew, /usr/local, @rpath/lib*.dylib)
#   no   - macOS system libs/frameworks, @loader_path/@executable_path (already
#          relative), and the plugin's self-reference (reaper_ambix.dylib).
should_bundle() {
    local p="$1"
    case "$p" in
        /usr/lib/*|/System/*|@loader_path/*|@executable_path/*)
            return 1 ;;
        */reaper_ambix.dylib|@rpath/reaper_ambix.dylib)
            return 1 ;;  # plugin's own LC_ID_DYLIB
    esac
    return 0
}

# Resolve a possibly-rpath-relative dep name to an absolute path so we can
# walk it. For a Homebrew install_name like /opt/homebrew/opt/foo/lib/...
# this is just the literal path.
resolve_dep() {
    local dep="$1"
    case "$dep" in
        @rpath/*)
            local name=${dep#@rpath/}
            for d in /opt/homebrew/lib /opt/homebrew/opt/*/lib /usr/local/lib; do
                [ -f "$d/$name" ] && { echo "$d/$name"; return 0; }
            done
            return 1 ;;
        *)
            [ -f "$dep" ] && { echo "$dep"; return 0; }
            return 1 ;;
    esac
}

LIBS_DIR=""  # set below; the presence of a file in $LIBS_DIR is also our
             # "already-bundled" sentinel (avoids needing bash 4 assoc arrays)

bundle_recursive() {
    local source_dylib="$1"
    local resolved
    resolved=$(resolve_dep "$source_dylib") || return 0
    local base
    base=$(basename "$resolved")

    # Already bundled? (file presence == visited)
    [ -f "$LIBS_DIR/$base" ] && return 0

    cp -L -X "$resolved" "$LIBS_DIR/$base"
    chmod u+w "$LIBS_DIR/$base"
    install_name_tool -id "@loader_path/$base" "$LIBS_DIR/$base"

    # Walk this lib's deps and rewrite each
    while IFS= read -r dep; do
        dep=$(echo "$dep" | awk '{print $1}')
        [ -z "$dep" ] && continue
        if should_bundle "$dep"; then
            local dep_base
            dep_base=$(basename "${dep##*/}")
            # also handle @rpath/foo.dylib
            [[ "$dep" == @rpath/* ]] && dep_base="${dep#@rpath/}"
            install_name_tool -change "$dep" "@loader_path/$dep_base" "$LIBS_DIR/$base"
            bundle_recursive "$dep"
        fi
    done < <(otool -L "$resolved" | tail -n +2 | awk '{print $1}' | grep -v "^${resolved}$" || true)
}

# Repoint a non-libs target (the plugin .dylib) at @loader_path/<LIBS_SUBDIR>/
relink_plugin() {
    local plug="$1"
    while IFS= read -r dep; do
        dep=$(echo "$dep" | awk '{print $1}')
        [ -z "$dep" ] && continue
        if should_bundle "$dep"; then
            local dep_base=$(basename "${dep##*/}")
            [[ "$dep" == @rpath/* ]] && dep_base="${dep#@rpath/}"
            install_name_tool -change "$dep" "@loader_path/${LIBS_SUBDIR}/${dep_base}" "$plug"
        fi
    done < <(otool -L "$plug" | tail -n +2 | awk '{print $1}')
}

codesign_one() {
    local target="$1"
    codesign -s "$CODESIGN_APP" \
             --force --strict --timestamp --options=runtime \
             "$target"
}

# =========================================================
# Clean & prepare
# =========================================================

echo ""; echo "=== reaper_ambix v$VERSION — macOS installer build ==="

rm -rf "$BUILD_DIR" 2>/dev/null || sudo rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$STAGE_DIR$INSTALL_PARENT"
mkdir -p "$ROOT/_OSX_RELEASE"

# =========================================================
# Configure + build
# =========================================================

echo ""; echo "=== Configuring (Release, no user-plugin install) ==="
cmake -S "$ROOT" -B "$BUILD_DIR" \
      -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DREAPER_AMBIX_INSTALL_USER_PLUGINS=OFF \
      -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$BUILD_DIR/out"

echo ""; echo "=== Building ==="
cmake --build "$BUILD_DIR" -j "$(sysctl -n hw.logicalcpu)"

PLUGIN_BUILT="$BUILD_DIR/out/$PLUGIN_NAME"
if [ ! -f "$PLUGIN_BUILT" ]; then
    echo "Error: build did not produce $PLUGIN_BUILT"
    exit 1
fi

# =========================================================
# Stage + bundle dependencies
# =========================================================

PLUGIN_STAGED="$STAGE_DIR$INSTALL_PARENT/$PLUGIN_NAME"
LIBS_DIR="$STAGE_DIR$INSTALL_PARENT/$LIBS_SUBDIR"

cp -X "$PLUGIN_BUILT" "$PLUGIN_STAGED"
chmod u+w "$PLUGIN_STAGED"
xattr -c "$PLUGIN_STAGED" 2>/dev/null || true
mkdir -p "$LIBS_DIR"

echo ""; echo "=== Bundling runtime dependencies ==="
# Walk the plugin's external dep list and recursively pull in each
while IFS= read -r dep; do
    dep=$(echo "$dep" | awk '{print $1}')
    [ -z "$dep" ] && continue
    if should_bundle "$dep"; then
        bundle_recursive "$dep"
    fi
done < <(otool -L "$PLUGIN_STAGED" | tail -n +2 | awk '{print $1}')

# Now rewrite the plugin's load commands to point into reaper_ambix-libs/
relink_plugin "$PLUGIN_STAGED"

# Drop any stale LC_RPATH entries (we no longer rely on Homebrew paths).
for rpath in $(otool -l "$PLUGIN_STAGED" | awk '/LC_RPATH/{getline;getline;print $2}'); do
    install_name_tool -delete_rpath "$rpath" "$PLUGIN_STAGED" 2>/dev/null || true
done
# Add @loader_path/<libs> as the only rpath, so anyone inspecting the
# plugin sees a clean self-contained dependency picture.
install_name_tool -add_rpath "@loader_path/${LIBS_SUBDIR}" "$PLUGIN_STAGED"

BUNDLED_COUNT=$(ls -1 "$LIBS_DIR" | wc -l | tr -d ' ')
echo ""; echo "=== Bundled dylibs ($BUNDLED_COUNT): ==="
ls -1 "$LIBS_DIR"

echo ""; echo "=== Plugin's resolved dep list ==="
otool -L "$PLUGIN_STAGED" | sed 's/^/  /'

# =========================================================
# Codesign every .dylib
# =========================================================

if ! $SKIP_SIGN; then
    echo ""; echo "=== Codesigning bundled libs ==="
    # Sign deps first (innermost), then the plugin (outermost).
    find "$LIBS_DIR" -type f -name "*.dylib" | while IFS= read -r f; do
        codesign_one "$f"
    done
    echo ""; echo "=== Codesigning plugin ==="
    codesign_one "$PLUGIN_STAGED"

    # Verify
    codesign --verify --deep --strict --verbose=2 "$PLUGIN_STAGED"
fi

# =========================================================
# pkgbuild + productsign + notarize
# =========================================================

UNSIGNED_PKG="$BUILD_DIR/reaper_ambix_v${VERSION}_macos_unsigned.pkg"
INSTALLER="$ROOT/_OSX_RELEASE/reaper_ambix_v${VERSION}_macos.pkg"

echo ""; echo "=== Building installer payload ==="
# install_name_tool stamps a com.apple.cs.CodeDirectory placeholder xattr on
# every modified Mach-O. pkgbuild's pax-format payload then encodes those as
# paired AppleDouble shadow files (._<libname>.dylib, ...). Strip xattrs
# AFTER install_name_tool runs and BEFORE the final stage copy.
xattr -cr "$STAGE_DIR"
find "$STAGE_DIR" -name "._*" -delete

# Re-stage via `ditto` into a fresh tree with NO xattrs / no resource forks /
# no quarantine — defense in depth.
CLEAN_STAGE="$BUILD_DIR/clean_stage"
rm -rf "$CLEAN_STAGE"
mkdir -p "$CLEAN_STAGE"
ditto --norsrc --noextattr --noacl --noqtn "$STAGE_DIR" "$CLEAN_STAGE"
find "$CLEAN_STAGE" -name "._*" -delete
xattr -cr "$CLEAN_STAGE"

COPYFILE_DISABLE=1 \
COPY_EXTENDED_ATTRIBUTES_DISABLE=1 \
pkgbuild --root "$CLEAN_STAGE" \
         --identifier "com.kronlachner.reaper_ambix" \
         --version "$VERSION" \
         --install-location "/" \
         --ownership recommended \
         "$UNSIGNED_PKG"

if $SKIP_SIGN; then
    mv "$UNSIGNED_PKG" "$INSTALLER"
    echo "Note: package is NOT signed (--no-sign)"
else
    productsign --sign "$CODESIGN_INSTALLER" "$UNSIGNED_PKG" "$INSTALLER"
    rm -f "$UNSIGNED_PKG"
    pkgutil --check-signature "$INSTALLER"

    echo ""; echo "=== Notarizing $(basename "$INSTALLER") ==="
    xcrun notarytool submit "$INSTALLER" \
          --apple-id "$NOTARIZE_APPLE_ID" \
          --password "$NOTARIZE_PASSWORD" \
          --team-id  "$NOTARIZE_TEAM_ID" \
          --wait

    xcrun stapler staple "$INSTALLER"

    echo ""; echo "=== Verifying installer ==="
    stapler validate "$INSTALLER"
    spctl -a -vvv --assess --type install "$INSTALLER"
fi

echo ""; echo "Done!"
ls -la "$INSTALLER"
