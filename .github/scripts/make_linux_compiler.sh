#!/usr/bin/env bash
# Bundles a C++ compiler into a Linux toolchain so that a Fast Linux template can
# be built on a machine with no distribution -dev packages at all. Godot vendors
# the X11/Wayland/ALSA headers and dlopens those libraries at run time (use_sowrap
# defaults to yes), so a compiler is the only thing a template build needs.
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
ZIG_TARGET="${ZIG_TARGET:-x86_64-linux-gnu.2.17}"

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
