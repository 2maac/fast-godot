#!/usr/bin/env bash
# Apply the Fast Godot patch stack to a Godot source checkout.
#
#   tools/apply.sh <godot-source-dir> [stack]
#
# stack defaults to 4.8-beta5. The source dir must be a git checkout at (or near)
# the commit pinned in upstream.lock; patches are applied with `git apply
# --3way` so a slightly newer base can still resolve.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: $0 <godot-source-dir> [stack]}
STACK=${2:-4.8-beta5}
DIR="$ROOT/patches/$STACK"
[ -d "$DIR" ] || { echo "no such stack: $STACK" >&2; exit 2; }
[ -d "$SRC/.git" ] || { echo "$SRC is not a git checkout" >&2; exit 2; }

PINNED=$(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' "$ROOT/upstream.lock")
HEAD=$(git -C "$SRC" rev-parse HEAD)
if [ -n "$PINNED" ] && [ "$PINNED" != "$HEAD" ]; then
	echo "note: base is $HEAD, pinned for $STACK is $PINNED (continuing with 3-way apply)"
fi

for p in "$DIR"/*.patch; do
	[ -e "$p" ] || continue
	printf '  %-40s' "$(basename "$p")"
	if git -C "$SRC" apply --3way --whitespace=nowarn "$p"; then
		echo ok
	else
		echo FAILED
		exit 1
	fi
done
echo "patch stack $STACK applied to $SRC"
