# Handover — Grit compiled exports (Fast_Godot)

Written after a long session that implemented compiled exports end to end, and that also
broke the build tree several times. Read the "Gotchas" section before touching the tree; it
is the part that cost the most time.

## Where things live

| path | what |
|---|---|
| `~/dev/fast-godot` | the engine + modules, git repo, remote `git@github.com:2maac/fast-godot.git` |
| `~/dev/fast-godot/bin/godot.macos.editor.arm64` | editor binary (bare Mach-O, not an .app) |
| `~/dev/fast-godot/bin/godot.macos.template_{release,debug}.arm64` | export templates |
| `~/dev/fast-godot/bin/grit/{release,debug}/` | per-target build manifest: `flags.txt`, `link.txt`, `scons_args.txt`, `last_build.log` (**gitignored, machine-specific**) |
| `~/dev/fast-godot/bin/grit_templates/<project>_<target>_compiled.zip` | artifacts built by a compiled export |
| `/tmp/gritproj` | throwaway test project (**/tmp may be wiped**) |
| `/tmp/bvenv/bin/scons` | SCons venv used for every build (**/tmp may be wiped**) |
| `~/VulkanSDK/1.4.363.1` | Vulkan SDK / MoltenVK |
| `/tmp/fakehome` | isolated `HOME` for headless tests, keeps the real Godot config clean |

Git: HEAD is `4a36c09`. Earlier feature commits are `e0f042e` … `560f4f3`.

## How a compiled export works today

1. Toolbar dropdown (left of the play buttons, `editor/editor_node.cpp`) sets the project
   setting `grit/export_mode`: `0` = VM, `1` = Compiled. Debug vs release comes from the
   export dialog's **Export With Debug** checkbox.
2. `modules/grit/editor/grit_export_plugin.cpp`, `_export_begin`:
   - VM mode: returns immediately, stock export, no code generation.
   - Compiled mode: runs the project compiler (writes `.godot/grit/generated/*.gen.cpp`), then
     shells out to `tools/grit_build_template.sh` as a **child process**, tailing
     `bin/grit/<target>/last_build.log` inside an `EditorProgress` dialog.
3. `tools/grit_build_template.sh` runs a **real SCons build** in an isolated copy (below),
   with `build_profile=` and `grit_generated=<this project's generated sources>`, and wraps the
   result into a `macos_template.app` zip containing
   `Contents/MacOS/godot_macos_{release,debug}.arm64`. There is no relink shortcut any more:
   the Grit module is only compiled when `grit_generated` is set (see
   `modules/grit/config.py`), so a stock template contains no Grit and there would be nothing
   to relink against.
