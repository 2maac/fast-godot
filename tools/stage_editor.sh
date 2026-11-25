#!/bin/bash
# Stage the editor built from this tree into the local staging app bundle.
#
# The staging bundle is a local, gitignored artifact (out/editor-staging) used
# for manually testing exports. Every editor rebuild should be staged here:
# this copies the binary over the bundle's executable, strips the Dropbox/Finder
# metadata that makes codesign fail, and ad-hoc re-signs the bundle.
#
# Usage: ./tools/stage_editor.sh
#        STAGE_DIR=/path/to/editor-staging ./tools/stage_editor.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="${STAGE_DIR:-$ROOT/out/editor-staging}"
APP="$STAGE/Fast_Godot.app"
SRC="$ROOT/bin/godot.macos.editor.arm64"
DST="$APP/Contents/MacOS/Godot"

if [ ! -x "$SRC" ]; then
    echo "Missing editor build: $SRC (build it with tools/grit_build.py editor)" >&2
    exit 1
fi
if [ ! -d "$APP" ]; then
    # Recreate the bundle skeleton from the editor app template. This keeps the
    # staging folder self-contained: a build can be staged even after the app was
    # moved out (e.g. copied to /Applications for a quick run).
    TEMPLATE="$ROOT/misc/dist/macos_tools.app"
    if [ ! -d "$TEMPLATE" ]; then
        echo "Missing staging app bundle: $APP (and template $TEMPLATE)" >&2
        exit 1
    fi
    echo "[stage] recreating $APP from $TEMPLATE"
    mkdir -p "$STAGE"
    ditto "$TEMPLATE" "$APP"
fi
mkdir -p "$APP/Contents/MacOS"

cp -p "$SRC" "$DST"

# Dropbox and Finder add xattrs/resource forks that make codesign refuse the
# bundle ("resource fork, Finder information, or similar detritus not allowed").
xattr -cr "$APP"
codesign --force --deep --sign - "$APP"
codesign --verify --strict "$APP"

VERSION="$("$DST" --headless --version 2>&1 | tail -1)"
echo "[stage] $DST"
echo "[stage] $VERSION"
