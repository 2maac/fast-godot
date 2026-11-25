#!/bin/sh
# Builds the Fast_Godot editor with the toolchain this script lives in.
# The toolchain and the source tree can be anywhere: the source is found from
# -Engine/GRIT_ENGINE, the current directory, the toolchain's parents or common
# locations. Extra arguments are passed to grit_build.py.

set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

engine=${GRIT_ENGINE:-${GRIT_SRC:-}}
while [ $# -gt 0 ]; do
	case "$1" in
		-Engine|--engine) engine=$2; shift 2 ;;
		-Toolchain|--toolchain) here=$2; shift 2 ;;
		*) break ;;
	esac
done

if [ -z "$engine" ]; then
	for start in "$PWD" "$here" "$here/.." "$here/../.."; do
		dir=$(CDPATH= cd -- "$start" 2>/dev/null && pwd) || continue
		while [ "$dir" != "/" ] && [ "$dir" != "." ]; do
			if [ -f "$dir/tools/grit_build.py" ]; then engine=$dir; break; fi
			dir=$(dirname -- "$dir")
		done
		[ -n "$engine" ] && break
	done
fi
if [ -z "$engine" ]; then
	for root in "${HOME:-}" "${HOME:-}/src" "${HOME:-}/dev" /opt /tmp; do
		[ -d "$root" ] || continue
		found=$(find "$root" -maxdepth 3 -type f -name grit_build.py -path "*/tools/*" 2>/dev/null | head -n 1)
		if [ -n "$found" ]; then engine=$(dirname -- "$(dirname -- "$found")"); break; fi
	done
fi
if [ -z "$engine" ]; then
	echo "no Fast_Godot source tree found (needs tools/grit_build.py); set GRIT_ENGINE" >&2
	exit 1
fi

python="$here/python/bin/python3"
if [ ! -x "$python" ]; then
	# Running from the source tree rather than the toolchain: fall back to a
	# known toolchain location so the bundled Python/SCons are still used.
	for t in "${GRIT_TOOLCHAIN:-}" "${FAST_GODOT_TOOLCHAIN:-}" "$HOME/fast_godot_toolchain"; do
		if [ -n "$t" ] && [ -x "$t/python/bin/python3" ]; then
			here=$t
			python="$here/python/bin/python3"
			break
		fi
	done
fi
[ -x "$python" ] || python=python3
"$python" "$engine/tools/grit_build.py" editor --engine "$engine" --toolchain "$here" "$@"

# Every editor build is staged into out/editor-staging so it is always the
# freshest binary that gets installed/tested. Skip with STAGE_EDITOR=0.
if [ "$(uname -s)" = "Darwin" ] && [ "${STAGE_EDITOR:-1}" != "0" ] && [ -x "$engine/tools/stage_editor.sh" ]; then
	"$engine/tools/stage_editor.sh"
fi
