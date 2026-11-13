#!/usr/bin/env bash
# Builds an export template that contains a project's generated Grit code.
#   grit_build_template.sh <engine_root> <generated_dir> <release|debug> <out_binary> [platform] <profile_json>
#
# The profile is mandatory and is what enables Grit in the template: the module
# only builds when grit_generated is set, so plain templates are stock Godot.
# bin/grit/<target>/flags.txt is optional and only supplies -fmodules-cache-path.
set -euo pipefail
# Windows runs this script through the bundled shell (Git's sh), so nothing may
# assume bash, GNU coreutils, rsync or sleep. Each need has a fallback below.
SH="${GRIT_SH:-sh}"
PYTHON_BIN="${GRIT_PYTHON:-}"
hash_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	elif [ -n "$PYTHON_BIN" ]; then
		"$PYTHON_BIN" -c 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
	else
		echo "Grit: no sha256 tool available" >&2
		exit 8
	fi
}
nap() {
	if command -v sleep >/dev/null 2>&1; then
		sleep "$1"
	elif [ -n "$PYTHON_BIN" ]; then
		"$PYTHON_BIN" -c 'import sys,time;time.sleep(float(sys.argv[1]))' "$1"
	else
		: # no timer; the caller's loop still makes progress
	fi
}
ENGINE="$1"; GEN="$2"; TARGET="$3"; OUT="$4"; PLATFORM="${5:-macos}"; PROFILE="${6:-}"
ARCH="${7:-arm64}"
M="$ENGINE/bin/grit/$TARGET"
mkdir -p "$M"
# Everything this script prints goes to a log the editor tails for progress.
LOG="$M/last_build.log"
: > "$LOG"
exec >>"$LOG" 2>&1
[ -d "$GEN" ] || { echo "Grit: no generated code in $GEN" >&2; exit 3; }
[ -n "$PROFILE" ] || { echo "Grit: missing build profile argument" >&2; exit 2; }
cd "$ENGINE"

# A build profile changes module flags and regenerates headers *in the source
# tree* (core/disabled_classes.gen.h, modules/modules_enabled.gen.h). Running it
# here rewrites what the editor's already-compiled objects were built against,
# which is how the 493-class editor happened. So the profile build runs in an
# isolated copy and only the finished template comes back.
[ -f "$PROFILE" ] || { echo "Grit: build profile not found: $PROFILE" >&2; exit 4; }
JOBS="${GRIT_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-8}")}"
# The editor's environment may not have scons on PATH; see grit_scons_bin.sh.
SCONS_BIN="$("$SH" "$ENGINE/tools/grit_scons_bin.sh" "$ENGINE" || true)"
[ -n "$SCONS_BIN" ] || { echo "Grit: scons not found. Run tools/grit_make_manifest.sh (it records bin/grit/scons_path.txt), or set GRIT_SCONS." >&2; exit 7; }
# Rebuild with the same configuration the engine was built with. Extra SCons
# options (vulkan_sdk_path, accesskit, ...) come from scons_args.txt next to
# the manifest, or are detected from the Vulkan SDK; see grit_scons_args.sh.
SCONS_ARGS="$("$SH" "$ENGINE/tools/grit_scons_args.sh" "$ENGINE" "$TARGET")"
# A full rebuild must use the same compile flags as the tree the manifests
# were captured from. -fmodules-cache-path matters most: inside a sandbox the
# default per-user module cache is not writable, and the build dies with
# "could not build module 'std'" / "Operation not permitted" on the .pcm files.
if [ -f "$M/flags.txt" ] && [ "$PLATFORM" != "windows" ]; then
	MODCACHE="$(grep -o -- '-fmodules-cache-path=[^ ]*' "$M/flags.txt" | head -1 || true)"
	if [ -n "$MODCACHE" ]; then
		SCONS_ARGS="$SCONS_ARGS ccflags=$MODCACHE"
	fi
fi

