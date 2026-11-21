<p align="center">
  <img src="docs/assets/fast-godot-logo.png" alt="Fast Godot" width="420">
</p>

Fast_Godot is a custom build of Godot **4.7.2** with two additions: **Grit**, which
compiles a project's GDScript into that project's export template as native C++, and
**CFFI**, which lets GDScript call C libraries directly. Export templates are no longer
shipped: every project builds its own **Fast** template from a self-contained build
toolchain. This repository has no CI and publishes no releases — editors, toolchains and
templates are all produced locally. The CFFI reference is at
[2maac.github.io/fast-godot](https://2maac.github.io/fast-godot/).

## Features

### Native compilation — Grit

- **GDScript functions compiled to C++ and linked into the project's own export template**,
  so game logic runs as native code instead of bytecode. Native code can be up to 80× faster
  in some cases.
- **Compilation happens at export time, per project**: the generated C++ lands in
  `<project>/.godot/grit/generated/` and is built against that project's engine configuration.
- **Compiled bodies are stripped from the exported scripts**, so the shipped bytecode does not
  carry the functions that became native.
- **Graceful fallback**: functions Grit cannot compile stay on the GDScript VM, and the export
  log names each one with the reason. An unsupported function never blocks an export.
- Common engine builtins are inlined in the generated C++, and a function-nesting runtime
  supports nested calls.

### C FFI — call C from GDScript

- **`CFFI` is an engine singleton** — no plugin, autoload or `preload`. It exposes
  `CFFILibraryHandle`, `CFFIFunction`, `CFFIType`, `CFFIPointer`, `CFFISpan`,
  `CFFIStructType`, `CFFIArrayType` and `CFFICallableFunction`.
- **libffi-backed**: `CFFI.open("libc.so.6")`, `get_function("strlen", "size_t", ["char*"])`,
  `invokev([...])` — calling conventions, struct layout and variadic arguments are handled for
  you.
- **Type strings** (`"int"`, `"char*"`, `"float[3]"`, `"const char**"`), user-defined structs
  with layout and `offset_of`, typed pointer reads and writes, bounded spans, UTF-8 string
  helpers, and a `StreamPeer` bridge.
- **Callbacks from C into GDScript** through `CFFICallableFunction`.
- **Checked conversions**: passing the wrong Variant for a declared C type is a runtime error,
  not a silent truncation.
- **Not included in web builds** — libffi has no wasm target.

### Export pipeline

- **No stock or VM export templates.** Every project builds its own Fast template on its first
  export and caches it in `res://exports/`; later exports rebuild only what changed.
- **Presets**: `macOS Fast` (arm64), `Linux Fast`, `Windows Fast` and `HTML5 Fast`. Native
  presets are only offered on their own host; HTML5 is offered everywhere and cross-compiled
  with the toolchain's Emscripten.
- **Per-project engine build profile**: a project can point at an Engine Compilation Profile
  (`.build`/`.gdbuild`) and the template is compiled with those classes and modules left out,
  making it smaller and quicker to build and load. The profile is validated and part of the
  template cache key.
- **Per-preset build options**: `grit/lto` (Automatic, None, Thin, Full) and `grit/optimize`
  (None, Speed, Size) choose link-time optimization and the optimization level of that
  preset's template — Speed is the default, Size shrinks the template. A debug template maps
  Speed to the engine's `speed_trace` (`-O2`) so backtraces stay usable.
- **Grit can be switched off per preset**: turn off `grit/enabled` in the preset's Grit
  section (or set `GRIT_DISABLE=1` in the editor's environment — the variable wins, and the
  running game honours it too) and the export compiles no script: nothing is stripped, a plain
  engine template is built, and the game runs entirely on the GDScript VM. Useful for a
  baseline build, to isolate a generated-code problem, or for projects that must not ship
  native code.
- **CI-friendly**: `GODOT_SCRIPT_ENCRYPTION_KEY` overrides the preset encryption key,
  `GRIT_DISABLE=1` forces a VM-only export, and `--export-release` works headless from the
  command line.

### PCK encryption

- **A 256-bit key can be configured in the preset's Encryption section**, or generated with the
  **random-key button** (operating-system CSPRNG, not reproducible). Include filters default to
  `*` when encryption is enabled; *Encrypt Index* hides file names too. No custom engine build
  per key is needed, because the template is built per project anyway.
- **Each export encrypts the pack with a new random 256-bit pack key** and builds the template
  against that same key, so there is nothing to wrap and nothing to unwrap.
- **White-box keystream**: the pack key is baked into an AES-256 table network with randomized
  lookup tables and per-binary layout, so it never exists in the game as bytes or instruction
  immediates. The game decodes the pack's CFB stream one block at a time through the tables
  (on demand, not by buffering the file), so no key ever reaches the process. Table entries are
  masked and the tables are shuffled with decoys; your own key only selects the layout and the
  build tree.
- **Anti-debug check before the first block** (macOS `ptrace(PT_DENY_ATTACH)`/`P_TRACED`, Windows
  `IsDebuggerPresent`, Linux `TracerPid`): a traced game gets only zeroed blocks, so the pack
  fails to open. Web builds have no equivalent hook, which makes HTML5 the weakest target.
- **Memory hygiene**: the decrypted payload is excluded from dumps and wiped when the file
  closes. Release templates get no core dumps, and on Linux a non-dumpable process.
- The key is stored in `.godot/export_credentials.cfg`, never in `export_presets.cfg`.
- **Caveat**: this is hardened obfuscation, not DRM. It defeats automated key recovery, but a
  determined analyst can still disassemble the tables and emulate the keystream, or dump the
  decrypted assets from a running game.

### Toolchain

- **One-click *Download Build Tools* in the editor**, or `tools/toolchain/download_toolchain.*`
  by hand: a self-contained folder with Python, SCons, the platform compiler and libraries,
  plus the engine source. Every file is SHA-256 checked and progress is reported.
- **Linux**: focal GCC 9 targeting **glibc 2.17**, libffi, wayland-scanner and Emscripten. X11
  and ALSA headers are vendored and the libraries are `dlopen`ed, so no system development
  packages are needed.
- **Windows**: MinGW-w64 (UCRT), libffi, static ANGLE, Mesa NIR + Agility SDK, WinRT headers
  and Emscripten.
- **macOS**: MoltenVK and Emscripten; the C++ compiler and SDK still come from the Xcode
  Command Line Tools.
- **Revision-free**: nothing in the toolchain names a commit, so rebuilding the editor never
  needs a new download, and nothing is fetched after the toolchain exists.

### Platform and engine behaviour

- **HTML5 Fast is single-threaded** (`threads=no`), so it needs no `SharedArrayBuffer`,
  COOP/COEP headers or special hosting. `grit/optimize` controls the wasm: Speed (`-O3`,
  ~46 MB) is the default and Size (`-Os`, ~38.5 MB) shrinks the download.
- **macOS defaults ETC2/ASTC texture import on** (an arm64 template is refused without it) and
  writes it into projects created on macOS; other platforms keep the stock default.
- With `grit/lto` on Automatic, Linux templates use LTO and macOS and Windows do not; a preset
  can force Thin/Full or turn LTO off.
- AccessKit is disabled (`--accessibility-driver` accepts only `dummy`), and the custom-template
  options are gone from presets.
- Includes a fix for a null-token crash in `ResourceLoader::_run_load_task()` when loading
  scenes with `use_sub_threads = true`.
- Fast_Godot branding: app icon, editor and boot splashes, and the Fast artwork as the default
  project icon for new projects.

## Building from source

The editor is built with a Fast_Godot **toolchain**: a self-contained folder holding Python,
SCons, the platform compiler and libraries, and the engine source. There are no prebuilt
editors, toolchains or templates.

**1. Get a toolchain.** From a checkout, run the downloader for your platform. It installs into
`~/fast_godot_toolchain` (`%USERPROFILE%\fast_godot_toolchain` on Windows) and copies this
checkout's engine source into it. The folder is roughly 2.5 GB.

```sh
tools/toolchain/download_toolchain.sh          # Linux, macOS
```

```bat
tools\toolchain\download_toolchain.cmd         :: Windows
```

The downloaders use only tools that ship with the OS (`curl`, `tar`, and `certutil` on
Windows). On macOS the C++ compiler and SDK come from the Xcode Command Line Tools, which must
be installed; Linux and Windows need nothing else.

**2. Build the editor.** The wrappers find the source tree and hand everything to
`tools/grit_build.py editor`:

```sh
tools/build-editor.sh --engine . --toolchain ~/fast_godot_toolchain       # Linux, macOS
```

```bat
tools\build-editor.cmd -Engine . -Toolchain %USERPROFILE%\fast_godot_toolchain   :: Windows
```

Or call the build script directly with the toolchain's own interpreter:

```sh
<toolchain>/python/bin/python3 tools/grit_build.py editor \
    --toolchain <toolchain> --engine .
```

Pass `--jobs N` to change build parallelism. The editor is written to
`bin/godot.macos.editor.arm64`, `bin/godot.linuxbsd.editor.x86_64` or
`bin/godot.windows.editor.x86_64.exe` (plus a `.console.exe` variant on Windows).

**3. Run it.** No export templates are staged: adding an export preset and exporting builds
that project's Fast template. The first export of a project takes 10–20 minutes on a warm
machine; later exports only rebuild what changed.

Toolchains can also be selected with the editor setting *ToolChain Root Folder Path*, and
every component can be assembled by hand with the scripts in `tools/toolchain/`
(`fetch_linux_toolchain.sh`, `make_linux_gcc.sh` with its pinned package list, and
`make_bundle_zip.py`).
