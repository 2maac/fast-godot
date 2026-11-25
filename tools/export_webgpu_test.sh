#!/bin/bash
# Export test_project for Web against the WebGPU-enabled template built here.
# The stock 4.7.2 editor is only an export front-end: the engine inside the
# template is this tree's fast-godot + WebGPU build.
#
# Usage: ./tools/export_webgpu_test.sh            # debug template (default)
#        MODE=release ./tools/export_webgpu_test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/test_project"
EDITOR="${GODOT_EDITOR:-/tmp/godot-stock/Godot.app/Contents/MacOS/Godot}"
MODE="${MODE:-debug}"

case "$MODE" in
debug)
    TEMPLATE="$ROOT/bin/godot.web.template_debug.wasm32.nothreads.dlink.zip"
    EXPORT_FLAG="--export-debug"
    ;;
release)
    TEMPLATE="$ROOT/bin/godot.web.template_release.wasm32.nothreads.dlink.zip"
    EXPORT_FLAG="--export-release"
    ;;
*)
    echo "MODE must be 'debug' or 'release' (got '$MODE')" >&2
    exit 1
    ;;
esac

if [ ! -f "$TEMPLATE" ]; then
    echo "Missing $MODE template: $TEMPLATE" >&2
    echo "Build it with: ./tools/build_webgpu_template.sh" >&2
    exit 1
fi

mkdir -p "$PROJECT/build"
# Exported files must not be scanned as project assets.
touch "$PROJECT/build/.gdignore"
"$EDITOR" --headless --path "$PROJECT" --import >/tmp/webgpu_import.log 2>&1 || true
"$EDITOR" --headless --path "$PROJECT" "$EXPORT_FLAG" "Web" "$PROJECT/build/index.html" >/tmp/webgpu_export.log 2>&1

# The stock editor predates rendering_device/driver.web, so its HTML shell does
# not emit renderingDriver. Tell the JS loader to pre-initialize the GPU device.
python3 - "$PROJECT/build/index.html" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding='utf-8') as f:
    text = f.read()
needle = "const GODOT_THREADS_ENABLED = false;"
if "GODOT_CONFIG['renderingDriver'] = 'webgpu';" not in text:
    if needle not in text:
        raise SystemExit("unexpected HTML shell: thread flag not found")
    text = text.replace(needle, needle + "\nGODOT_CONFIG['renderingDriver'] = 'webgpu';", 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
print("[export] renderingDriver=webgpu injected into index.html")
PY

echo "[export] done ($MODE): $PROJECT/build"
