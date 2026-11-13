#!/usr/bin/env bash
# Prints the scons executable to use for this tree, or exits 1 if there is none.
#
#   grit_scons_bin.sh <engine_root>
#
# Order:
#   1. $GRIT_SCONS, if it points at something executable.
#   2. bin/grit/scons_path.txt — written by grit_make_manifest.sh. This exists
#      because the editor does not necessarily inherit a shell environment: an
#      export started from the GUI (or from Finder) has no venv on PATH, and
#      `OS::execute`/`create_process` only pass on what the editor itself got.
#      Gotcha #3 in HANDOVER.md is the same failure, one level down.
#   3. scons on PATH.
#
# The path is recorded rather than hardcoded so the repository stays portable.
set -euo pipefail
ENGINE="${1:?usage: grit_scons_bin.sh <engine_root>}"

is_exe() {
	[ -n "${1:-}" ] && [ -x "${1:-}" ]
}

if is_exe "${GRIT_SCONS:-}"; then
	printf '%s\n' "${GRIT_SCONS}"
	exit 0
fi

CONFIG="$ENGINE/bin/grit/scons_path.txt"
if [ -s "$CONFIG" ]; then
	CANDIDATE="$(head -1 "$CONFIG")"
	if is_exe "$CANDIDATE"; then
		printf '%s\n' "$CANDIDATE"
		exit 0
	fi
fi

if FOUND="$(command -v scons 2>/dev/null)"; then
	printf '%s\n' "$FOUND"
	exit 0
fi

exit 1
