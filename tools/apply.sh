#!/usr/bin/env bash
# Apply the Fast Godot patch stack to a Godot source checkout.
#
#   tools/apply.sh <godot-source-dir>
#
# The ordered .patch files live in ./patches/ (single-stack repo). The source dir
# must be a git checkout at (or near) the commit pinned in upstream.lock; patches
# are applied with `git apply --3way` so a slightly newer base can still resolve.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: $0 <godot-source-dir>}
# Accept linked worktrees too (their .git is a file, not a directory).
git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1 || { echo "$SRC is not a git checkout" >&2; exit 2; }

PINNED=$(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' "$ROOT/upstream.lock")
HEAD=$(git -C "$SRC" rev-parse HEAD)
if [ -n "$PINNED" ] && [ "$PINNED" != "$HEAD" ]; then
	echo "note: base is $HEAD, pinned is $PINNED (continuing with 3-way apply)"
fi

for p in "$ROOT"/patches/*.patch; do
	[ -e "$p" ] || continue
	printf '  %-40s' "$(basename "$p")"
	if git -C "$SRC" apply --3way --whitespace=nowarn "$p"; then
		echo ok
	else
		echo FAILED
		exit 1
	fi
done
echo "patch stack applied to $SRC"
