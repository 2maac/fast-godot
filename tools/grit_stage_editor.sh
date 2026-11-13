#!/usr/bin/env bash
# Stages the built editor for testing, so feature tests never run the binary that
# sits in the build tree.
#
#   grit_stage_editor.sh <engine_root> [templates_dir]
#
# Why: a build profile or a plain template build regenerates shared headers in
# the source tree (core/disabled_classes.gen.h, modules/modules_enabled.gen.h).
# Objects live in per-target directories, but anything rebuilt while a different
# module set is on disk is compiled against that set — which is how a tree-shaken
# build once left a 493-class editor. A staged copy is immune: it was built and
# verified once, and nothing later in the tree can change it.
#
# The staging directory contains everything the editor needs to run:
#   Fast_Godot.app             a real app bundle (misc/dist/macos_tools.app plus
#                              the built binary at Contents/MacOS/Godot)
#   home/                      created by run.sh: a private HOME for tests, so the
#                              staged editor never shares config or shader cache
#   run.sh                     test launcher: same binary, HOME pinned to home/
#   VERIFIED                   git revision + class count it was verified with
#
# Override the location with GRIT_EDITOR_STAGE.
set -euo pipefail
ENGINE="${1:?usage: grit_stage_editor.sh <engine_root> [templates_dir]}"
TEMPLATES_SRC="${2:-${GRIT_TEMPLATES_SRC:-}}"
STAGE="${GRIT_EDITOR_STAGE:-$ENGINE/out/editor-staging}"
BIN="$ENGINE/bin/godot.macos.editor.arm64"

[ -f "$BIN" ] || {
	echo "Grit: $BIN not built yet (scons platform=macos target=editor)" >&2
	exit 1
}

REV="$(git -C "$ENGINE" rev-parse --short HEAD 2>/dev/null || echo unknown)"

APP_NAME="${GRIT_EDITOR_APP_NAME:-Fast_Godot}"
APP="$STAGE/$APP_NAME.app"
rm -rf "$STAGE"
mkdir -p "$STAGE"
# Stage the editor as the product ships: an app bundle, not a bare Mach-O.
cp -R "$ENGINE/misc/dist/macos_tools.app" "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/Godot"
chmod +x "$APP/Contents/MacOS/Godot"
# Ad-hoc sign: an unsigned bundle still runs locally, but LaunchServices is
# happier with a signature and it costs a second.
codesign --force --sign - "$APP" >/dev/null 2>&1 || echo "Grit: ad-hoc codesign failed (the bundle will still run locally)" >&2

# No export templates are staged: this fork keeps them inside the project
# (<project>/exports/), so the editor builds or installs one per project the first
# time a platform is added.

# Verify the copy, not the tree binary: this is the build under test.
cat > /tmp/grit_stage_check.gd <<'CHECK'
extends SceneTree
func _initialize() -> void:
	print("classes=", ClassDB.get_class_list().size())
	print("3d=", ClassDB.class_exists("StandardMaterial3D"))
	quit()
CHECK
# Throwaway HOME: verification must not write into the user's real config.
VERIFY_HOME="$(mktemp -d)"
CHECK_OUT="$(HOME="$VERIFY_HOME" "$APP/Contents/MacOS/Godot" --headless --script /tmp/grit_stage_check.gd 2>&1 | grep -E 'classes=|3d=' || true)"
CLASSES="$(printf '%s\n' "$CHECK_OUT" | sed -n 's/^classes=//p')"
echo "Grit: staged $STAGE (rev $REV)"
echo "$CHECK_OUT"

if [ "${CLASSES:-0}" -lt 1000 ] 2>/dev/null; then
	echo "Grit: WARNING staged editor reports ${CLASSES} classes; expected ~1077. The tree build is broken — rebuild before staging." >&2
	exit 2
fi
echo "Grit: staged editor verified (classes=$CLASSES)"

# The bundle has to be self-contained: nothing beside it, nothing pinned to this
# machine's paths inside it.
rm -rf "$VERIFY_HOME"
LEFT="$(ls "$STAGE" | grep -v "\.app$" || true)"
if [ -n "$LEFT" ]; then
	echo "Grit: WARNING the staging directory has entries besides the app: $LEFT" >&2
fi
