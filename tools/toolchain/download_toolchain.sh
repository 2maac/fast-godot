#!/bin/sh
# Downloads a complete Fast_Godot toolchain from the original sources into
# <dest> (default: ~/fast_godot_toolchain). No Python is used to download or unpack
# anything: curl, tar and (on Linux) ar/dpkg-deb do the work. Emscripten is the one
# exception: emsdk is a Python program and installs Emscripten itself, using the
# toolchain interpreter that was just downloaded.
#
# Progress is reported on stdout as "GRIT_PROGRESS <percent> <label>", errors as
# "GRIT_ERROR <text>", and success as "GRIT_DONE <path>"; the editor reads those.
#
# usage: download_toolchain.sh [dest]
set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dest=${1:-${HOME:-/tmp}/fast_godot_toolchain}
prog=${2:-}
os=$(uname -s)
arch=$(uname -m)

progress() {
	printf 'GRIT_PROGRESS %s %s\n' "$1" "$2"
	[ -n "$prog" ] && printf 'GRIT_PROGRESS %s %s\n' "$1" "$2" >> "$prog"
}
fail() {
	printf 'GRIT_ERROR %s\n' "$1"
	[ -n "$prog" ] && printf 'GRIT_ERROR %s\n' "$1" >> "$prog"
	exit 1
}
sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		return 1
	fi
}
fetch() {
	url=$1
	sum=$2
	out=$3
	curl -sSfL --retry 3 -o "$out" "$url" || fail "could not download $url"
	got=$(sha256 "$out") || fail "no sha256 tool"
	[ "$got" = "$sum" ] || fail "checksum mismatch for $url"
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$dest"
cd "$dest"

progress 5 "Python and SCons"
if [ "$os" = "Darwin" ]; then
	if [ "$arch" = "arm64" ]; then
		PY_SHA="e956e60bc9465d577ef8062059aa05cd3a2665b4b8678a2ed24e432368330b8f"
		PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-aarch64-apple-darwin-install_only.tar.gz"
	else
		PY_SHA="83f2922437e6d546d0fcdb8c11f150a8e52787e8d9ea8651b1974e20908eb1bc"
		PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-apple-darwin-install_only.tar.gz"
	fi
else
	PY_SHA="c2f87d4b4e8db3abddb0f116c6ccfc503d0cbdbb9ee8faacddad1b679ec12e99"
	PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16%2B20261110-x86_64-unknown-linux-gnu-install_only.tar.gz"
fi
fetch "$PY_URL" "$PY_SHA" "$tmp/python.tar.gz"
tar -xzf "$tmp/python.tar.gz" -C "$dest"

SCONS_URL="https://files.pythonhosted.org/packages/42/b9/b7a5c88f348a0c34594d88100872c55fa1cae863ccb222c1c438341b5503/scons-4.11.1.tar.gz"
SCONS_SHA="4210d1a80a62e986029208117991b6347ccaaaab37b67463a3ff31ee065dc487"
mkdir -p "$dest/scons/4.11.1"
fetch "$SCONS_URL" "$SCONS_SHA" "$tmp/scons.tar.gz"
tar -xzf "$tmp/scons.tar.gz" -C "$dest/scons/4.11.1"

if [ "$os" = "Linux" ]; then
	progress 20 "libffi"
	LIBFFI_URL="http://archive.ubuntu.com/ubuntu/pool/main/libf/libffi/libffi-dev_3.3-4_amd64.deb"
	LIBFFI_SHA="8c83e874ec75c223774eb0aa083022496a8199f88471f1cb46f5de473d748f71"
	fetch "$LIBFFI_URL" "$LIBFFI_SHA" "$tmp/libffi.deb"
	mkdir -p "$tmp/ffi"
	if command -v dpkg-deb >/dev/null 2>&1; then
		dpkg-deb -x "$tmp/libffi.deb" "$tmp/ffi"
	else
		(cd "$tmp/ffi" && ar p "$tmp/libffi.deb" data.tar.* | tar -x)
	fi
	mkdir -p "$dest/libffi/linux-x86_64/include" "$dest/libffi/linux-x86_64/lib"
	for inc in "$tmp/ffi/usr/include" "$tmp/ffi/usr/include/x86_64-linux-gnu"; do
		[ -d "$inc" ] || continue
		cp "$inc"/*ffi*.h "$dest/libffi/linux-x86_64/include/" 2>/dev/null || true
	done
	[ -f "$dest/libffi/linux-x86_64/include/ffi.h" ] || fail "libffi headers were not found in the package"
	cp "$tmp/ffi/usr/lib/x86_64-linux-gnu/libffi.a" "$dest/libffi/linux-x86_64/lib/"

	progress 35 "GCC 9, glibc 2.31 sysroot and wayland-scanner"
	sh "$here/make_linux_gcc.sh" "$dest"
else
	progress 35 "MoltenVK"
	MOLTENVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar"
	MOLTENVK_SHA="f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"
	fetch "$MOLTENVK_URL" "$MOLTENVK_SHA" "$tmp/moltenvk.tar"
	mkdir -p "$tmp/mv"
	tar -xf "$tmp/moltenvk.tar" -C "$tmp/mv" MoltenVK/MoltenVK/static/MoltenVK.xcframework
	rm -rf "$dest/moltenvk"
	mkdir -p "$dest/moltenvk/macOS/lib"
	mv "$tmp/mv/MoltenVK/MoltenVK/static/MoltenVK.xcframework" "$dest/moltenvk/macOS/lib/"
fi

progress 60 "Emscripten 4.0.11 (emsdk)"
EMSDK_URL="https://github.com/emscripten-core/emsdk/archive/refs/tags/4.0.11.tar.gz"
EMSDK_SHA="058f97f3d408e6438c0128de63fc83b085a1a1c8f8e2ef992edee01281af9546"
fetch "$EMSDK_URL" "$EMSDK_SHA" "$tmp/emsdk.tar.gz"
mkdir -p "$tmp/emsdk"
tar -xzf "$tmp/emsdk.tar.gz" -C "$tmp/emsdk"
rm -rf "$dest/emsdk"
mv "$tmp/emsdk"/emsdk-4.0.11 "$dest/emsdk"
python="$dest/python/bin/python3"
[ -x "$python" ] || fail "the downloaded Python is missing"
"$python" "$dest/emsdk/emsdk.py" install 4.0.11 >/dev/null
"$python" "$dest/emsdk/emsdk.py" activate 4.0.11 >/dev/null

progress 90 "verifying"
for f in "$dest/python/bin/python3" "$dest/scons/4.11.1/scons-4.11.1/SCons/__init__.py" "$dest/emsdk/upstream/emscripten/emcc"; do
	[ -e "$f" ] || fail "missing $f"
done
if [ "$os" = "Linux" ]; then
	[ -x "$dest/gcc/bin/gcc" ] || fail "missing the GCC wrappers"
	[ -x "$dest/gcc/bin/wayland-scanner" ] || fail "missing wayland-scanner"
else
	[ -d "$dest/moltenvk/macOS/lib/MoltenVK.xcframework" ] || fail "missing MoltenVK"
fi

if [ "$os" = "Linux" ]; then
	cat > "$dest/toolchain.json" <<JSON
{
  "name": "Fast_Godot_Toolchain_linux_x86_64",
  "format": 1,
  "engineVersion": "4.7.2-stable",
  "platform": "linux",
  "arch": "x86_64",
  "python": "python",
  "scons": "scons/4.11.1/scons-4.11.1",
  "gcc": "gcc",
  "libffi": "libffi/linux-x86_64",
  "emsdk": "emsdk"
}
JSON
else
	cat > "$dest/toolchain.json" <<JSON
{
  "name": "Fast_Godot_Toolchain_macos_$arch",
  "format": 1,
  "engineVersion": "4.7.2-stable",
  "platform": "macos",
  "arch": "$arch",
  "python": "python",
  "scons": "scons/4.11.1/scons-4.11.1",
  "moltenvk": "moltenvk",
  "emsdk": "emsdk"
}
JSON
fi

progress 100 "toolchain ready"
printf 'GRIT_DONE %s\n' "$dest"
[ -n "$prog" ] && printf 'GRIT_DONE %s\n' "$dest" >> "$prog"
