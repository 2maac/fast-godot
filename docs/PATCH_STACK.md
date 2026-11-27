# Patch stack

This repository stores our Godot changes **as patches**. There is exactly one
active stack:

| stack | base |
|---|---|
| `4.8-beta5` (branch and tag, repository default branch; **pushed**; M2 temporal stack commit `cb0aa639c7` = tag `4.8-beta5`, branch tip is the docs refresh above it; below it the pmndrs milestone-1 stack commit `d8b3d583ae` and first-class spatial-mode commit `91a0a3e8e4`) | godot `master` at `67421f36e5` (`4.8.0-beta`), pinned in `../patches/upstream.lock` |

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
layer list grew by 4 paths, including `renderer_scene_render_rd.h`). Existing
files must be added to a layer list explicitly too: the first-class spatial-mode
change added six existing paths to `0005` (`scene/main/viewport.{h,cpp}`,
`servers/rendering/rendering_server_enums.h`,
`servers/rendering/storage/render_scene_buffers.cpp`,
`doc/classes/{Viewport,RenderingServer}.xml`), and **milestone 2 added the three
new `servers/rendering/renderer_rd/effects/pmndrs_temporal*` files**.

**Layer completeness (important, fixed in M2).** The per-layer round-trip check
only re-diffs the paths *listed* in `layers/*.files`, so it **cannot detect an
omitted file**. Three SDFGI files had been modified in the worktree but were
missing from every layer list (`servers/rendering/renderer_rd/environment/
gi.h`, `…/shaders/environment/gi.glsl`,
`…/shaders/environment/sdfgi_integrate.glsl`), so the previously
committed/tagged stack could not reproduce the SDFGI build even though its
per-layer round-trip looked clean. M2 added them to `0005`; now every
worktree-changed path is covered (**58/58** check against
`git -C /tmp/godot48 status`) and the `0005` layer list has **162 paths**.
Regeneration must therefore cross-check the worktree status paths against the
union of all layer lists, not just run the round-trip.

Verified 2026-11-27 ~21:55 for the first-class pmndrs spatial scaling mode: the
regenerated stack (milestone 1 `d8b3d583ae` + spatial mode `91a0a3e8e4`) applied
cleanly to a fresh checkout of the pinned commit and re-diffed byte-identically
for all 8 layers (`0005` = 5,936,829 B; `/tmp/applytest`, 1,784 staged paths).
Patch deltas: milestone 1 was `0004` `+247/−21`, `0005` `+1288/−32`; the spatial
mode added `0005` `+174/−41` and six paths. Both commits were pushed and tag
`4.8-beta5` moved to `91a0a3e8e4` on 2026-11-27 ~23:45.

