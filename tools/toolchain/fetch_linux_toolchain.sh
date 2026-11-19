#!/usr/bin/env bash
# Adds the portable Python, SCons and toolchain.json to a Linux bundle directory.
#   fetch_linux_toolchain.sh <bundle_dir>
set -euo pipefail
B="${1:?usage: fetch_linux_toolchain.sh <bundle_dir>}"

PY_NAME="cpython-3.12.16+20261110-x86_64-unknown-linux-gnu-install_only.tar.gz"
PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-unknown-linux-gnu-install_only.tar.gz"
PY_SHA="c2f87d4b4e8db3abddb0f116c6ccfc503d0cbdbb9ee8faacddad1b679ec12e99"

SCONS_URL="https://files.pythonhosted.org/packages/42/b9/b7a5c88f348a0c34594d88100872c55fa1cae863ccb222c1c438341b5503/scons-4.11.1.tar.gz"
SCONS_SHA="4210d1a80a62e986029208117991b6347ccaaaab37b67463a3ff31ee065dc487"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -sSL --fail -o "$TMP/python.tar.gz" "$PY_URL"
echo "$PY_SHA  $TMP/python.tar.gz" | sha256sum -c -
tar -xzf "$TMP/python.tar.gz" -C "$B"

mkdir -p "$B/scons/4.11.1"
curl -sSL --fail -o "$TMP/scons.tar.gz" "$SCONS_URL"
echo "$SCONS_SHA  $TMP/scons.tar.gz" | sha256sum -c -
tar -xzf "$TMP/scons.tar.gz" -C "$B/scons/4.11.1"

# The bundle carries the engine source tree it was built from, like the
# downloaders do, so editors and templates can be built from it directly. The
# folder has no revision in its name: a toolchain works with any 4.7.x editor.
ENGINE_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
ENGINE_DEST="$B/engine/source"
if [ -f "$ENGINE_ROOT/SConstruct" ]; then
	rm -rf "$ENGINE_DEST"
	mkdir -p "$ENGINE_DEST"
	(
		cd "$ENGINE_ROOT" &&
		tar -cf - \
			--exclude=.git --exclude=./bin --exclude=./out --exclude=./.godot \
			--exclude=./compile_commands.json --exclude=./.sconsign.dblite \
			--exclude=./.sconsign5.dblite --exclude=./.scons_env.json \
			--exclude=./.scons_node_count --exclude=__pycache__ --exclude=.DS_Store \
			. 2>/dev/null
	) | tar -xf - -C "$ENGINE_DEST"
	[ -f "$ENGINE_DEST/SConstruct" ] || { echo "engine source copy is incomplete" >&2; exit 1; }
else
	echo "warning: $ENGINE_ROOT is not a checkout; the bundle will have no engine source" >&2
fi

cat > "$B/toolchain.json" <<JSON
{
  "name": "Fast_Godot_Toolchain_linux_x86_64",
  "format": 1,
  "engineVersion": "4.7.2-stable",
  "platform": "linux",
  "arch": "x86_64",
  "engine": "engine/source",
  "python": "python",
  "scons": "scons/4.11.1/scons-4.11.1",
  "gcc": "gcc"
}
JSON
