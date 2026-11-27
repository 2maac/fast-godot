# Patch stack

This repository stores our Godot changes **as patches**. There is exactly one
active stack:

| stack | base |
|---|---|
| `4.8-beta5` (branch and tag, repository default branch; pmndrs milestone-1 stack commit `d8b3d583ae` + docs refreshes on the branch, tag `529407b9fc` — the tag has not been moved to the pmndrs stack yet) | godot `master` at `67421f36e5` (`4.8.0-beta`), pinned in `../patches/upstream.lock` |

The 4.7.2 stack was dropped on 2026-11-26 (branch deleted locally and from
GitHub). The full-source `4.7.2` branch remains on GitHub for history only.

## Apply / regenerate

```sh
tools/apply.sh <godot-source-dir>          # apply every patches/*.patch, in order
tools/make-patches.sh <godot-checkout>     # regenerate patches/*.patch
```

`patches/` is a **self-contained portable bundle**: the ordered `.patch` files,
the `layers/*.files` path lists that drive regeneration and `upstream.lock` (the
Godot pin) all live in it. Copy `patches/` plus `tools/apply.sh` to another
machine and you can reproduce the stack without the rest of this repo. There is
no `install.sh` in this repo: the Fast_Godot_Toolchain bundle is created by the provisioner that
lives in the patch stack (`tools/toolchain/download_toolchain.sh` +
`tools/toolchain/make_bundle_zip.py`), run from a patched source tree.

Regenerating binary layers: the committed patches contain 23 `GIT binary patch`
hunks (8 in `0005`, 15 in `0007`); `make-patches.sh` passes `--binary` to both
`git diff` calls (fixed 2026-11-26), so regeneration keeps them. The script also
accepts linked worktrees (e.g. `/tmp/godot48`, whose `.git` is a file) and runs
on macOS's bash 3.2.

**New files must be listed in a layer.** `make-patches.sh` marks every untracked
file from the layer lists intent-to-add before diffing (`git add -N -f`), so a
new file is emitted as a full addition instead of being silently skipped; `-f`
also covers names matched by `.gitignore` (`*.gen.*` etc.). The intent-to-add
entries live only in the source tree's index, not in the working tree. The
pmndrs milestone-1 regeneration used this for the three new
`servers/rendering/renderer_rd/effects/pmndrs_upscaler*` files (the `0005`
layer list grew by 4 paths, including `renderer_scene_render_rd.h`).

Verified 2026-11-27 after pmndrs milestone 1: the regenerated stack applies
cleanly to a fresh checkout of the pinned commit (`git worktree add --detach
67421f36e5` + `tools/apply.sh`), and re-diffing every layer with its
`layers/*.files` list reproduces **all 8 `patches/*.patch` files
byte-identically** (`/tmp/applytest` holds the applied tree). Patch line deltas
for the pmndrs work: `0004` `+247/−21`, `0005` `+1288/−32`. The regenerated
patch files are committed on the (local, not yet pushed) branch as
`d8b3d583ae` with docs refreshes above it; tag `529407b9fc` still points at the
SDFGI/FSR stack and must be moved — and the branch pushed — to ship the pmndrs
milestone-1 driver/template to fresh toolchain installs.

Layout change 2026-11-26: the stack moved from `patches/4.8-beta5/` to the
self-contained `patches/` bundle (patch files + `layers/` + `upstream.lock`,
no version subfolder). The editor's toolchain bootstrap reads `patches/*.patch`
and `patches/upstream.lock` from the stack archive; editor builds that predate
the layout change look for `patches/4.8-beta5/` and cannot bootstrap from the
current tag, so the editor and the layout are updated together.

## `4.8-beta5` patches

