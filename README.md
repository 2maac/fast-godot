# Fast_Godot

Fast_Godot is a custom build of Godot **4.7.2** with two additions:

- **Grit** — compiles a project's GDScript into C++ and links it into a
  per-project export template, so game logic runs as native code instead of
  bytecode.
- **CFFI** — lets GDScript call into C libraries directly.

The CFFI reference is at [2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/)
and in [`docs/`](docs/). This fork has no CI and publishes no releases; editors and
build toolchains are produced locally.

## How fast is it?

Grit compiles at function granularity, so the gain depends on what the code does.
Measured on this fork (macOS arm64, GDScript VM versus compiled):

| workload | VM | native | speedup |
|---|---|---|---|
| 3M iterations of integer arithmetic | 39.9 ms | 0.5 ms | ~80x |
| 1M calls to a small function | 64.0 ms | 5.8 ms | ~11x |

Straight-line code gains most; call-heavy code is bounded by the call boundary.
GPU-bound 3D scenes will not show a difference — there the renderer, not script,
sets the frame time.

Functions Grit cannot compile stay on the GDScript VM, and the export log names
each one with the reason.

## Export model: only Fast presets

There are no VM/stock exports. Every project builds its own template:

| platform | preset | template |
|---|---|---|
| macOS arm64 | `macOS Fast` | `<project>/exports/macos_fast.zip` |
| Linux x86_64 | `Linux Fast` | `<project>/exports/linux_fast_<target>.<arch>` |
| Windows x86_64 | `Windows Fast` | `<project>/exports/windows_fast_<target>_<arch>.exe` |
| Web | `HTML5 Fast` | `<project>/exports/web_fast.zip` |

A platform is only registered on its own host, so a macOS editor offers
`macOS Fast` and ignores presets for other platforms. Nothing is cross-compiled;
the HTML5 template is built by the toolchain's Emscripten.

The first export of a project compiles the engine for that project's template
(10–20 minutes on a warm machine); later exports only rebuild what changed.

## First run: Download Build Tools

A fresh editor has no build tools, so the export window shows no presets — only a
centered **Download Build Tools** button. Pressing it:

1. fetches the engine source for this editor's commit once (it is cached and
   reused later),
2. downloads every toolchain component from its original source into
   `~/fast_godot_toolchain` (Windows: `%USERPROFILE%\fast_godot_toolchain`), with
   progress and a SHA-256 check for each file,
3. validates the folder and points `grit/settings/toolchain_root_folder_path` at
   it.

After that the presets appear and the button never comes back; the export reuses
the toolchain and source.

The downloader scripts live in the engine source and use only the tools that ship
with the OS:

- `tools/toolchain/download_toolchain.sh` (Linux, macOS): `curl`, `tar`, `ar` /
  `dpkg-deb`. Linux gets focal GCC 9 + a glibc 2.31 sysroot + `wayland-scanner`
  (from pinned Ubuntu packages) and libffi; macOS gets MoltenVK. Emscripten is
  installed by emsdk, which is itself a Python program and uses the Python that
  was just downloaded.
- `tools/toolchain/download_toolchain.cmd` (Windows): `curl.exe`, `tar.exe`,
  `certutil.exe` (PowerShell only for the libffi `dl*` symbol rename), plus emsdk.

What the toolchain carries:

| host | components |
|---|---|
| Linux | Python, SCons, focal GCC 9 (glibc 2.31 sysroot), binutils, libffi, wayland-scanner, Emscripten |
| Windows | Python, SCons, MinGW-w64, libffi, static ANGLE, Mesa NIR + Agility SDK, WinRT headers, Emscripten |
| macOS | Python, SCons, MoltenVK, Emscripten |

Nothing else needs to be installed for an export: no system C++ compiler, X11,
ALSA, Wayland or libffi development packages. macOS still uses the Xcode Command
Line Tools for its compiler.

## Building the editor itself

`tools/build-editor.sh` (Linux, macOS) and `build-editor.cmd` in the Windows
toolchain folder find the source tree and the toolchain and run SCons with
`production=yes`:

```
tools/grit_build.py editor --toolchain <toolchain-folder> [--engine <checkout>]
```

On macOS the build needs MoltenVK; the downloaded toolchain provides it under
`moltenvk/`, or pass `vulkan_sdk_path=<toolchain>/moltenvk`.

## Toolchains by hand

Every component can also be assembled with the scripts in `tools/toolchain/`
(`fetch_linux_toolchain.sh`, `make_linux_gcc.sh` with its pinned
`linux_gcc_packages.txt`, `make_bundle_zip.py`), and any folder can be selected
with the *ToolChain Root Folder Path* editor setting. A valid folder contains
`toolchain.json` and the components above.

## Engine fixes

The fork carries a fix for a null-token crash in
`ResourceLoader::_run_load_task()` that kills a worker thread when a scene loads
with `load_threaded_request(..., use_sub_threads = true)`.

## Status

- macOS, Linux and Windows editors build locally.
- Linux, Windows, macOS and HTML5 Fast templates build; the Linux and HTML5
  exports have been run and benchmarked, the Windows export has not been run on
  Windows yet.
