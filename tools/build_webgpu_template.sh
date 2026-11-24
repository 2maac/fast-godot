#!/bin/bash
# Build a debug Web export template from this tree with the WebGPU driver.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="${FAST_GODOT_TOOLCHAIN:-$HOME/fast_godot_toolchain}"
# shellcheck disable=SC1091
source "$TOOLCHAIN/emsdk/emsdk_env.sh" >/dev/null 2>&1
cd "$ROOT"
PYTHONPATH="$TOOLCHAIN/scons/4.11.1/scons-4.11.1" \
  "$TOOLCHAIN/python/bin/python3" -m SCons \
  platform=web target=template_debug webgpu=yes opengl3=no threads=no dlink_enabled=yes \
  -j"${JOBS:-10}"
