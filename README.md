<p align="center">
  <img src="docs/assets/fast-godot-logo.png" alt="Fast Godot" width="420">
</p>

Fast Godot is a custom build of Godot **4.7.2** with two additions:

- **Grit** — compiles a project's GDScript into the project's Fast export
  template as native C++, so game logic runs as native code instead of bytecode.
  Native code can be up to 80x faster in some cases.
  
- **CFFI** — lets GDScript call into C libraries directly.

The CFFI reference is at [2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/)
and in [`docs/`](docs/). This repository has no CI and publishes no releases; editors and
build toolchains are produced locally.

Functions Grit cannot compile stay on the GDScript VM, and the export log names
each one with the reason.

## Export Presets

There are no stock exports. Every project builds its own template on export:

| platform | preset | template |
|---|---|---|
| macOS arm64 | `macOS Fast` | `<project>/exports/macos_fast.zip` |
| Linux x86_64 | `Linux Fast` | `<project>/exports/linux_fast_<target>.<arch>` |
| Windows x86_64 | `Windows Fast` | `<project>/exports/windows_fast_<target>_<arch>.exe` |
| Web | `HTML5 Fast` | `<project>/exports/web_fast.zip` |

Native-platform presets are only offered on their own host, so a macOS editor
offers `macOS Fast` and ignores presets for Linux or Windows. HTML5 is the
exception: `HTML5 Fast` is offered on every host and is cross-compiled with the
toolchain's Emscripten.

The first export of a project compiles the engine for that project's template
(10–20 minutes on a warm machine); later exports only rebuild what changed.

A **macOS Fast** template is arm64, and the macOS exporter refuses an arm64
build unless ETC2/ASTC texture compression is enabled. This fork's macOS editor
therefore defaults `rendering/textures/vram_compression/import_etc2_astc` to on,
and the project manager writes it into every project it creates on macOS, so a
fresh project exports without touching that setting. Other platforms keep the
stock default (off). Existing projects that were imported before this default
was enabled may need their textures reimported to actually carry ETC2/ASTC data
(delete `.godot/imported` and let the editor reimport).

## First run: Download Build Tools

A fresh editor has no build tools, so the export window shows no presets — only a
centered **Download Build Tools** button. Pressing it:

1. fetches the engine source for this editor's commit once (only to run the
   downloader) and keeps a copy inside the new toolchain at
   `<toolchain>/engine/<revision>/`,
2. downloads every toolchain component from its original source into
   `~/fast_godot_toolchain` (Windows: `%USERPROFILE%\fast_godot_toolchain`), with
   progress and a SHA-256 check for each file,
3. validates the folder and points `grit/settings/toolchain_root_folder_path` at
   it.

After that the presets appear and the button never comes back. **Nothing is
downloaded after the toolchain is installed**: editor builds and exports use the
toolchain's engine source, and a toolchain without one fails with an error
instead of fetching the archive.

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
| all hosts | the engine source tree the bundle was built from (`engine/<revision>/`) |

Nothing else needs to be installed for an export: no system C++ compiler, X11,
ALSA, Wayland or libffi development packages. macOS still uses the Xcode Command
Line Tools for its compiler.

The engine source lives at `<toolchain>/engine/source/` and is recorded under
the `engine` key in `toolchain.json`. A bundle is therefore self-contained: editor
builds and export templates use that tree, and an older bundle without it is
reported as an error (press Download Build Tools again to get a source-carrying
one) — never downloaded at export time. There is no fallback: exports and editor
builds only ever use the engine tree the toolchain declares.

**Toolchains are not tied to an editor revision.** Nothing in the toolchain's
paths or `toolchain.json` names a commit, and no revision check or warning runs
at export time: a toolchain built once works with any 4.7.x editor, so rebuilding
the editor never requires re-downloading the build tools.

Creating a toolchain never touches a source checkout and never copies the engine
source around:

1. the editor downloads the `4.7.2` engine source archive (the repository's
   `4.7.2` branch — the 4.7.x line, not an individual commit);
2. the archive is unpacked and its `fast-godot-4.7.2/` folder is **renamed** to
   `<toolchain>/engine/source/` — nothing is copied;
3. the downloader that ships inside that tree fetches Python, SCons and the
   platform libraries, and writes `toolchain.json`.

The archive and the progress file live in a temporary `<toolchain>/.download`
folder that is deleted as soon as it is no longer needed, so after creation the
toolchain folder is self-contained and nothing is left behind. Running
`tools/toolchain/download_toolchain.sh` by hand from a checkout still copies
that checkout's tree into the toolchain, so manual toolchain builds work
unchanged. Template build caches (`out/grit-profiles`, `bin/grit`) are written
inside the toolchain's engine tree too, not into the source folder.

Note: the toolchain carries the engine source, Python, SCons and the platform
libraries (MoltenVK/ANGLE/Mesa/Mingw/libffi/emsdk as applicable), but on macOS
the C++ compiler and SDK still come from the installed Xcode Command Line Tools;
on Windows and Linux the compiler is bundled in the toolchain.

