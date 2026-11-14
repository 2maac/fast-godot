#!/usr/bin/env bash
#
# Installs the development packages a Linux Fast template build needs.
#
# Only the C++ toolchain is missing on a typical machine: Godot vendors the
# X11/Wayland/ALSA headers and loads those libraries at run time (use_sowrap
# defaults to yes), so no -dev packages for them are required. The static
# libstdc++ is needed because Godot links it statically (use_static_cpp=yes).
#
#   tools/install_linux_deps.sh            # report what is missing, do not install
#   tools/install_linux_deps.sh --yes      # install it (asks for your password)
#
set -euo pipefail

say() {
	printf '%s\n' "$*"
}

assume_yes=0
for arg in "$@"; do
	case "$arg" in
	-y | --yes)
		assume_yes=1
		;;
	-h | --help)
		sed -n '2,13p' "$0"
		exit 0
		;;
	*)
		say "unknown option: $arg" >&2
		exit 2
		;;
	esac
done

if [ "$(uname -s)" != "Linux" ]; then
	say "This script is for Linux; nothing to do on $(uname -s)."
	exit 0
fi

manager=""
if command -v apt-get >/dev/null 2>&1; then
	manager="apt"
elif command -v dnf >/dev/null 2>&1; then
	manager="dnf"
elif command -v pacman >/dev/null 2>&1; then
	manager="pacman"
elif command -v zypper >/dev/null 2>&1; then
	manager="zypper"
fi

case "$manager" in
apt)
	command_line="sudo apt-get install -y build-essential pkg-config"
	;;
dnf)
	command_line="sudo dnf install -y gcc-c++ libstdc++-static pkgconf"
	;;
pacman)
	command_line="sudo pacman -S --needed base-devel pkgconf"
	;;
zypper)
	command_line="sudo zypper install -y gcc-c++ libstdc++-devel pkg-config"
	;;
*)
	say "Could not find a supported package manager (apt-get, dnf, pacman, zypper)."
	say "Install a C++ toolchain with the static libstdc++ (g++ / build-essential)"
	say "and pkg-config by hand."
	exit 1
	;;
esac

missing=""
if ! command -v g++ >/dev/null 2>&1; then
	missing="$missing g++"
fi
if ! command -v pkg-config >/dev/null 2>&1; then
	missing="$missing pkg-config"
fi
if command -v g++ >/dev/null 2>&1; then
	static_libstdcpp="$(g++ -print-file-name=libstdc++.a 2>/dev/null || true)"
	if [ -z "$static_libstdcpp" ] || [ ! -f "$static_libstdcpp" ]; then
		missing="$missing static-libstdc++"
	fi
fi

if [ -z "$missing" ]; then
	say "Everything a Linux template build needs is already installed."
	exit 0
fi

say "Missing:$missing"
say "Install with:"
say "  $command_line"

if [ "$assume_yes" -ne 1 ]; then
	say ""
	say "Re-run with --yes to install (it will ask for your password)."
	exit 0
fi

if [ "$(id -u)" -eq 0 ]; then
	# Drop the sudo prefix when already root.
	command_line="${command_line#sudo }"
fi
say ""
say "Running: $command_line"
exec $command_line
