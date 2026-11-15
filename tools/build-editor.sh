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
[ -x "$python" ] || python=python3
exec "$python" "$engine/tools/grit_build.py" editor --engine "$engine" --toolchain "$here" "$@"
