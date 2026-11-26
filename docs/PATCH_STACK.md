# Patch stack

This repository stores our Godot changes **as patches**. There is exactly one
active stack:

| stack | base |
|---|---|
| `4.8-beta5` (branch and tag, repository default branch; patch files in `patches/`) | godot `master` at `67421f36e5` (`4.8.0-beta`), pinned in `../upstream.lock` |

The 4.7.2 stack was dropped on 2026-11-26 (branch deleted locally and from
GitHub). The full-source `4.7.2` branch remains on GitHub for history only.

## Apply / regenerate

```sh
tools/apply.sh <godot-source-dir>          # apply every patches/*.patch, in order
tools/make-patches.sh <godot-checkout>     # regenerate patches/*.patch
```

The patch series lives in `patches/` (single stack, no version subfolder); the
`layers/*.files` path lists that drive regeneration are at the repository root.
There is no `install.sh` in this repo: the Fast_Godot_Toolchain bundle is created by the provisioner that
lives in the patch stack (`tools/toolchain/download_toolchain.sh` +
`tools/toolchain/make_bundle_zip.py`), run from a patched source tree.

Regenerating binary layers: the committed patches contain 23 `GIT binary patch`
hunks (8 in `0005`, 15 in `0007`); `make-patches.sh` passes `--binary` to both
`git diff` calls (fixed 2026-11-26), so regeneration keeps them. The script also
accepts linked worktrees (e.g. `/tmp/godot48`, whose `.git` is a file) and runs
on macOS's bash 3.2. Verified 2026-11-26: regenerating all 8 layers from
`/tmp/godot48` is byte-identical to the committed patches.

Layout change 2026-11-26: the stack moved from `patches/4.8-beta5/` to
`patches/` (single stack, no version subfolder; `layers/` stays at the repo
root). The editor's toolchain bootstrap reads `patches/*.patch` from the stack
archive; editor builds that predate the layout change look for
`patches/4.8-beta5/` and cannot bootstrap from the current tag, so the editor
and the layout are updated together.

## `4.8-beta5` patches

| patch | files | contents |
|---|---:|---|
| `0001-modules-grit` | 39 | Fast/Grit AOT export compiler, runtime and editor plugins (adapted to 4.8 GDScript internals: `GDType` member maps, const `MethodBind` codegen, parser `resolved_type`/`type_constraint`) |
| `0002-modules-cffi` | 53 | CFFI module |
| `0003-web-html5-fast-export` | 17 | "HTML5 Fast" export platform + web platform hooks (detect.py, js/engine, display server, export options) |
| `0004-webgpu-driver` | 32 | komm64/4.7 WebGPU driver (MIT) rebased onto 4.8: `buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`, readback/upload conversion, layered copy/initialize, per-stage SPIR-V version; push-constant ring (8 MB), sRGB view siblings, per-frame debug counters and the `rendering/webgpu/*` diagnostic settings |
| `0005-engine-renderer-patches` | 129 | engine/renderer/editor patches the driver needs: batching (`batch_instance_index`), shader container format override, driver traits, GDScript native-call hooks, export window, WGSL shader baker plugin, frame profiler, **white-box PCK encryption** (`core_builders.py` table network, `FileAccessEncrypted::open_and_parse_keystream`, pack loader keystream branches), icons/splash |
| `0006-thirdparty-vendored` | 1214 | vendored Tint + SPIRV-Tools for the export-time SPIR-V→WGSL bake |
| `0007-tools-fixtures-docs` | 251 | tools/ (grit_build.py, toolchain provisioning), test_project/, docs, webgpu notes/tests, SPIR-V fixtures |
| `0099-misc` | 19 | remaining touched files |

Binary assets are included (`git diff --binary`): Fast Godot app icon
(`GodotLG.icns`/`Resource.icns`), editor splash/app_icon, `misc/logo` PNGs,
Windows `.ico`, docs images and the SPIR-V shader-corpus fixtures.

## Build requirements

- **Emscripten 6.0.12** for web (4.8 requires >= 6.0.1; this is the single SDK
  pinned by `tools/toolchain/download_toolchain.sh`).
- Python 3.12 (+ `zstandard` for SCons 4.x), SCons 4.11.1 — all provisioned by
  the same toolchain script.
- macOS editor: `vulkan_sdk_path` pointing at the toolchain's MoltenVK.
- First-use toolchain bootstrap: downloads Godot's source at `upstream.lock`'s
  commit plus the `4.8-beta5` stack archive and applies it with the system
  `git apply`; needs git on the machine (`GRIT_ENGINE_PROBE=1` tests just that
  step).

Verified: editor builds/boots; web template builds
(`platform=web target=template_release threads=no webgpu=yes opengl3=no`);
encrypted and plain HTML5 Fast exports both boot with 0 errors.

## Known gaps

- Push-constant ring flush path is mitigated (8 MB ring) but not structurally
  hardened; see `WEBGPU_HANDOFF.md`.
- Forward+ on WebGPU is out of scope for this stack.
- The stock editor-side CFB writer is reused for encrypted packs; the runtime
  only needs the keystream decoder (see the handoff).
