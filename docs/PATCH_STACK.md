# Patch stack

This repository stores our Godot changes **as patches**. There is exactly one
active stack:

| stack | base |
|---|---|
| `patches/4.8-beta5` | godot `master` at `67421f36e5` (`4.8.0-beta`), pinned in `../upstream.lock` |

The 4.7.2 stack was dropped on 2026-11-26 (branch deleted locally and from
GitHub). The full-source `4.7.2` branch remains on GitHub for history only.

## Apply / regenerate

```sh
tools/apply.sh <godot-source-dir> [4.8-beta5]         # git apply --3way, in order
tools/make-patches.sh <godot-checkout> 4.8-beta5      # regenerate from a modified checkout
```

`apply.sh`/`make-patches.sh` default to `4.8-beta5`. `install.sh <dest>` creates
the Fast_Godot_Toolchain bundle from a staged `out/` directory.

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

Verified: editor builds/boots; web template builds
(`platform=web target=template_release threads=no webgpu=yes opengl3=no`);
encrypted and plain HTML5 Fast exports both boot with 0 errors.

## Known gaps

- Push-constant ring flush path is mitigated (8 MB ring) but not structurally
  hardened; see `../WEBGPU_HANDOFF.md`.
- Forward+ on WebGPU is out of scope for this stack.
- The stock editor-side CFB writer is reused for encrypted packs; the runtime
  only needs the keystream decoder (see the handoff).
