#!/usr/bin/env bash
# Regenerate the patch series from a working tree that already contains our
# changes on top of the pinned Godot commit.
#
#   tools/make-patches.sh <godot-source-dir>
#
# The split into layers comes from layers/*.files (path lists), and the
# generated .patch files are written to the repository root.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: $0 <godot-source-dir>}
DIR="$ROOT"
PINNED=$(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' "$ROOT/upstream.lock")
# Accept linked worktrees too (their .git is a file, not a directory).
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 || { echo "$SRC is not a git checkout" >&2; exit 2; }

for list in "$DIR"/layers/*.files; do
	name=$(basename "$list" .files)
	paths=()
	while IFS= read -r p; do
		if [ -n "$p" ]; then paths+=("$p"); fi
	done < "$list"
	git -C "$SRC" diff --binary "$PINNED" -- "${paths[@]}" > "$DIR/$name.patch" 2>/dev/null || \
		git -C "$SRC" diff --binary HEAD -- "${paths[@]}" > "$DIR/$name.patch"
	echo "$name.patch $(wc -c < "$DIR/$name.patch") bytes"
done
