#!/bin/bash
# Build a Web export template from this tree with the WebGPU driver.
#
# Usage: ./tools/build_webgpu_template.sh            # template_debug
#        MODE=release ./tools/build_webgpu_template.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="${FAST_GODOT_TOOLCHAIN:-$HOME/fast_godot_toolchain}"
MODE="${MODE:-debug}"

case "$MODE" in
debug) TARGET=template_debug ;;
release) TARGET=template_release ;;
*)
    echo "MODE must be 'debug' or 'release' (got '$MODE')" >&2
    exit 1
    ;;
esac

# shellcheck disable=SC1091
source "$TOOLCHAIN/emsdk/emsdk_env.sh" >/dev/null 2>&1
cd "$ROOT"
PYTHONPATH="$TOOLCHAIN/scons/4.11.1/scons-4.11.1" \
  "$TOOLCHAIN/python/bin/python3" -m SCons \
  platform=web target="$TARGET" webgpu=yes opengl3=no threads=no dlink_enabled=yes \
  -j"${JOBS:-10}"
