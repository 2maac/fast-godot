# Fast_Godot

Fast_Godot is a custom build of Godot **4.7.2** with two additions:

- **Grit** — compiles a project's GDScript into C++ and links it into a
  per-project export template, so game logic runs as native code instead of
  bytecode.
- **CFFI** — lets GDScript call into C libraries directly.

Both are documented in [`docs/`](docs/) and at
[2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/). Editors for
every supported platform are on the [releases page](https://github.com/2maac/fast-godot/releases).

## How fast is it?

Grit compiles at function granularity, so the gain depends on what the code
does. Measured on this fork (macOS arm64, GDScript, VM versus compiled):

| workload | VM | native | speedup |
|---|---|---|---|
| 3M iterations of integer arithmetic | 39.9 ms | 0.5 ms | ~80× |
| 1M calls to a small function | 64.0 ms | 5.8 ms | ~11× |

Straight-line code gains most; call-heavy code is bounded by the call boundary.
GPU-bound 3D scenes will not show a difference — there the renderer, not script,
sets the frame time.

Functions Grit cannot compile stay on the GDScript VM, and the export log names
each one with the reason. In practice this is a handful of edge cases.

## Export model: only Fast presets

There are no VM/stock exports. Every project builds its own template:

| platform | preset | template |
|---|---|---|
| macOS arm64 | `macOS Fast` | `<project>/exports/macos_fast.zip` |
| Linux x86_64 | `Linux Fast` | `<project>/exports/linux_fast_<target>.<arch>` |
| Windows x86_64 | `Windows Fast` | `<project>/exports/windows_fast_<target>_<arch>.exe` |

A platform is only registered on its own host, so a macOS editor offers
`macOS Fast` and ignores presets for other platforms. Nothing is
cross-compiled.

The first export of a project compiles the engine for that project's template
(10–20 minutes on a warm machine); later exports only rebuild what changed.

## Requirements

Building a template needs the **engine source tree**, which you point at with the
`grit/export/engine_root` project setting. The editor provisions what it can when
the export window opens:

| dependency | macOS | Linux | Windows |
|---|---|---|---|
| SCons | provisioned from PyPI | provisioned | provisioned (needs Python) |
| MoltenVK | provisioned from Khronos | — | — |
| compiler | Xcode Command Line Tools (manual) | distro gcc/clang (manual) | MinGW (manual) |
| engine tree | clone this repo | clone this repo | clone this repo |

Provisioned tools live under the editor's data directory (`…/tools`), and
everything is overridable in Editor Settings under `grit/provision/`.

## Building from source

```sh
git clone https://github.com/2maac/fast-godot
cd fast-godot
scons platform=<macos|linuxbsd|windows> target=editor -j"$(nproc)"
```

`accesskit` is disabled by default; `vulkan=yes` is what the shipped builds use.

## Status

- macOS and Linux editors build and export.
- The Windows editor **builds** (MinGW, D3D12 + Vulkan) but its exported games
  have not been run on Windows yet.
- Per-platform template compilation is native only; there is no cross-compilation
  and no VM fallback.
