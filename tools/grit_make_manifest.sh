#!/usr/bin/env bash
# Regenerates bin/grit/<target>/{flags.txt,link.txt} from the current build of
# this tree. Run it after building the target and after code changes that alter
# compiler flags.
set -euo pipefail
ENGINE="${1:?usage: grit_make_manifest.sh <engine_root> <release|debug>}"
TARGET="${2:?}"
cd "$ENGINE"
OUT="bin/grit/$TARGET"; mkdir -p "$OUT"
[ -f compile_commands.json ] || { echo "run scons with compiledb=yes first" >&2; exit 2; }
python3 - "$TARGET" "$OUT/flags.txt" <<'PY'
import json, shlex, sys
target, out = sys.argv[1], sys.argv[2]
db = json.load(open("compile_commands.json"))
entry = next((e for e in db if e["file"].endswith("modules/grit/runtime/grit_registry.cpp")), None)
if entry is None:
    sys.exit("Grit: compile_commands.json has no entry for modules/grit/runtime/grit_registry.cpp;\n"
             "run scons with compiledb=yes for this target first")
args = entry.get("arguments") or shlex.split(entry["command"])
res, i = [], 0
while i < len(args):
    if args[i] in ("-c", "-o"):
        i += 2
        continue
    res.append(args[i]); i += 1
res = [a for a in res if not a.endswith((".cpp", ".c", ".mm"))]
joined = " ".join(res)
# compile_commands.json only ever holds the target captured last, so building
# the editor after the template happily hands us editor flags. Compiling the
# generated code with those links a debug-style object into a release template,
# which is subtle to diagnose later — refuse instead.
is_debug_flavor = "-DDEBUG_ENABLED" in joined
is_release_flavor = "-DNDEBUG" in joined and not is_debug_flavor
matches = (not is_debug_flavor) if target == "release" else (is_debug_flavor and not is_release_flavor)
if not matches:
    sys.exit("Grit: compile_commands.json holds %s flags, not target=template_%s flags.\n"
             "Run: scons platform=macos target=template_%s arch=arm64 compiledb=yes\n"
             "then re-run this script." % ("debug/editor" if is_debug_flavor else "release", target, target))
open(out, "w").write(" ".join(shlex.quote(a) for a in res) + " -std=c++17")
PY
# The editor's environment may not have scons on PATH; see grit_scons_bin.sh.
SCONS_BIN="$(bash "$ENGINE/tools/grit_scons_bin.sh" "$ENGINE" || true)"
[ -n "$SCONS_BIN" ] || { echo "Grit: scons not found; set GRIT_SCONS to the scons executable" >&2; exit 3; }
# Record it for export builds started from the editor, whose environment does
# not include the venv that ran this script.
mkdir -p bin/grit
printf '%s\n' "$SCONS_BIN" > bin/grit/scons_path.txt

# Configure the dry run exactly like the real build. Without the same
# vulkan_sdk_path, scons can fail to detect MoltenVK and print no link line at
# all — which used to look like "could not capture the link command".
SCONS_ARGS="$(bash "$ENGINE/tools/grit_scons_args.sh" "$ENGINE" "$TARGET")"

# A dry run only prints the link command when the binary is considered stale.
BIN="bin/godot.macos.template_$TARGET.arm64"
if [ -f "$BIN" ]; then
	mv "$BIN" "$BIN.bak"
	trap 'mv "$BIN.bak" "$BIN"' EXIT
fi
"$SCONS_BIN" platform=macos target="template_$TARGET" arch=arm64 -n verbose=yes $SCONS_ARGS \
	> "$OUT/.link.out" 2> "$OUT/.link.err"
{ grep '^clang++ -o bin/godot\.' "$OUT/.link.out" | head -1 > "$OUT/.link.raw"; } || true
rm -f "$OUT/.link.out"
if [ ! -s "$OUT/.link.raw" ]; then
	echo "Grit: could not capture the link command; scons said:" >&2
	tail -20 "$OUT/.link.err" >&2
	rm -f "$OUT/.link.err"
	exit 4
fi
rm -f "$OUT/.link.err"
python3 - "$OUT/link.txt" "$OUT/.link.raw" <<'PY'
import sys
cmd = open(sys.argv[2]).read().strip().split()
cmd[2] = "__OUT__"
i = next(k for k, t in enumerate(cmd) if t.endswith(".a"))
cmd[i:i] = ["__OBJS__"]
open(sys.argv[1], "w").write(" ".join(cmd))
PY
rm -f "$OUT/.link.raw"
echo "Grit: manifest written to $OUT"