## Template build profile

By default a project's Fast export template is a full engine build. A project can
instead give its own template a Godot **build profile**: *Project Settings → Grit
→ Export → Build Profile* takes an **Engine Compilation Profile** file — the
`.build`/`.gdbuild` files the editor's build profile tool saves, which `scons
build_profile=<file>` also reads — a list of disabled classes and features:

```json
{
  "type": "build_profile",
  "disabled_classes": ["MovieWriter"],
  "disabled_build_options": { "module_gltf_enabled": false }
}
```

The template is then compiled with those classes/modules left out, which makes
it smaller and quicker to build and to load. The profile must keep everything
the project actually uses (including the engine calls Grit compiles): a missing
class fails the template build or the game. Leave the setting empty for the full
engine. The profile file's contents are part of the per-project template cache
key, so choosing a different profile rebuilds the template, and an unreadable or
malformed profile fails the export with an error instead of building anything.

## PCK encryption

A Fast export can be shipped with an encrypted PCK. Godot encrypts the pack with
a 256-bit AES key that is compiled into the export template, so in the stock
engine you have to build a custom template for every key. Grit builds the
project's template on export anyway, so the key can simply be configured:

1. open the export window and go to the **Encryption** section of the preset;
2. enable **Encrypt Exported PCK** (and optionally encrypt the directory and set
   include/exclude filters);
3. paste the **256-bit AES key as 64 hexadecimal characters**.

That's all: the export builds the project's Fast template with
`SCRIPT_AES256_ENCRYPTION_KEY` set to the same key, so the runtime can decrypt
the PCK it ships with, and the pack itself is written encrypted. The key is
stored in the project's `.godot/export_credentials.cfg`, never in
`export_presets.cfg`. `GODOT_SCRIPT_ENCRYPTION_KEY` overrides the preset key for
CI/command-line exports, matching the engine's behaviour.

The export uses a **random 256-bit data key per export**; the pack is encrypted
with that data key, and the template embeds only the data key *wrapped* with your
key. At pack-open a **white-box AES-256 table network** (the key baked into
randomized lookup tables with random GF(2) byte encodings between rounds, and a
fresh table layout per build) recomputes the data key, which is then used for the
pack. Neither your key nor the pack key exists as bytes or instruction
immediates in the binary — only the wrapped value and the encoded tables do.
The unwrap is two block encryptions at pack-open, so there is no runtime cost
worth mentioning; the template grows by roughly 220 KB.

The encrypted template also runs an **anti-debug check** before unwrapping
(macOS `ptrace(PT_DENY_ATTACH)` + `P_TRACED`, Windows `IsDebuggerPresent`, Linux
`/proc/self/status` `TracerPid`): a game started under a debugger does not get the
key, so the pack cannot be inspected from memory that way.

Caveat: this is our own white-box construction, not a peer-reviewed one. It
raises extraction far above "read the array/immediates" (automated tools that
scan for the key find nothing), but a determined analyst with the source and a
debugger can still invest in breaking the encodings (or bypass the anti-debug).
Treat it as strong obfuscation, not as unbreakable DRM. The user key is part of
the template's build identity (`out/grit-profiles`) so changing it rebuilds the
template; a missing or non-hex key fails the export before anything is built, and
a toolchain whose build helper predates encryption support is refused (re-download
the build tools with this editor to get the updated 4.7.2 source).

## Building the editor itself

`tools/build-editor.sh` (Linux, macOS) and `build-editor.cmd` in the Windows
toolchain folder find the source tree and the toolchain and run SCons with
`production=yes`:

```
tools/grit_build.py editor --toolchain <toolchain-folder> [--engine <checkout>]
```

By default the editor is built from the engine source shipped inside the
toolchain; `--engine <checkout>` selects a different tree. On macOS the build
needs MoltenVK; the downloaded toolchain provides it under `moltenvk/`, or pass
`vulkan_sdk_path=<toolchain>/moltenvk`.

## Toolchains by hand

Every component can also be assembled with the scripts in `tools/toolchain/`
(`fetch_linux_toolchain.sh`, which also copies the engine source into
`<bundle>/engine/<revision>/`, `make_linux_gcc.sh` with its pinned
`linux_gcc_packages.txt`, `make_bundle_zip.py`), and any folder can be selected
with the *ToolChain Root Folder Path* editor setting. A valid folder contains
`toolchain.json` and the components above.

## Engine fixes

The fork carries a fix for a null-token crash in
`ResourceLoader::_run_load_task()` that kills a worker thread when a scene loads
with `load_threaded_request(..., use_sub_threads = true)`.

## Status

- macOS, Linux and Windows editors build locally.
- Fast templates build on macOS, Linux and Windows; the Linux and HTML5 exports
  have been run and benchmarked, the Windows export has not been run on Windows
  yet.