4. Steps 2 and 3 run from `prepare_export()`, which the editor calls **before** the platform
   starts (see gotcha #6), so a build failure or Esc aborts the export with nothing written.
   The plugin then points the preset's `custom_template/{release,debug}` at the zip; the
   platform reads it during packing, and `_export_end` puts the original values back, so the
   write is invisible to later exports and a plain VM export keeps using the stock template.
5. The helper writes a fingerprint into the pack; at startup `GritRegistry` compares it with
   the template's. Mismatch → VM fallback with a warning; with `grit/strip_scripts` on →
   deliberate `CRASH_NOW_MSG`.

`_build_template` needs:
- project setting `grit/export/engine_root` = path to this tree;
- `scons` on `PATH`, or recorded in `bin/grit/scons_path.txt` (`tools/grit_scons_bin.sh`);
- anything `bin/grit/<target>/scons_args.txt` wants to override (SDK path, AccessKit).

The profile argument is always supplied: the plugin writes the project's
`grit/build_profile_json`, or a minimal default profile when the project has none, so the
helper always takes the isolated-build path.

`bin/grit/<target>/scons_args.txt` is optional: `tools/grit_scons_args.sh` resolves the extra
SCons options (`vulkan_sdk_path`, `accesskit`, ...) from that file when present, otherwise by
finding the newest `~/VulkanSDK/<ver>` or `$VULKAN_SDK` that has
`macOS/lib/MoltenVK.xcframework`.

A first build for a profile is full (~2.5 min, 1969 files for a 2D profile); repeat exports
reuse the work tree and take ~10 s.

## Provisioned tools directory

Everything the provisioner installs that is not the MoltenVK SDK goes into one place:

```
<editor data dir>/tools/            (override: grit/provision/tools_dir)
  scons/4.11.1/scons-4.11.1/        SCons sdist, extracted
  scons/4.11.1/bin/scons            generated launcher (scons.cmd on Windows)
```

SCons is **not bundled in the editor**: it is downloaded from PyPI (pinned sdist plus sha256)
because the sdist keeps the package intact, while bundling would tie the editor release to a
Python version. The launcher runs `python3 -m SCons` with `PYTHONPATH` pointing at the extracted
package — modern sdists have no `script/scons.py`. The interpreter is discovered as `python3`,
`python`, then `py -3` (Windows), and the launcher is a `.cmd` there.

After installing, the launcher path is written to `bin/grit/scons_path.txt`, so
`tools/grit_scons_bin.sh` picks it up with no other change.

Verified on macOS with an empty HOME and a stripped environment (`env -i PATH=/usr/bin:/bin`):
download, checksum, extract, launcher, and `grit_scons_bin.sh` all work. Linux follows the same
path. Windows has the cross-platform branches but needs a real run, and a machine there still
lacks the rest of the toolchain (MinGW, libffi, and an `sh` for the build helper).

## Threaded loading crash (patched in core)

`ResourceLoader::_run_load_task()` ended by calling `load_task.load_token->unreference()`, and the
pointer can be **null** by then: the task entry is reachable through a reference into
`thread_load_tasks`, a `HashMap` that other threads rehash when nested loads insert entries. The
reference is read after that, so the token field comes back as garbage (null in practice) and the
process dies with `EXC_BAD_ACCESS` on a worker thread — which happens with official 4.7.2 too
(7/8 runs on the tps-demo), so it is an upstream defect, not a Grit one.

Both release sites are now null-guarded. Verified with a reproducer that mimics the tps-demo's
Play button (`load_threaded_request` of its level with sub-threads): 3/8 runs crashed before,
32/32 clean after.

The guard stops the crash; the underlying lifetime problem remains — several paths keep a
`ThreadLoadTask &` into a rehashing `HashMap`. A complete fix needs stable storage for those
entries (pointers, or copying the fields that outlive the lock).

## Provisioning build tools

Compiled exports need a compiler, scons, MoltenVK and the engine tree. `GritProvisioner`
(`modules/grit/editor/grit_provisioner.cpp`) checks them when the **export window is opened**
(`EditorExportPlugin::export_dialog_opened()`, once per session; set `GRIT_PROBE_NOW=1` to
run it at startup for headless testing) — not at editor startup, which
kept firing during command-line exports. One line when everything is present, a modal
`EditorProgress` with **no cancel button and Esc ignored** when something is missing —
provisioning is not optional.

It then downloads and installs what it can, from pinned sources with pinned sha256:

| item | source | installed to |
|---|---|---|
| MoltenVK | `KhronosGroup/MoltenVK` v1.4.2, `MoltenVK-macos.tar` | `~/VulkanSDK/<ver>/MoltenVK/MoltenVK.xcframework` — the layout `get_mvk_sdk_path()` already recognises |
| MinGW (winlibs) | `brechtsanders/winlibs_mingw`, gcc-16.2.0 r3 | (not yet installed by the provisioner) |
| scons, tree, compiler | detected only | — |

Everything is overridable in Editor Settings under `grit/provision/`: per-source `url`,
`mirrors`, `sha256` and `version`, plus `offline`, `proxy` and `allow_unverified`. Empty means
"use the built-in default", and `grit/provision/sources_version` re-applies the defaults when the
code's pinned sources change — changing a default alone does not migrate an existing install.

Downloads use `curl` (TLS, redirects, proxies, system certificates) and are hashed with
`HashingContext` before extraction; a checksum mismatch deletes the download. Extraction uses
`tar`, which handles both `.tar` and `.zip`.

Traps found while building this:
- The MoltenVK release archive ships a **dynamic and a static** framework. Only the static one has
  `libMoltenVK.a`, which is what Godot links; the installer must pick that one.
- The static framework contains `Versions/Current` symlinks, so a naive recursive search over the
  extracted tree loops forever. Depth-cap it and skip links.
- A stale `vulkan_sdk_path=` in `bin/grit/<target>/scons_args.txt` silently overrides detection,
  so a provisioned SDK can be installed and never used.

## Stripping compiled scripts

The export window's "Export Project..." file dialog carries a **Strip Scripts** checkbox next to
"Export With Debug". It is disabled (and cleared) while debug is ticked, because stripping only
ever applies to a release build — Grit leaves debug templates intact.

`FileDialog::get_option_checkbox()` exists so the export dialog can reach an option's checkbox and
enforce that dependency.

The choice travels to the plugin as project metadata (`export_options/strip_scripts`), falling back
to the `grit/strip_scripts` project setting for command-line exports and "Export All". A release
export with it on reports what it did:

```
Grit: stripped the bodies of 6 functions from the exported scripts.
```

## Only Fast platforms exist

There are no VM exports in this fork. Each host registers exactly one export platform, and it is
the Fast one:

| host | platform | template |
|---|---|---|
| macOS | `macOS Fast` | `<project>/exports/macos_fast.zip` |
| Linux | `Linux Fast` | `<project>/exports/linux_fast_<target>.<arch>` |
| Windows | `Windows Fast` | `<project>/exports/windows_fast_<target>_<arch>.exe` |

A platform is only registered when the editor runs on its host
(`OS::get_singleton()->get_name()`), so a macOS editor offers only `macOS Fast`; presets for other
platforms are ignored outright. Nothing is cross-compiled and nothing is downloaded as a stock
template: every export builds its template from the project.

That means the editor needs a working toolchain (engine tree, scons, compiler, MoltenVK on
macOS), which is what the provisioner checks and installs when the export window opens.

## Provisioning build tools

Compiled exports need a compiler, scons, MoltenVK and the engine tree. `GritProvisioner`
(`modules/grit/editor/grit_provisioner.cpp`) checks them when the **export window is opened**
(`EditorExportPlugin::export_dialog_opened()`, once per session; set `GRIT_PROBE_NOW=1` to
run it at startup for headless testing) — not at editor startup, which
kept firing during command-line exports. One line when everything is present, a modal
`EditorProgress` with **no cancel button and Esc ignored** when something is missing —
provisioning is not optional.

It then downloads and installs what it can, from pinned sources with pinned sha256:

| item | source | installed to |
|---|---|---|
| MoltenVK | `KhronosGroup/MoltenVK` v1.4.2, `MoltenVK-macos.tar` | `~/VulkanSDK/<ver>/MoltenVK/MoltenVK.xcframework` — the layout `get_mvk_sdk_path()` already recognises |
| MinGW (winlibs) | `brechtsanders/winlibs_mingw`, gcc-16.2.0 r3 | (not yet installed by the provisioner) |
| scons, tree, compiler | detected only | — |

Everything is overridable in Editor Settings under `grit/provision/`: per-source `url`,
`mirrors`, `sha256` and `version`, plus `offline`, `proxy` and `allow_unverified`. Empty means
"use the built-in default", and `grit/provision/sources_version` re-applies the defaults when the
code's pinned sources change — changing a default alone does not migrate an existing install.

Downloads use `curl` (TLS, redirects, proxies, system certificates) and are hashed with
`HashingContext` before extraction; a checksum mismatch deletes the download. Extraction uses
`tar`, which handles both `.tar` and `.zip`.

Traps found while building this:
- The MoltenVK release archive ships a **dynamic and a static** framework. Only the static one has
  `libMoltenVK.a`, which is what Godot links; the installer must pick that one.
- The static framework contains `Versions/Current` symlinks, so a naive recursive search over the
  extracted tree loops forever. Depth-cap it and skip links.
- A stale `vulkan_sdk_path=` in `bin/grit/<target>/scons_args.txt` silently overrides detection,
  so a provisioned SDK can be installed and never used.

## Fast builds are native to their platform

Each platform has a Fast preset, and each Fast template is built by **that platform's own editor**
— macOS Fast on macOS, Linux Fast on Linux, Windows Fast on Windows. There is no
cross-compilation anywhere in the tree.

The Fast platform is only **registered on its own host** (`register_*_exporter()` guards it with
`OS::get_singleton()->get_name()`), so on a macOS editor the presets for "Linux Fast" and
"Windows Fast" are not offered at all — `export_presets.cfg` entries for them are ignored, and the
editor reports only the presets it can honour.

Each editor provisions the toolchain for its own Fast builds when the export window opens
(clang on macOS/Linux, mingw-w64 plus a libffi prefix on Windows).

## Stock (VM) templates live in Godot's default directory

Because stock templates are built by CI, downloaded, and **never modified**, they live where Godot
expects them: `EditorPaths::get_export_templates_dir()/<version>/` (`macos.zip`,
`linux_{release,debug}.x86_64`, `windows_*_x86_64.exe`). Only Fast templates are per project, in
`<project>/exports/`.

They are built without `grit_generated`, so `modules/grit/config.py` leaves the module out
entirely: **stock templates contain no Grit code**. The only addition over upstream Godot is the
CFFI module.

## Stock (VM) templates come from the releases

VM presets use the prebuilt templates published with each GitHub release, fetched into
`<project>/exports/` by the provisioner. They are built without `grit_generated`, so
`modules/grit/config.py` leaves the module out entirely: **the stock templates contain no Grit
code**. The only addition over upstream Godot is the CFFI module.

## Export platforms that exist

Desktop only, and each has a Fast variant:

| platform | Fast variant | template files |
|---|---|---|
| `Linux` | `Linux Fast` | stock `linux_{release,debug}.x86_64` / Fast `linux_fast_*` |
| `macOS` | `macOS Fast` | stock `macos.zip` / Fast `macos_fast.zip` |
| `Windows Desktop` | `Windows Fast` | stock `windows_*_x86_64.exe` / Fast `windows_fast_*.exe` |

The Fast variants are subclasses that only override `get_name()` and
`get_template_file_name()` (the `_fast` suffix keeps them from ever picking up or
overwriting a stock template).

Adding a preset provisions what that platform needs: `macOS` builds its stock template from the
tree, `Linux`/`Windows` fetch their stock bundle from this repo's release assets, and `macOS Fast`
builds the project's compiled template. Adding `Linux Fast` / `Windows Fast` explains that those
are built on their own host instead of attempting an impossible build, and an export from the
wrong host refuses the same way.


`SConstruct` filters `platform_exporters` to `linuxbsd`, `macos` and `windows`, so the export
dialog offers only desktop targets — no Android, iOS, visionOS or Web. "macOS Fast" comes free:
it is registered inside `register_macos_exporter()` (`platform/macos/export/export.cpp`), so it
appears whenever macOS does.

The "Manage Export Templates" link was removed from the export window: this fork builds its
templates instead of downloading them, and never uses the shared export-templates directory.

## Testing the editor: run the staged copy, never the tree binary

The editor is the end product, and the build tree is not a safe place to run it. A profile or
template build regenerates shared headers in the tree (`core/disabled_classes.gen.h`,
`modules/modules_enabled.gen.h`), so a binary sitting in `bin/` can be rebuilt into something
other than what was tested — that is how a tree-shaken build once produced a 493-class editor.

Build, stage, then test the staged copy:

```sh
/tmp/bvenv/bin/scons platform=macos target=editor -j10 ccflags=-fmodules-cache-path=/tmp/clangmods
./tools/grit_stage_editor.sh "$PWD"
out/editor-staging/run.sh -e --path /tmp/gritproj
```

`out/editor-staging/` contains **only** `Fast_Godot.app` — nothing else:

- `misc/dist/macos_tools.app` with the built binary at `Contents/MacOS/Godot`, ad-hoc codesigned;
- no launcher, no private HOME, no verification file, and no export templates (those live in
  `<project>/exports/`).

The bundle is relocatable: it links nothing but system libraries, so copying it to `/tmp` or
`/Applications` and launching it works. What it cannot do by itself is *find the engine tree*,
which compiled exports need to build templates. `grit/export/engine_root` covers that per
project, and the tree is also remembered in `EditorSettings` (`grit/engine_root`, written lazily
because `EDITOR_DEF` cannot run during module init) so an app moved away from its build tree
still resolves it:

```
Grit: using the engine tree remembered at "/Users/orangetek/dev/fast-godot".
```

The script verifies the **copy**, not the tree binary, and exits 2 below ~1000 classes (the
493-class symptom). `GRIT_EDITOR_STAGE` moves the directory; `GRIT_TEMPLATES_SRC` overrides the
templates source. `out/` is gitignored, so staging never touches the repository.

## Platforms: "macOS" and "macOS Fast"

There is no editor-wide VM/Compiled switch any more, and no build-profile gear button. The choice
is the export platform, so it belongs to the preset:

| platform | what an export produces |
|---|---|
| `macOS` | stock Godot template, scripts ship as GDScript |
| `macOS Fast` | per-project template built from the project's generated code, scripts run natively |

**Every template this fork uses lives in `<project>/exports/`** — `macos.zip` for the plain
platform, `macos_fast.zip` for the fast one. `EditorExportPlatformMacOS::find_export_template()`
is overridden to look there instead of the shared export-templates directory, which paves the way
for a per-project tree-shaken template: one project's build can never affect another's, and the
global directory is not used at all.

`EditorExportPlatformMacOSFast` (`platform/macos/export/export_plugin_fast.h`) is the macOS
platform with a different `get_name()`, registered next to it in `platform/macos/export/export.cpp`.
It resolves its template the same way and looks for **`<project>/exports/macos_fast.zip`** — the compiled template belongs to the project, so
it travels with it and cannot collide with the shared stock archive. A compiled template is never
written into a preset's `custom_template` fields; the stock `macos.zip` in the export-templates
directory stays untouched, so VM exports keep using the Grit-free stock template. The `.zip` under
`res://exports/` is not packed into the exported app (it is not a resource type), so keeping it in
the project costs nothing at export time.
The Grit export plugin keys off that name (`_is_fast_platform()`); everything else — prepare,
build, preset restore — is unchanged. A new `macOS`/`macOS Fast` preset also gets its
architecture set to `arm64`, since the build products are arm64-only.

Removing a `macOS Fast` preset deletes the project's compiled templates
(`bin/grit_templates/<project>_{debug,release}_compiled.zip`) — but only when no other
`macOS Fast` preset remains, since the templates are per project rather than per preset.
That runs through `EditorExportPlugin::export_platform_removed()`, called before the preset
is removed so the plugin can see it.

A macOS preset with an empty `application/bundle_identifier` cannot export ("Invalid bundle
identifier: Identifier is missing"), and a fresh preset ships it empty. Grit fills in
`fast.<project>` (the project name lowercased, unsupported characters as hyphens) when the platform is added, when the export dialog builds its options, and again
just before an export, so the user never has to invent one; a value that is already set is never
touched.

Build profiles are still read from `grit/build_profile_json`; with the gear gone there is no
editor UI for editing it, so set it in Project Settings.

## First export: what happens automatically

A fresh project needs two things before it can export; both are handled on first use rather than
being prerequisites the user has to discover:

- **`grit/export/engine_root`** — when it is unset, `_resolve_engine_root()` walks up from the
  running executable until it finds a directory with `SConstruct` and
  `tools/grit_build_template.sh`, then saves it as a project setting and says so in the Output
  panel. This works for the tree binary *and* for a staged `Fast_Godot.app` (whose executable
  sits several levels down in `out/editor-staging/...`). Only if that walk fails does it print
  the "set the project setting" error.
- **The stock export template** — a VM export needs `export_templates/<version>/macos.zip`.
  The trigger is **adding the platform** in the export settings:
  `EditorExport::add_export_preset()` calls `EditorExportPlugin::export_platform_added()`, and
  Grit uses it to (a) set a new macOS preset's architecture to `arm64`, because the templates
  this engine builds are arm64-only and a fresh preset defaults to `universal`, and (b) run
  `tools/grit_install_templates.sh`.

  That installer checks the build environment **before** starting anything (engine tree found,
  `scons` resolvable through `tools/grit_scons_bin.sh`), then builds `template_release` /
  `template_debug` only if `bin/godot.macos.template_*.arm64` are missing, and finally assembles
  `macos.zip` from `misc/dist/macos_template.app` plus those binaries — which are Grit-free.
  It is built from the tree, never downloaded. Progress is reported through `EditorProgress`
  while the editor tails `bin/grit/templates_build.log`.

  `_ensure_stock_templates()` still runs when the export dialog opens and before `can_export()`
  on the command line, as a second chance if a preset was created some other way.

## Quickstart

```sh
# build everything (from the tree root)
/tmp/bvenv/bin/scons platform=macos target=editor           -j10 ccflags=-fmodules-cache-path=/tmp/clangmods
/tmp/bvenv/bin/scons platform=macos target=template_release -j10 ccflags=-fmodules-cache-path=/tmp/clangmods
/tmp/bvenv/bin/scons platform=macos target=template_debug   -j10 ccflags=-fmodules-cache-path=/tmp/clangmods

# optional: record where scons lives, so a GUI-launched editor can find it
mkdir -p bin/grit && printf '/tmp/bvenv/bin/scons\n' > bin/grit/scons_path.txt
# optional: override the auto-detected SDK path for profile builds
printf 'vulkan_sdk_path=%s/VulkanSDK/1.4.363.1 accesskit=yes\n' "$HOME" > bin/grit/release/scons_args.txt
```

Sanity check the editor **first** — this one command catches a broken binary:

```sh
cat > /tmp/cls.gd <<'EOF'
extends SceneTree
func _initialize() -> void:
	print("total=", ClassDB.get_class_list().size())
	print("3d=", ClassDB.class_exists("StandardMaterial3D"))
	quit()
EOF
HOME=/tmp/fakehome bin/godot.macos.editor.arm64 --headless --script /tmp/cls.gd
```

**Expected: `total=1077`, `3d=true`.** (1077 = 1081 minus the four `EditorExportPlatform*`
classes that left with the Android, iOS, visionOS and Web exporters.) If you see **493 / false**, the build is broken — see
the next section.

Headless export test:

```sh
cd /tmp/gritproj
HOME=/tmp/fakehome PATH="/tmp/bvenv/bin:$PATH" \
  ~/dev/fast-godot/bin/godot.macos.editor.arm64 --headless --path /tmp/gritproj \
  --export-release "macOS" /tmp/out/Game.zip
# success: "Grit: compiled executable written to ...", exactly ONE "Started Exporting"
# then run the produced .app binary with --headless --quit --verbose and look for
#   Grit: N native functions available.   /   natively.   /   N=... MS=...
```

## Gotchas (each one cost real time)

### 1. A broken editor binary reports 493 classes instead of 1077 — check this first

Symptom: `ERROR: In Object of type 'TabContainer': Attempt to connect nonexistent signal
'maximum_size_changed'`, `Class 'StandardMaterial3D' isn't exposed`, then a crash inside
`mvk::SPIRVToMSLConverter::convert` during export.

Cause: the binary was built without 3D classes. In this session that came from **stale SCons
state files surviving `rm -rf bin/obj`**.

Fix: delete `.sconsign5.dblite`, `.scons_env.json`, `.scons_node_count` **and** `bin/obj`,
then rebuild. Verify with the class-count check above. A rebuild that compiles ~2923 files is
a real full build; a suspiciously fast "successful" rebuild is not.

### 2. A build profile must never be built in this tree

The profile feeds SCons `build_profile=`, which sets `disabled_classes` and
`disabled_build_options`. Those regenerate **shared headers in the source tree** —
`core/disabled_classes.gen.h` (`core/SCsub:173`, a `CommandNoCache`) and
`modules/modules_enabled.gen.h` — so whichever target built last decides their contents, while
the editor's objects were compiled against the previous contents. Symptoms:
`Undefined symbols: initialize_text_server_fb_module(...)` at link, and the 493-class editor.

`grit_build_template.sh` runs a profile build in an isolated copy keyed by
`sha256(profile) + target` under `out/grit-profiles/<key>/`, and copies only the finished
template back (`GRIT_PROFILE_WORKDIR` overrides the location). The copy is refreshed from this
checkout with `rsync --delete` on every run, so a new commit costs only the files it changed
instead of a fresh tree and a full build. The first build for a **new profile** is still a
full one (~2460 files — a copied tree does not reuse SCons signatures even though `sconsign`
keys are relative); later exports recompile just the generated code (measured 8–9 s).

Two things the refresh must not copy:
- **`*.gen.*`** — generated per configuration. Copying the other tree's
  `core/disabled_classes.gen.h`, `modules/modules_enabled.gen.h` or `core/version_hash.gen.cpp`
  over this tree's made SCons recompile **433 dependents on every single run** (49 s each)
  before regenerating them.
- **SCons' own state**: `bin/`, `out/`, `.scons*`, `compile_commands.json`.

A tree that has had generated files synced into it is **poisoned, not self-healing**: delete it.
The symptom is `undefined symbol: initialize_<module>_module` at link while
`modules/modules_enabled.gen.h` still enables modules whose `libmodule_*.a` was never built for
this profile. Rebuilding from a clean copy is the only fix; the cache key does not change, so
remove `out/grit-profiles/<key>/` by hand.

A plain `template_release` build does not damage the editor — verified, 1077 before and after.

The full rebuild inherits `-fmodules-cache-path` from `bin/grit/<target>/flags.txt`. Without it
the build dies under a sandbox with `could not build module 'std'` /
`unable to open output file .../C/clang/ModuleCache/....pcm: Operation not permitted` — the
same trap as gotcha #1, but reached through a full SCons build rather than the relink path.

### 3. `OS::execute` from the editor does not inherit your shell environment

`GRIT_SCONS_ARGS` never reached the helper. Configuration must come from **files**
(`bin/grit/<target>/scons_args.txt`), not env vars.

### 4. `vulkan_sdk_path` must be the SDK root

`~/VulkanSDK/1.4.363.1`, **not** `.../macOS`. `detect_mvk()` looks for
`<path>/macOS/lib/MoltenVK.xcframework/<arch>/libMoltenVK.a`. Passing the subdirectory appears
to work under your real `HOME` only because auto-detection scans `~/VulkanSDK`; under an
isolated `HOME` it fails. That difference cost a debugging cycle.

### 5. The macOS export template is a zip with a specific shape

Root entry must be `macos_template.app`, and the binary inside
`Contents/MacOS/godot_macos_release.arm64` (or `debug`). A bare binary, or a differently named
app directory, gives `Requested template binary … not found`.

### 6. Preparing work that can fail: the `prepare_export` hook

`_export_begin` is `void`, so it cannot stop an export. `EditorExportPlugin::prepare_export()`
(fork-specific) runs from `_editor_export_project_*` in `project_export.cpp`, the Export All
loop, and `EditorNode::_fs_changed` — **before** the platform writes anything — and returns an
`Error`. That is where Grit now does code generation and the template build, so a failed or
cancelled build aborts the export cleanly: no artifact, no platform error, and the preset is
never touched. `_export_begin` just adds the fingerprint file and lets the platform pack.

`_export_begin` still has a fallback that builds when `prepare_export` was never called (some
export paths bypass the editor's dialog); there a failure points the preset at a nonexistent
template so the platform stops instead of exporting a VM app.

The preset is restored in `_export_end`, which runs in the `ExportNotifier` destructor after the
platform has packed everything. This matters because `EditorExportPreset::_set` calls
`save_presets()` on **every** property change: a leftover `custom_template/release` in
`export_presets.cfg` made the next plain VM export run a compiled binary. Settings are written
by a timer (`EditorExport::save_presets` only starts it), so in the GUI the restore lands on
disk a moment later; a headless `--export-release` process often exits before the timer fires,
which is why the file can look stale after a headless run without meaning it was not restored.

### 7. Progress + cancel

- The export is synchronous on the main thread, so a **click on the dialog's Cancel button can
  never work** — no GUI frame runs. Button is hidden; **Esc** is polled (every 30 ms) through
  `DisplayServer::process_events()`.
- Cancel writes a marker file; the **helper** kills its own `scons` child and exits 3. Do not
  kill the grandchild via `OS::kill` — it errors with "does not exist or is not a child of the
  calling process" (that is `drivers/unix/os_unix.cpp`'s `ECHILD` path).
- Repaint only when the label or percentage changes, otherwise the dialog flickers.
- The log is truncated by the **editor** before spawning the helper, not only by the helper.
  Otherwise the first poll can read the previous build's last line and display it as this
  build's progress — it looked like a profile build was running during a plain relink.

### 8. Naming rules learned

- Compiled artifacts are `<project>_<target>_compiled.zip`, not `godot.macos.template_*`, so
  they can never be confused with, or overwrite, a stock template.
- Release assets use `Fast_Godot_v<ver>_*` (`package.sh` / workflow).

### 9. Git and GitHub

- `Fast_Godot.app` is >100 MB: never `git add -A` after packaging locally. `.gitignore` covers
  `Fast_Godot.app/`, `Godot.app/`, `packages/`, `out/`, `bin/`.
- GitHub cannot create a wiki without a first page via the web UI; docs are published as
  **GitHub Pages** from `docs/` (`jekyll-theme-midnight`, `jekyll-optional-front-matter`,
  `jekyll-relative-links`).
- `project.godot` is a config file with multi-line strings — do not edit it with naive regexes;
  a `re.sub` on a multi-line `build_profile_json` corrupted it once already.

### 10. `compile_commands.json` only ever holds the last captured target

`grit_make_manifest.sh` derives `flags.txt` from it. Building the **editor** after the template
leaves editor flags (`-O2 -DDEBUG_ENABLED`) behind, so regenerating a release manifest at that
point silently produced a debug-style `flags.txt` — the generated code then linked a
`-O2 -DDEBUG_ENABLED` object into a release template. The script now refuses when the flags do
not match the requested target and prints the `compiledb=yes` command to run. Recapture first:

```sh
scons platform=macos target=template_release arch=arm64 compiledb=yes -j10 ccflags=-fmodules-cache-path=/tmp/clangmods
```

## What is done vs not

Done and verified:
- VM/Compiled toolbar dropdown, gear button, project-scoped profile storage (persisted in
  `project.godot` as `grit/build_profile_json`), progress dialog, Esc cancel, cancel cleanup.
- Compiled export: codegen → relink → **native artifact** in a single export pass (verified:
  `Grit: 2 native functions available`, ~48 ms vs ~158 ms VM on a 20 M-iteration loop).
  The old auto re-export is gone; the build happens in `_export_begin`, before packing.
- Compiled **debug** export works once `template_debug` + its manifest exist.
- **Profile builds run isolated** (see gotcha #2): verified end to end with a profile set on
  `/tmp/gritproj`. Empty profile: 2461 files / ~2m34s, 986 MB work tree; repeat export 11 s with
  nothing recompiled; after adding a function, 2 files recompiled and `3 native functions
  available`. The editor stayed at 1077 classes throughout.
- **Tree-shaking measured** (profiles set on `/tmp/gritproj`; classes counted from the exported
  app, size of the template binary vs the stock 65,381,488):

  | profile | classes | `mat3d` | template | delta |
  |---|---|---|---|---|
  | stock | 983 | true | 65,381,488 | — |
  | `disabled_classes` ×6 | 977 | true | 65,182,688 | −0.30% |
  | 2D-only build options | 702 | false | 55,120,880 | −15.69% |
  | both combined | 696 | false | 54,905,424 | −16.02% |

  The 2D profile is `disable_3d`, `disable_xr`, `disable_navigation_3d`,
  `disable_physics_3d`; the classes are `Line2D`, `Path2D`, `Polygon2D`,
  `AnimatedSprite2D`, `CPUParticles2D`, `BackBufferCopy`. 1969 files compiled (492 fewer than
  the 2461-file full build), export zip 22.8 MB → 19.1 MB. Native dispatch worked in every
  case, and the two mechanisms compose additively: 702 − 6 = 696 classes, and the size deltas
  add up. Build failed once with the sandbox module-cache trap, see gotcha #2.
- **`disabled_classes` is reflection-only, not a size win.** `disabled_class_builder`
  (`core/core_builders.py:10`) emits only
  `class T; template <> struct is_class_enabled<T> : std::false_type {};`, and
  `ClassDB::register_class` skips the registration via `if constexpr`. The class's code still
  compiles and links, so 6 classes cost a full 2461-file rebuild and saved 199 KB. Verified
  precisely: `classes=977`, `line2d=false poly2d=false`, but `sprite2d=true` (not disabled) and
  `mat3d=true` (no build options). Size comes from `disabled_build_options`; `disabled_classes`
  only trims the scripting API surface.
- CI (`build-release.yml`): 9 jobs, macOS arm64 / Linux / Windows x86_64, editor + debug +
  release templates, Vulkan + AccessKit, per-platform template zips, release on `v*` tags.

Not done:
1. ~~**Profile isolation** (the tree-shaking feature you asked for).~~ Implemented and verified
   with both a 2D-only build-option profile and a `disabled_classes` profile (numbers above).
   Each work tree is ~700 MB–1 GB, keyed by profile+target; delete `out/grit-profiles/` to
   reclaim space (the next export of that profile pays the full build again).
2. **GUI testing of the gear dialog**: it compiles and persists, but nobody has clicked through
   it (Reset / Detect from Project / OK → reopen project → values persisted).
3. `scons.pid` is not truncated at helper start, so cancelling a build within the first few
   hundred ms could signal a stale PID.
4. ~~Compiled export requires `scons_args.txt` to be maintained by hand per machine.~~ Resolved:
   the SDK path is auto-detected now (`tools/grit_scons_args.sh`), and the file only overrides it.
5. Windows/Linux compiled exports are untested (helper is bash; no manifest exists there).

## Environment facts worth keeping

- **VM templates carry no Grit code.** `modules/grit/config.py` builds the module only for the
  editor or when `grit_generated` is set, so a plain `target=template_*` build is stock Godot
  (plus CFFI). The installed `macos.zip` is therefore genuinely Grit-free: a VM export prints no
  `Grit:` lines. Compiled templates are separate builds that do include it.
- A stock `macos.zip` in the export templates dir must contain **both**
  `Contents/MacOS/godot_macos_release.arm64` and `godot_macos_debug.arm64`. The one left in
  `/tmp/fakehome/.../export_templates/4.7.2.stable/` had only the release binary, so every
  **debug** export failed with `Requested template binary "godot_macos_debug.arm64" not found`
  — including plain VM exports, which has nothing to do with Grit. Rebuilt it from
  `misc/dist/macos_template.app` plus both `bin/godot.macos.template_*.arm64` binaries; the old
  one is at `/tmp/macos.zip.bak`.

- clang from Xcode CLT; SDK `-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`.
- `ccflags=-fmodules-cache-path=/tmp/clangmods` is needed inside sandboxes.
- The editor's `--path` is **disabled in templates** (`disable_path_overrides`), so run exported
  apps from their `.app` and use `--quit` for headless smoke tests.
- MoltenVK is linked statically on macOS (`use_volk=no` there); Windows/Linux use `use_volk=yes`.
- **AccessKit is gone.** It needed prebuilt libraries that only exist for MSVC, which forced the
  whole Windows build onto MSVC. `accesskit` now defaults to `False` in `SConstruct` and every
  workflow passes `accesskit=no`; `misc/scripts/install_accesskit.py` is deleted and
  `drivers/accesskit` is not built.
- **Windows builds with MinGW** (`.github/workflows/build-release.yml`): one downloadable
  toolchain, no Visual Studio and no Windows SDK. libffi comes from the MSYS2 `mingw64` prefix
  instead of vcpkg, and the dlmalloc clash is still handled by renaming libffi's `dl*` symbols —
  now with `objcopy --redefine-syms` on `libffi.a` instead of `llvm-ar`/`llvm-objcopy` on a
  `.lib`. This pipeline has **not been run** on a Windows runner yet.