**Verified 2026-11-28 ~00:36 for pmndrs milestone 2 (temporal):** a freshly
recreated pristine `67421f36e5` worktree (`/tmp/applytest`) + `tools/apply.sh`
with the full M2 stack, then re-diffing every layer with its `layers/*.files`
list, reproduces **all 8 `patches/*.patch` files byte-identically** (independently
re-checked); both the worktree and the applied tree have **16,070 tracked
files**. Sizes: `0001` 508,662 B, `0002` 203,592, `0003` 45,856, `0004` 898,639,
`0005` 6,085,575, `0006` 15,131,334, `0007` 3,541,178, `0099` 29,700. Patch
deltas vs the previously committed stack: `0004` `+135/−7`, `0005` `+3104/−275`
(driver M2 fixes + temporal effect + mode 7 + the three SDFGI files), layer list
`+6` paths → 162. The M2 patch files are **committed and pushed** as
`cb0aa639c7`, and tag `4.8-beta5` was force-moved from `91a0a3e8e4` to it on
2026-11-28 ~01:00 so fresh toolchain installs bootstrap the mode-7
driver/template (the moved tag archive was re-checked).

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
| `0004-webgpu-driver` | 32 | komm64/4.7 WebGPU driver (MIT) rebased onto 4.8: `buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`, readback/upload conversion, layered copy/initialize, per-stage SPIR-V version; push-constant ring (8 MB), sRGB view siblings, per-frame debug counters and the `rendering/webgpu/*` diagnostic settings. Forward+ additions: compute-pass usage-scope splitting (per-subresource writable+read conflicts), Tint-risk pre-scan/`OpBitcast` rewrite in `spirv_preprocess`, storage-texture format table (`rgb10a2unorm/uint`, integer 8-bit, B10G11R11→RGBA16F promotion + `rg11b10ufloat` WGSL rewrite), WGSL component-driven sample types, physical-format texture views, dimension/sample-type-correct fallback views, `depth32float-stencil8` feature gating, fragment-stage `read_write` storage buffers, unused-colour-target handling, non-negative LOD clamps; per-binding required sampler type map (combined sampler+texture layouts) so NonFiltering MSAA/depth samplers are always satisfied, in both bind-group creation and cross-shader rebinds. **Raw-WGSL compute path** (pmndrs milestone 1): `set_code_from_wgsl_compute()` container factory (fills reflection + per-stage WGSL, mechanically rewrites authored `@binding(N)` → `@binding(2*N)`, checks reflection↔WGSL pairs), and `_shader_build()` recognises the raw-stage marker (WGSL present, SPIR-V empty) and skips the Tint-output WGSL rewrites for it. **M2 driver extensions**: raw-WGSL sampled-texture sample-type auto-layout (`textureLoad`-only f32 → `UnfilterableFloat`, sampler-sampled → `Float`, scoped to raw-WGSL stages so SPIR-V/Tint Forward+ BGLs stay byte-identical) and cached depth-only aspect views for combined depth-stencil textures bound to `Depth` sample-type entries (`D32S8` → `D32Float`, `D24PlusS8` → `D24Plus`); storage buffers/atomics/samplers/multi-group support on the raw path audited and first exercised end-to-end by the temporal effect (see `WGSL_COMPUTE_PATH.md` §12) |
| `0005-engine-renderer-patches` | 162 | engine/renderer/editor patches the driver needs: batching (`batch_instance_index`), shader container format override, driver traits, GDScript native-call hooks, export window, WGSL shader baker plugin, frame profiler, **white-box PCK encryption** (`core_builders.py` table network, `FileAccessEncrypted::open_and_parse_keystream`, pack loader keystream branches), icons/splash. Forward+: HTML5 Fast accepts `forward_plus`; cluster builder renders with plain atomics and a real colour attachment (`USE_ATTACHMENT` variant) when side-effect-only passes are unsupported; scene shader identity subgroup macros + `NO_IMAGE_ATOMICS`; FSR1/FSR2 fp16 variant gating; FSR2 WebGPU paths (B10G11R11→RGBA16F via the driver, no image atomics, no-subgroup SPD, rgba16f output); FSR1 upscales through a storage-capable intermediate; SDFGI enabled on WebGPU (RGBA16F light/probe storage+sample textures, dummy occlusion binding with occlusion disabled, integer voxel-GI default + GI binding pin, colour-framebuffer SDF pipeline with an explicit pass); volumetric-fog dead binding removed; TAA previous-velocity buffer uses RGBA16F on WebGPU (RG16F has no storage format). **pmndrs milestone 1**: raw-WGSL RenderingDevice API (`WGSLUniform`/`WGSLShaderData`, `RenderingDevice::shader_create_from_wgsl()`, shared private `_shader_register()`, default-failing non-pure `RenderingDeviceDriver::shader_create_from_wgsl()`, `SUPPORTS_RAW_WGSL_COMPUTE` feature), the `RendererRD::PmndrsSpatial` spatial upscaler (EASU+RCAS WGSL vendored from pmndrs/upscaler v0.6.0 commit `f6e52098…`, new `effects/pmndrs_upscaler*` files) and the **first-class pmndrs scaling mode** (`Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` / `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL` = 6, appended after `SCALING_3D_MODE_NEAREST` so old values are unchanged, `*_MAX` = 7; `rendering/scaling_3d/mode` hint `pmndrs (Spatial - Fast):6`; enum docs in `doc/classes/{Viewport,RenderingServer}.xml`; Forward+-only guard + spatial fallback semantics in `renderer_viewport.cpp`; mode-6 → PmndrsSpatial selection with FSR1 fallback in `renderer_scene_render_rd.cpp`; the old `rendering/scaling_3d/pmndrs_spatial` bool was removed. **pmndrs milestone 2 (temporal)**: `RendererRD::PmndrsTemporal` (reconstruct/depth-clip/exposure-taps/accumulate/RCAS, 5 vendored WGSL modules from pmndrs/upscaler v0.6.0 commit `f6e52098…`, new `effects/pmndrs_temporal*` files), the per-viewport `PmndrsTemporalContext` seam inside `RenderForwardClustered` (FSR2 slot, pre-tonemap, engine jitter/motion-vector/reversed-Z plumbing), and the first-class temporal mode `Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL` / `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL` = 7 (appended after mode 6 so old values are unchanged, `*_MAX` = 8; classified `VIEWPORT_SCALING_3D_TYPE_TEMPORAL`; hints `pmndrs (Temporal - Slow):7`; enum docs; Forward+-only guard + FSR2→FSR1→off fallback + MSAA-forced-off/TAA conflict handling in `renderer_viewport.cpp`; mode-7 fallback/selection in `render_forward_clustered.cpp`). The `0005` list gained the 3 temporal files plus 3 previously-missing SDFGI paths (`gi.h`, `gi.glsl`, `sdfgi_integrate.glsl`) → 162 paths) |
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
pmndrs spatial upscaler (EASU+RCAS) are browser-verified on the TPS Forward+
export — the first-class `pmndrs Spatial` scaling mode
(`Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` = 6) logs `[PMNDRS] spatial upscaler
running (EASU+RCAS) 640x360 -> 960x540, 120x68 groups` with 0 validation/shader
errors at 59-60 fps, selected live through the TPS settings-screen button
(verified 2026-11-27 21:54); the same build with the default FSR1 selection
shows no `running` line and runs FSR1 unchanged with 0 errors. pmndrs milestone
2 (2026-11-28): the temporal path is browser-verified as the first-class
`pmndrs Temporal` scaling mode (`Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL` = 7)
on the 00:28 TPS export — selected live from the TPS settings button it logs
`[PMNDRS] temporal upscaler running (reconstruct/clip/exposure/accumulate/RCAS)
640x360 -> 960x540, frame 0 (reset)` with 0 validation/shader errors at
59-60 fps; the default FSR1 selection shows no temporal-running line, and the
strong-motion capture series shows no visible ghosting. FSR2 is not usable as a
WebGPU A/B target here (disabled/falls back to FSR1), so the M2 comparison is
vs FSR1 and the no-temporal baseline. See `WEBGPU_HANDOFF.md` (milestone-2
section) and the M2 round-trip/layer-completeness note above.