# One work tree per (profile content, target). The tree is refreshed from this
# checkout on every run rather than keyed by revision: keying on HEAD threw the
# tree away after every commit and paid a full ~2 min build again. With a
# refresh, a commit only recompiles the files that changed. The copy keeps its
# own bin/ and SCons state, so it is never touched by the sync.
PROFILE_HASH="$(hash_of "$PROFILE" | cut -c1-16)"
WORK_ROOT="${GRIT_PROFILE_WORKDIR:-$ENGINE/out/grit-profiles}"
# Platform and arch belong in the key: a Windows tree and a macOS tree cannot
# share objects, and before this they did.
SRC="$WORK_ROOT/${PROFILE_HASH}-${PLATFORM}-${ARCH}-${TARGET}"
if [ ! -d "$SRC" ]; then
	echo "Grit: seeding isolated tree $SRC (first build for this profile is a full one)"
	mkdir -p "$SRC"
else
	echo "Grit: refreshing isolated tree $SRC"
fi
# --delete removes files this checkout no longer has; excluded paths (bin/,
# out/, .git, SCons state) are left alone. compile_commands.json and the
# .scons* files must not be copied in: they describe this tree's build, not
# the isolated one's. Nor may *.gen.* be copied: those are generated per
# configuration (core/disabled_classes.gen.h, modules/modules_enabled.gen.h,
# core/version_hash.gen.cpp, ...), and overwriting this tree's copies with the
# other tree's made SCons recompile their dependents on every single run.
# tar instead of rsync: Windows' bundled shell has no rsync. It overwrites changed
# files and keeps the isolated tree's own bin/ and SCons state, which is what
# makes a rebuild incremental. Stale files are left behind rather than deleted.
( cd "$ENGINE" && tar -cf - \
	--exclude='.git' --exclude='*.app' --exclude='.godot' \
	--exclude='bin' --exclude='out' --exclude='.scons*' \
	--exclude='*.gen.*' --exclude='compile_commands.json' . ) | ( cd "$SRC" && tar -xf - )
echo "Grit: rebuild with build profile in $SRC"

# The editor asks for a cancel by creating this file; the helper owns the
# killing so no outside process has to signal our children.
CANCEL_FILE="$M/cancel_build"
rm -f "$CANCEL_FILE"
( cd "$SRC" && exec "$SCONS_BIN" platform="$PLATFORM" target="template_$TARGET" arch="$ARCH" \
	build_profile="$PROFILE" grit_generated="$GEN" -j"$JOBS" $SCONS_ARGS ) &
SPID=$!
echo "$SPID" > "$M/scons.pid"
while kill -0 "$SPID" 2>/dev/null; do
	if [ -e "$CANCEL_FILE" ]; then
		echo "Grit: cancel requested, stopping scons"
		kill -9 "$SPID" 2>/dev/null || true
		rm -f "$CANCEL_FILE" "$M/scons.pid"
		exit 3
	fi
	nap 0.2
done
wait "$SPID" || exit 5
rm -f "$M/scons.pid"
case "$PLATFORM" in
	macos) BIN="bin/godot.macos.template_${TARGET}.${ARCH}" ;;
	windows) BIN="bin/godot.windows.template_${TARGET}.${ARCH}.exe" ;;
	*) BIN="bin/godot.linuxbsd.template_${TARGET}.${ARCH}" ;;
esac
[ -f "$SRC/$BIN" ] || { echo "Grit: build finished but $SRC/$BIN is missing" >&2; exit 6; }
cp "$SRC/$BIN" "$OUT"
count="profile"

# macOS export templates are a zip whose root is macos_template.app, and the
# exporter looks for a version-suffixed binary inside Contents/MacOS.
if [ "$PLATFORM" = "macos" ]; then
	STAGE="$M/stage"
	rm -rf "$STAGE"; mkdir -p "$STAGE"
	cp -R "$ENGINE/misc/dist/macos_template.app" "$STAGE/macos_template.app"
	mkdir -p "$STAGE/macos_template.app/Contents/MacOS"
	# Provide both names so the exporter finds one regardless of its debug flag.
	cp "$OUT" "$STAGE/macos_template.app/Contents/MacOS/godot_macos_release.arm64"
	cp "$OUT" "$STAGE/macos_template.app/Contents/MacOS/godot_macos_debug.arm64"
	chmod +x "$STAGE/macos_template.app/Contents/MacOS/"*
	ditto -c -k --norsrc --noextattr --keepParent "$STAGE/macos_template.app" "$OUT.zip"
	rm -rf "$STAGE" "$OUT"
	OUT="$OUT.zip"
fi
echo "Grit: compiled $count generated file(s) and produced $OUT"
