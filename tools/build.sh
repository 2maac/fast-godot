#!/bin/sh
# Build editor and export templates.
#
#   PLATFORM=linuxbsd ./tools/build.sh        # linux
#   PLATFORM=windows  ./tools/build.sh        # windows (from a VS developer prompt)
#   ./tools/build.sh                          # macos (default)
#
# Extra SCons flags are passed through: ./tools/build.sh lto=full vulkan=yes
set -e
cd "$(dirname "$0")/.."
PLATFORM="${PLATFORM:-macos}"
JOBS="${JOBS:-10}"

scons platform="$PLATFORM" target=editor           -j"$JOBS" "$@"
scons platform="$PLATFORM" target=template_debug   -j"$JOBS" "$@"
scons platform="$PLATFORM" target=template_release -j"$JOBS" "$@"
