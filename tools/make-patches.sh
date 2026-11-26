#!/usr/bin/env bash
# Regenerate the patch series from a working tree that already contains our
# changes on top of the pinned Godot commit.
#
#   tools/make-patches.sh <godot-source-dir> [stack]
#
# The split into layers comes from patches/<stack>/layers/*.files (path lists).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: $0 <godot-source-dir> [stack]}
STACK=${2:-4.8-beta5}
DIR="$ROOT/patches/$STACK"
PINNED=$(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' "$ROOT/upstream.lock")
[ -d "$SRC/.git" ] || { echo "$SRC is not a git checkout" >&2; exit 2; }

mkdir -p "$DIR"
for list in "$DIR"/layers/*.files; do
	name=$(basename "$list" .files)
	mapfile -t paths < <(grep -v '^$' "$list")
	git -C "$SRC" diff "$PINNED" -- "${paths[@]}" > "$DIR/$name.patch" 2>/dev/null || \
		git -C "$SRC" diff HEAD -- "${paths[@]}" > "$DIR/$name.patch"
	echo "$name.patch $(wc -c < "$DIR/$name.patch") bytes"
done
