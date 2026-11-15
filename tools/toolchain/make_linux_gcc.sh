#!/usr/bin/env bash
# Bundles a real GCC for Linux: Ubuntu 20.04 (focal) GCC 9 with its glibc 2.31
# sysroot, binutils with LTO plugin support (gcc-ar/gcc-ranlib), and
# wayland-scanner.
#
# Why focal: the docs recommend building distributable Linux binaries on an old
# distribution such as Ubuntu 20.04, and GCC 9 is the oldest compiler Godot 4.7
# supports. GCC (unlike the Zig wrapper it replaces) can do LTO, which is what
# production=yes wants.
#
# The packages are pinned by URL and SHA-256 in linux_gcc_packages.txt. Godot
# vendors the X11/Wayland/ALSA headers and dlopens those libraries at run time
# (use_sowrap), so no -dev packages are needed beyond wayland-scanner, which is a
# build-time tool.
#
# usage: make_linux_gcc.sh <bundle dir>
set -euo pipefail

B="${1:?usage: make_linux_gcc.sh <bundle dir>}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
list="$here/linux_gcc_packages.txt"

mkdir -p "$B/gcc/root" "$B/gcc/bin" "$B/gcc/tmp"
cd "$B/gcc/tmp"
while read -r url sha _name; do
    case "$url" in ""|\#*) continue ;; esac
    file=$(basename "$url")
    if [ ! -f "$file" ]; then
        curl -sSfL --retry 3 -o "$file" "$url"
    fi
    echo "$sha  $file" | sha256sum -c - >/dev/null
    dpkg-deb -x "$file" "$B/gcc/root"
done < "$list"
rm -rf "$B/gcc/tmp"

# dpkg-deb preserves absolute symlinks (libdl.so -> /lib/x86_64-linux-gnu/...).
# The linker follows those on the *host* filesystem even with --sysroot, which
# pulled the host's glibc into the link; make every absolute symlink relative so it
# resolves inside the sysroot.
find "$B/gcc/root" -type l | while IFS= read -r link; do
    target=$(readlink "$link")
    case "$target" in
        /*)
            resolved="$B/gcc/root$target"
            if [ -e "$resolved" ]; then
                rel=$(realpath --relative-to="$(dirname "$link")" "$resolved")
                ln -sfn "$rel" "$link"
            fi
            ;;
    esac
done

# The archives use the usrmerge layout only partially; the compiler's default
# search paths expect /lib and /lib64 to exist next to /usr.
cd "$B/gcc/root"
ln -sfn usr/lib lib
ln -sfn usr/lib64 lib64
ln -sfn usr/bin bin
ln -sfn usr/sbin sbin
cd "$B"

write_wrapper() {
    target="$1"
    name="$2"
    shift 2
    cat > "$B/gcc/bin/$name" <<WRAP
#!/bin/sh
here=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
root=\$here/../root
export LD_LIBRARY_PATH="\$root/usr/lib/x86_64-linux-gnu:\$root/usr/lib"
exec "\$root/usr/bin/$target" --sysroot="\$root" -B"\$root/usr/lib/gcc/x86_64-linux-gnu/9/" -B"\$root/usr/bin/" "\$@"
WRAP
    chmod +x "$B/gcc/bin/$name"
}

write_wrapper gcc-9 gcc
write_wrapper g++-9 g++

cat > "$B/gcc/bin/gcc-ar" <<WRAP
#!/bin/sh
here=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
root=\$here/../root
export LD_LIBRARY_PATH="\$root/usr/lib/x86_64-linux-gnu:\$root/usr/lib"
exec "\$root/usr/bin/gcc-ar-9" "\$@"
WRAP
cat > "$B/gcc/bin/gcc-ranlib" <<WRAP
#!/bin/sh
here=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
root=\$here/../root
export LD_LIBRARY_PATH="\$root/usr/lib/x86_64-linux-gnu:\$root/usr/lib"
exec "\$root/usr/bin/gcc-ranlib-9" "\$@"
WRAP
chmod +x "$B/gcc/bin/gcc-ar" "$B/gcc/bin/gcc-ranlib"

cat > "$B/gcc/bin/wayland-scanner" <<WRAP
#!/bin/sh
here=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
exec "\$here/../root/usr/bin/wayland-scanner" "\$@"
WRAP
chmod +x "$B/gcc/bin/wayland-scanner"

"$B/gcc/bin/gcc" --version | head -n 1
"$B/gcc/bin/g++" -print-file-name=libstdc++.a
"$B/gcc/bin/wayland-scanner" -v