## Known gaps

- Push-constant ring flush path is mitigated (8 MB ring) but not structurally
  hardened; see `WEBGPU_HANDOFF.md`.
- Forward+ limitations on WebGPU: SDFGI is enabled (occlusion disabled,
  RGBA16F light/probe path). Volumetric fog tints the SDFGI scene differently
  from the native renderer, so the TPS demo defaults it off on WebGPU.
  FSR 1 and the first-class `pmndrs Spatial` (6) and `pmndrs Temporal` (7)
  scaling modes are browser-verified; FSR 2 is disabled/falls back to FSR 1 on
  the current WebGPU builds. See `WEBGPU_HANDOFF.md`.
- pmndrs spatial (milestone 1) is post-tonemap in the spatial-upscaler slot, has
  no `sharpness == 0` blit fallback, is Forward+-only and needs a render scale
  below 1.0 (at 1.0 spatial modes are internally switched OFF).
- pmndrs temporal (milestone 2) quality gaps: no reactive mask
  (transparent/particle ghosting), no shading-change detector (animated-lighting
  ghosting), sky/background ghosting under camera rotation (zero motion), no
  camera-cut reset signal, single-view only (multiview/XR unsupported), and the
  deferred passes (every-texel exposure, moments, debug view, linear-depth mode,
  split-frame, blit fallback). Verification used the TPS third-person camera
  only — no human Subjective ghosting review yet. Memory footprint ~32 B/display-px
  + ~28 B/render-px per viewport. The fixed-camera quality/perf A/B (milestone 3)
  has not been run. See `WEBGPU_HANDOFF.md` and `PMNDRS_TEMPORAL_PORT.md`.
- The stock editor-side CFB writer is reused for encrypted packs; the runtime
  only needs the keystream decoder (see the handoff).