| patch | files | contents |
|---|---:|---|
| `0001-modules-grit` | 39 | Fast/Grit AOT export compiler, runtime and editor plugins (adapted to 4.8 GDScript internals: `GDType` member maps, const `MethodBind` codegen, parser `resolved_type`/`type_constraint`) |
| `0002-modules-cffi` | 53 | CFFI module |
| `0003-web-html5-fast-export` | 17 | "HTML5 Fast" export platform + web platform hooks (detect.py, js/engine, display server, export options) |
| `0004-webgpu-driver` | 32 | komm64/4.7 WebGPU driver (MIT) rebased onto 4.8: `buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`, readback/upload conversion, layered copy/initialize, per-stage SPIR-V version; push-constant ring (8 MB), sRGB view siblings, per-frame debug counters and the `rendering/webgpu/*` diagnostic settings. Forward+ additions: compute-pass usage-scope splitting (per-subresource writable+read conflicts), Tint-risk pre-scan/`OpBitcast` rewrite in `spirv_preprocess`, storage-texture format table (`rgb10a2unorm/uint`, integer 8-bit, B10G11R11→RGBA16F promotion + `rg11b10ufloat` WGSL rewrite), WGSL component-driven sample types, physical-format texture views, dimension/sample-type-correct fallback views, `depth32float-stencil8` feature gating, fragment-stage `read_write` storage buffers, unused-colour-target handling, non-negative LOD clamps; per-binding required sampler type map (combined sampler+texture layouts) so NonFiltering MSAA/depth samplers are always satisfied, in both bind-group creation and cross-shader rebinds. **Raw-WGSL compute path** (pmndrs milestone 1): `set_code_from_wgsl_compute()` container factory (fills reflection + per-stage WGSL, mechanically rewrites authored `@binding(N)` → `@binding(2*N)`, checks reflection↔WGSL pairs), and `_shader_build()` recognises the raw-stage marker (WGSL present, SPIR-V empty) and skips the Tint-output WGSL rewrites for it |
| `0005-engine-renderer-patches` | 150 | engine/renderer/editor patches the driver needs: batching (`batch_instance_index`), shader container format override, driver traits, GDScript native-call hooks, export window, WGSL shader baker plugin, frame profiler, **white-box PCK encryption** (`core_builders.py` table network, `FileAccessEncrypted::open_and_parse_keystream`, pack loader keystream branches), icons/splash. Forward+: HTML5 Fast accepts `forward_plus`; cluster builder renders with plain atomics and a real colour attachment (`USE_ATTACHMENT` variant) when side-effect-only passes are unsupported; scene shader identity subgroup macros + `NO_IMAGE_ATOMICS`; FSR1/FSR2 fp16 variant gating; FSR2 WebGPU paths (B10G11R11→RGBA16F via the driver, no image atomics, no-subgroup SPD, rgba16f output); FSR1 upscales through a storage-capable intermediate; SDFGI enabled on WebGPU (RGBA16F light/probe storage+sample textures, dummy occlusion binding with occlusion disabled, integer voxel-GI default + GI binding pin, colour-framebuffer SDF pipeline with an explicit pass); volumetric-fog dead binding removed; TAA previous-velocity buffer uses RGBA16F on WebGPU (RG16F has no storage format). **pmndrs milestone 1**: raw-WGSL RenderingDevice API (`WGSLUniform`/`WGSLShaderData`, `RenderingDevice::shader_create_from_wgsl()`, shared private `_shader_register()`, default-failing non-pure `RenderingDeviceDriver::shader_create_from_wgsl()`, `SUPPORTS_RAW_WGSL_COMPUTE` feature), the `RendererRD::PmndrsSpatial` spatial upscaler (EASU+RCAS WGSL vendored from pmndrs/upscaler v0.6.0 commit `f6e52098…`, new `effects/pmndrs_upscaler*` files) and the opt-in `rendering/scaling_3d/pmndrs_spatial` setting/selection in the FSR1 slot |
| `0006-thirdparty-vendored` | 1217 | vendored Tint + SPIRV-Tools for the export-time SPIR-V→WGSL bake |
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
- First-use toolchain bootstrap: downloads Godot's source at
  `patches/upstream.lock`'s commit plus the `4.8-beta5` stack archive and applies
  it with the system `git apply`; needs git on the machine
  (`GRIT_ENGINE_PROBE=1` tests just that step).

Verified: editor builds/boots; web template builds
(`platform=web target=template_release threads=no webgpu=yes opengl3=no`);
encrypted and plain HTML5 Fast exports both boot with 0 errors. Forward+
(2026-11-27): TPS demo exported through HTML5 Fast renders the full level with
0 WebGPU validation errors, 0 SPIR-V→WGSL failures and 60 fps in
Chrome/Dawn. pmndrs milestone 1 (2026-11-27): the raw-WGSL compute path and the
pmndrs spatial upscaler (EASU+RCAS, opt-in
`rendering/scaling_3d/pmndrs_spatial`) are browser-verified on the TPS Forward+
export — setting on logs `[PMNDRS] spatial upscaler running (EASU+RCAS)
640x360 -> 960x540, 120x68 groups` with 0 validation/shader errors at 59-60 fps;
the setting-off export runs FSR1 unchanged with 0 errors.

## Known gaps

- Push-constant ring flush path is mitigated (8 MB ring) but not structurally
  hardened; see `WEBGPU_HANDOFF.md`.
- Forward+ limitations on WebGPU: SDFGI is enabled (occlusion disabled,
  RGBA16F light/probe path). Volumetric fog tints the SDFGI scene differently
  from the native renderer, so the TPS demo defaults it off on WebGPU.
  FSR 1/FSR 2 and the opt-in pmndrs spatial upscaler are browser-verified.
  See `WEBGPU_HANDOFF.md`.
- pmndrs upscaler milestone 1 is **spatial only**: no temporal history/jitter,
  post-tonemap (FSR1-slot) placement, no `sharpness == 0` blit fallback, and the
  fixed-camera perf A/B against FSR1/FSR2 has not been run. The temporal path is
  milestone 2. See `WEBGPU_HANDOFF.md` and `PMNDRS_UPSCALER_SPATIAL.md`.
- The stock editor-side CFB writer is reused for encrypted packs; the runtime
  only needs the keystream decoder (see the handoff).
