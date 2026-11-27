#!/usr/bin/env bash
# Regenerate the patch series from a working tree that already contains our
# changes on top of the pinned Godot commit.
#
#   tools/make-patches.sh <godot-source-dir>
#
# The split into layers comes from patches/layers/*.files (path lists), and the
# generated .patch files are written to ./patches/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: $0 <godot-source-dir>}
PATCH_DIR="$ROOT/patches"
LAYERS_DIR="$PATCH_DIR/layers"
PINNED=$(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' "$ROOT/patches/upstream.lock")
mkdir -p "$PATCH_DIR"
# Accept linked worktrees too (their .git is a file, not a directory).
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 || { echo "$SRC is not a git checkout" >&2; exit 2; }

for list in "$LAYERS_DIR"/*.files; do
	name=$(basename "$list" .files)
	paths=()
	while IFS= read -r p; do
		if [ -n "$p" ]; then paths+=("$p"); fi
	done < "$list"
	# New (untracked) files would be silently skipped by `git diff <commit>`, so
	# mark them intent-to-add first: that makes the diff carry their full
	# contents as additions. `-f` also covers a new file whose name matches a
	# .gitignore rule (`*.gen.*` used to swallow generated headers). The
	# intent-to-add entries live only in the source tree's index and do not
	# modify the working tree.
	while IFS= read -r newfile; do
		if [ -n "$newfile" ]; then
			git -C "$SRC" add -N -f -- "$newfile" || {
				echo "failed to mark new file as intent-to-add: $newfile" >&2
				exit 3
			}
		fi
	done < <(git -C "$SRC" ls-files --others -- "${paths[@]}" 2>/dev/null || true)
	git -C "$SRC" diff --binary "$PINNED" -- "${paths[@]}" > "$PATCH_DIR/$name.patch" 2>/dev/null || \
		git -C "$SRC" diff --binary HEAD -- "${paths[@]}" > "$PATCH_DIR/$name.patch"
	echo "$name.patch $(wc -c < "$PATCH_DIR/$name.patch") bytes"
done
