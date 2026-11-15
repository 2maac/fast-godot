#!/usr/bin/env bash
# Bundles the pieces a Fast Linux template needs beyond Python and SCons: a C++
# compiler and libffi. Godot vendors the X11/Wayland/ALSA headers and dlopens those
# libraries at run time (use_sowrap defaults to yes), and the CFFI module needs
# libffi headers plus a static archive, which Linux distros only ship in
# libffi-dev. With this script a template builds on a machine with no -dev
# packages at all.
#
# Zig is used because it is one self-contained directory: it brings its own
# libc++ (linked statically) and targets glibc 2.17, so the produced template does
# not get pinned to this build machine's glibc.
#
# usage: make_linux_compiler.sh <bundle dir>
set -euo pipefail

B="${1:?usage: make_linux_compiler.sh <bundle dir>}"
ZIG_VERSION="${ZIG_VERSION:-0.17.1}"
ZIG_SHA256="${ZIG_SHA256:-3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a}"
ZIG_TARGET="${ZIG_TARGET:-x86_64-linux-gnu.2.28}"

mkdir -p "$B/zig" "$B/compiler"
archive="$(mktemp)"
curl -sSfL "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" -o "$archive"
echo "${ZIG_SHA256}  ${archive}" | sha256sum -c -
tar -xJf "$archive" -C "$B/zig" --strip-components=1
rm -f "$archive"

cat >"$B/compiler/clang" <<'WRAP'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ZIG_GLOBAL_CACHE_DIR:=$here/../zig-cache}"
export ZIG_GLOBAL_CACHE_DIR
exec "$here/../zig/zig" cc -target __TARGET__ "$@"
WRAP
cat >"$B/compiler/clang++" <<'WRAP'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ZIG_GLOBAL_CACHE_DIR:=$here/../zig-cache}"
export ZIG_GLOBAL_CACHE_DIR
exec "$here/../zig/zig" c++ -target __TARGET__ "$@"
WRAP
cat >"$B/compiler/ar" <<'WRAP'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/../zig/zig" ar "$@"
WRAP
cat >"$B/compiler/ranlib" <<'WRAP'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/../zig/zig" ranlib "$@"
WRAP
cat >"$B/compiler/pkg-config" <<'STUB'
#!/bin/sh
# Godot's configure step probes pkg-config unconditionally. With use_sowrap there
# is nothing to look up (the libraries are dlopened), so --version is enough.
if [ "${1:-}" = "--version" ]; then
	echo "0.29.2"
	exit 0
fi
exit 1
STUB
sed -i "s/__TARGET__/${ZIG_TARGET}/" "$B/compiler/clang" "$B/compiler/clang++"
chmod +x "$B/compiler/"*

# Warm the libc++ cache during assembly: the first real build should not pay for
# it. The probe also proves the compiler runs and produces a Linux binary here.
probe="$(mktemp --suffix=.cpp)"
cat >"$probe" <<'CPP'
#include <cstdio>
#include <string>
#include <vector>

int main() {
	std::vector<int> values{1, 2, 3};
	std::string text = "zig";
	std::printf("%s %d\n", text.c_str(), values[1]);
	return 0;
}
CPP
"$B/compiler/clang++" -O2 -std=c++20 -pthread "$probe" -o "$probe.bin"
"$probe.bin"
rm -f "$probe" "$probe.bin"

echo "linux compiler: zig ${ZIG_VERSION} -> ${ZIG_TARGET}, $(du -sh "$B/zig" | cut -f1)"

# libffi, for the CFFI module. The package only ships the headers and the static
# archive in libffi-dev; extract them so a template build does not need it installed.
apt-get update -qq
ffi_tmp="$(mktemp -d)"
(
	cd "$ffi_tmp"
	apt-get download libffi-dev >/dev/null
	dpkg-deb -x libffi-dev_*.deb root
)
ffi_h="$(find "$ffi_tmp/root/usr/include" -name ffi.h | head -1)"
ffi_lib="$(find "$ffi_tmp/root/usr/lib" -name libffi.a | head -1)"
if [ -z "$ffi_h" ] || [ -z "$ffi_lib" ]; then
	echo "libffi-dev layout not recognised" >&2
	exit 1
fi
mkdir -p "$B/libffi/linux-x86_64/include" "$B/libffi/linux-x86_64/lib"
cp "$ffi_h" "$(dirname "$ffi_h")/ffitarget.h" "$B/libffi/linux-x86_64/include/"
cp "$ffi_lib" "$B/libffi/linux-x86_64/lib/"
rm -rf "$ffi_tmp"
echo "linux libffi: $(du -sh "$B/libffi/linux-x86_64" | cut -f1)"
