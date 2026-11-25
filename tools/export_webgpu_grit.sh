#!/bin/bash
# Export test_project through the Grit "HTML5 Fast" path.
#
# Unlike tools/export_webgpu_test.sh (which only needs a template built by
# tools/build_webgpu_template.sh and the stock Godot editor), this path builds a
# per-project wasm template from the engine source and links the project's
# Grit-compiled code into it. It needs:
#   - an editor built from this tree (bin/godot.macos.editor.arm64), so the web
#     export knows about the WebGPU rendering driver, and
#   - the toolchain's engine source pointed at this checkout, because Grit builds
#     templates from $TOOLCHAIN/engine/source rather than from the editor's own
#     tree (see WEBGPU_HANDOFF.md, "Grit HTML5 Fast export").
#
# Usage: ./tools/export_webgpu_grit.sh [debug|release]   (default: release)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/test_project"
EDITOR="${GODOT_EDITOR:-$ROOT/bin/godot.macos.editor.arm64}"
MODE="${1:-release}"
OUT_DIR="$PROJECT/build-fast"

if [ ! -x "$EDITOR" ]; then
    echo "Missing editor: $EDITOR" >&2
    echo "Build it with:" >&2
    echo "  ~/fast_godot_toolchain/python/bin/python3 tools/grit_build.py editor \\" >&2
    echo "      --engine \"$ROOT\" --toolchain \"$HOME/fast_godot_toolchain\" --jobs 10" >&2
    exit 1
fi

case "$MODE" in
debug) EXPORT_FLAG="--export-debug" ;;
release) EXPORT_FLAG="--export-release" ;;
*)
    echo "MODE must be 'debug' or 'release' (got '$MODE')" >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"
# Exported files must not be scanned as project assets.
touch "$OUT_DIR/.gdignore"

# The preset must select a WebGPU rendering method: the HTML5 Fast template only
# contains the WebGPU renderer, and the export platform refuses gl_compatibility.
"$EDITOR" --headless --path "$PROJECT" "$EXPORT_FLAG" "HTML5 Fast" "$OUT_DIR/index.html"

echo "[export] done (HTML5 Fast, $MODE): $OUT_DIR"
echo "[export] serve it with: cd \"$OUT_DIR\" && python3 -m http.server 8125 --bind 127.0.0.1"
