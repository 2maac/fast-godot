<p align="center">
  <img src="docs/assets/fast-godot-logo.png" alt="Fast Godot" width="420">
</p>

Fast Godot is a custom build of Godot **4.7.2** with two additions:

- **Grit** — compiles a project's GDScript into C++ and links it into a
  per-project export template, so game logic runs as native code instead of
  bytecode. Native code can be up to 80x faster in some cases. The editor can
  also turn a node's script into a GDExtension class on demand (see
  [GDExtension classes from scripts](#gdextension-classes-from-scripts)).
  
- **CFFI** — lets GDScript call into C libraries directly.

The CFFI reference is at [2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/)
and in [`docs/`](docs/). This repository has no CI and publishes no releases; editors and
build toolchains are produced locally.

Functions Grit cannot compile stay on the GDScript VM, and the export log names
each one with the reason.

## GDExtension classes from scripts

GDExtension classes are created from a source file, not from the scene tree. In
the FileSystem dock, **New Script…** offers **GDScript (GDExtension)**: it writes
a `.gde` script whose template declares the class and its base:

```gdscript
@gdextension_class_name("Player")
extends CharacterBody2D
```

`@gdextension_class_name` is a Grit annotation that GDScript ignores; the name
becomes the generated class. The script is a **class source**: it cannot be
attached to a node (the Attach Script dialog does not offer the language and
rejects `.gde` paths); nodes use the generated class as their type instead.

Creating the script generates the extension next to it:

```
res://player.gde
res://player_gdext/player.gdextension
res://player_gdext/SConstruct
res://player_gdext/src/...
res://player_gdext/.grit_manifest.json
```

Only the scaffold is generated; building the library is a separate step:
`--grit-gdx-generate res://player.gde --grit-gdx-build` builds it with the
configured SCons/godot-cpp toolchain (the `SConstruct` in the folder works too).
Running `--grit-gdx-generate` again after editing the source regenerates it and
keeps the same extension folder. A generated class whose source changed since the
library was built gets the amber badge on its class icon.

An extension folder holds:

| file | purpose |
|---|---|
| `.grit_manifest.json` | the classes, their `.gde` source, base type and built hash |
| `<name>.gdextension` | the descriptor the editor loads |
| `SConstruct` | builds the library against godot-cpp |
| `src/` | generated class sources and the class registry |
| `bin/lib<name>.<platform>.<arch>.<config>.<ext>` | the built library |

Projects converted by an older build (`.gd` + `.gds` archives and the
node-context actions) are upgraded once with `--grit-gdx-migrate`: each class
gets a `.gde` source in place of its script/archive and keeps its extension. The
old node-context actions are gone; generation and migration are driven from the
CLI, and creation from the New Script dialog.

Generated classes are ordinary godot-cpp classes: exported variables become
properties, `signal` declarations become signals, and a script-defined `_ready`
or `_process` overrides the native virtual. The class name is taken from the
annotation; a name that hides a native class is refused rather than renamed.

What converts: functions, exported variables, signals, virtual overrides,
`is`/`as` (built-in, native and preloaded script types), `assert`, optional
parameters, static functions, typed containers (`Array[T]`, `Dictionary[K, V]`)
and lambdas (`func(...): ...`, including captured values). What does not:
`await` — a GDExtension method cannot suspend and resume, so a method that awaits
is reported and stays behind — rest parameters (`...args`), `static var`, and
`is`/`as` on an inner-class script type.

Every action reloads the edited scene and, when a library changed, the extension
itself (the descriptors set `reloadable = true` and the libraries are built with
hot reload support), so the scene tree shows the new node type immediately and no
editor restart is needed. Adding a class to an extension reloads the shared
library in place.

godot-cpp ships with the toolchain (`10.0.0-stable`, the release that targets
this engine's GDExtension API, which also fixed a crash when the editor quit with
a library loaded) and its static libraries are built by the toolchain's own
Python and SCons, so converting never clones or builds godot-cpp by hand.

## Export Presets

There are no stock exports. Every project builds its own template on export:

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
   progress and a SHA-256 check for each file, and fetches godot-cpp
   (`10.0.0-stable`) so GDExtension conversions need no network access,
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
| Linux | Python, SCons, focal GCC 9 (glibc 2.31 sysroot), binutils, libffi, wayland-scanner, godot-cpp, Emscripten |
| Windows | Python, SCons, MinGW-w64, libffi, static ANGLE, Mesa NIR + Agility SDK, WinRT headers, godot-cpp, Emscripten |
| macOS | Python, SCons, MoltenVK, godot-cpp, Emscripten |

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
- Scripted nodes convert to GDExtension classes on macOS and Linux (the library
  is built by the toolchain's godot-cpp); conversion, conversion back and a
  fixture project suite are exercised, and the generated sources were also built
  and run with the Windows toolchain.
- Linux, Windows, macOS and HTML5 Fast templates build; the Linux and HTML5
  exports have been run and benchmarked, the Windows export has not been run on
  Windows yet.
