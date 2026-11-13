#!/usr/bin/env bash
# Prints the extra SCons options needed to configure this tree the way it was
# actually built (vulkan_sdk_path, accesskit, ...), one line, for use as
# unquoted arguments.
#
#   grit_scons_args.sh <engine_root> <release|debug>
#
# Resolution order:
#   1. bin/grit/<target>/scons_args.txt, if it exists and is non-empty (the
#      per-machine override; gitignored).
#   2. A detected Vulkan SDK. detect_mvk() wants the SDK *root*, not .../macOS,
#      so both $VULKAN_SDK and ~/VulkanSDK/<version> are checked for
#      macOS/lib/MoltenVK.xcframework, newest version first.
#   3. $GRIT_SCONS_ARGS is appended to either of the above.
#
# Nothing is written back: the choice stays a function of this machine, so a
# stale hardcoded path can never outlive the SDK it points at.
#
# Sibling helper: grit_scons_bin.sh resolves the scons executable.

set -euo pipefail
ENGINE="${1:?usage: grit_scons_args.sh <engine_root> <release|debug>}"
TARGET="${2:?}"
M="$ENGINE/bin/grit/$TARGET"

if [ -s "$M/scons_args.txt" ]; then
	printf '%s %s\n' "$(cat "$M/scons_args.txt")" "${GRIT_SCONS_ARGS:-}" | sed 's/  */ /g; s/ $//'
	exit 0
fi

is_sdk_root() {
	[ -n "${1:-}" ] && [ -d "$1/macOS/lib/MoltenVK.xcframework" ]
}

args=()
if is_sdk_root "${VULKAN_SDK:-}"; then
	args+=("vulkan_sdk_path=${VULKAN_SDK}")
else
	shopt -s nullglob
	roots=("${HOME:-/nonexistent}"/VulkanSDK/*)
	shopt -u nullglob
	if [ ${#roots[@]} -gt 0 ]; then
		# Highest version wins; sort -V handles 1.9 vs 1.10 correctly.
		while IFS= read -r d; do
			if is_sdk_root "$d"; then
				args+=("vulkan_sdk_path=$d")
				break
			fi
		done < <(printf '%s\n' "${roots[@]}" | sort -V -r)
	fi
fi

if [ -n "${GRIT_SCONS_ARGS:-}" ]; then
	# shellcheck disable=SC2206 # deliberately word-split into separate options
	args+=(${GRIT_SCONS_ARGS})
fi

if [ ${#args[@]} -gt 0 ]; then
	printf '%s\n' "${args[*]}"
fi
exit 0
