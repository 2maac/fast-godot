# Fast_Godot

Fast_Godot is a custom build of Godot **4.7.2** with two additions:

- **Grit** — compiles a project's GDScript into C++ and links it into a
  per-project export template, so game logic runs as native code instead of
  bytecode.
- **CFFI** — lets GDScript call into C libraries directly.

The CFFI reference is at [2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/)
and in [`docs/`](docs/). Editors and toolchains are **built locally**: this fork
has no CI and publishes no releases.

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

## Building the editor

`tools/build-editor.sh` (Linux, macOS) and `build-editor.cmd` in the Windows
toolchain folder are the portable entry points: they find the source tree and the
toolchain, check/install what the build needs, and run SCons with
`production=yes`. The same script is what the editor calls to build an export
template.

```
tools/grit_build.py editor --toolchain <toolchain-folder> [--engine <checkout>]
```

macOS still uses the Xcode Command Line Tools as its compiler; the toolchain
supplies SCons, Python and MoltenVK.

## Toolchains

A toolchain is a directory built with the scripts in `tools/toolchain/` and
chosen in Editor Settings (`grit/settings/toolchain_root_folder_path`), together
with the engine source tree (`source_code_root_folder_path`). Nothing is
downloaded during an export.

| host | the toolchain carries |
|---|---|
| Linux | Python, SCons, focal GCC 9 + glibc 2.31 sysroot, libffi, wayland-scanner, Emscripten |
| Windows | Python, SCons, MinGW-w64, libffi, ANGLE, Mesa NIR + Agility SDK, WinRT headers, Emscripten |
| macOS | Python, SCons, MoltenVK, Emscripten |

## Engine fixes

The fork carries a fix for a null-token crash in
`ResourceLoader::_run_load_task()` that kills a worker thread when a scene loads
with `load_threaded_request(..., use_sub_threads = true)`.

## Status

- macOS, Linux and Windows editors build locally.
- Linux, Windows, macOS and HTML5 Fast templates build; the Linux and HTML5
  exports have been run and benchmarked, the Windows export has not been run on
  Windows yet.
