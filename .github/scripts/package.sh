#!/usr/bin/env bash
# Package one build into the layout Godot's exporter and users expect.
#   package.sh <platform> <target> <arch> <version> <outdir>
set -euo pipefail
PLATFORM="$1"; TARGET="$2"; ARCH="$3"; VERSION="$4"; OUT="${5:-packages}"
mkdir -p "$OUT"

# use_llvm=yes adds a ".llvm" infix to the binary name (env.extra_suffix).
case "$PLATFORM" in
  windows) EXT=".exe" ;;
  *) EXT="" ;;
esac
BASE="bin/godot.${PLATFORM}.${TARGET}.${ARCH}"
BIN=""
for cand in "${BASE}.llvm${EXT}" "${BASE}${EXT}"; do
  if [ -f "$cand" ]; then BIN="$cand"; break; fi
done
[ -n "$BIN" ] || { echo "missing ${BASE}${EXT}"; ls -l bin | head -20; exit 1; }

bundle() { # $1 = skeleton, $2 = binary, $3 = output zip
  rm -rf Fast_Godot.app && cp -R "$1" Fast_Godot.app
  mkdir -p Fast_Godot.app/Contents/MacOS
  cp "$2" Fast_Godot.app/Contents/MacOS/Fast_Godot
  chmod +x Fast_Godot.app/Contents/MacOS/Fast_Godot
  sed -i '' -e 's/<string>\$binary<\/string>/<string>Fast_Godot<\/string>/' -e 's/<string>Godot<\/string>/<string>Fast_Godot<\/string>/' Fast_Godot.app/Contents/Info.plist
  # The binary carries a linker-signed ad-hoc signature, but a bundle needs its
  # own _CodeSignature/CodeResources or macOS reports the app as damaged.
  # AppleDouble/xattrs at the bundle root also break signing, so clear them.
  find Fast_Godot.app -name '._*' -delete
  xattr -cr Fast_Godot.app
  codesign --force --deep --sign - Fast_Godot.app
  codesign --verify --deep --strict Fast_Godot.app
  ditto -c -k --norsrc --noextattr --keepParent Fast_Godot.app "$3"
}

case "$PLATFORM-$TARGET" in
  macos-editor)
    bundle misc/dist/macos_tools.app "$BIN" "$OUT/Fast_Godot_v${VERSION}_macos.arm64.zip"
    ;;
  macos-template_release)
    # Godot looks up a single "macos.zip" for both debug and release exports.
    mkdir -p "$OUT/templates"
    bundle misc/dist/macos_template.app "$BIN" "$OUT/templates/macos.zip"
    ;;
  macos-template_debug)
    # Ships standalone: usable as an export preset's custom debug template.
    bundle misc/dist/macos_template.app "$BIN" "$OUT/macos_debug.zip"
    ;;
  linuxbsd-editor)
    cp "$BIN" "Fast_Godot_v${VERSION}_linux.x86_64"
    chmod +x "Fast_Godot_v${VERSION}_linux.x86_64"
    python -m zipfile -c "$OUT/Fast_Godot_v${VERSION}_linux.x86_64.zip" "Fast_Godot_v${VERSION}_linux.x86_64"
    ;;
  windows-editor)
    cp "$BIN" "Fast_Godot_v${VERSION}_win64.exe"
    python -m zipfile -c "$OUT/Fast_Godot_v${VERSION}_win64.zip" "Fast_Godot_v${VERSION}_win64.exe"
    ;;
  linuxbsd-template_release)
    mkdir -p "$OUT/templates" && cp "$BIN" "$OUT/templates/linux_release.$ARCH" ;;
  linuxbsd-template_debug)
    mkdir -p "$OUT/templates" && cp "$BIN" "$OUT/templates/linux_debug.$ARCH" ;;
  windows-template_release)
    mkdir -p "$OUT/templates" && cp "$BIN" "$OUT/templates/windows_release_$ARCH.exe" ;;
  windows-template_debug)
    mkdir -p "$OUT/templates" && cp "$BIN" "$OUT/templates/windows_debug_$ARCH.exe" ;;
esac
find "$OUT" -type f | sort
