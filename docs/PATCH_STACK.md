# Patch stack

This repository stores our Godot changes **as patches**. There is exactly one
active stack:

| stack | base |
|---|---|
| **`4.8-rc1`** (branch + lightweight tag, created/pushed with the 2026-11-28 rebase; the stack commit is **`76d82e9993`** (with a docs commit above it)). The old `4.8-beta5` tag is kept as history (tag = `a7a5aefbe2`, the threads fix; its branch, tip `5da498f468f`, was deleted 2026-11-28); the repo default branch was switched to `4.8-rc1` on 2026-11-28 | godot `master` at **`7c97c29779`** (`4.8-rc` state: `version.py` status `rc`; 86 commits after `67421f36e5`; upstream has **no** `4.8-rc1` git tag — that is our name), pinned in `../patches/upstream.lock` |

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
accepts linked worktrees (live ones: `/tmp/godot48-rc1`, whose `.git` is a file;
the old `/tmp/godot48` beta tree is history) and runs on macOS's bash 3.2.

**New files must be listed in a layer.** `make-patches.sh` marks every untracked
file from the layer lists intent-to-add before diffing (`git add -N -f`), so a
new file is emitted as a full addition instead of being silently skipped; `-f`
also covers names matched by `.gitignore` (`*.gen.*` etc.). The intent-to-add
entries live only in the source tree's index, not in the working tree. The
pmndrs milestone-1 regeneration used this for the three new
`servers/rendering/renderer_rd/effects/pmndrs_upscaler*` files (the `0005`
layer list grew by 4 paths, including `renderer_scene_render_rd.h`). Existing
files must be added to a layer list explicitly too: the first-class pmndrs
scaling-mode change added six existing paths to `0005`
(`scene/main/viewport.{h,cpp}`,
`servers/rendering/rendering_server_enums.h`,
`servers/rendering/storage/render_scene_buffers.cpp`,
`doc/classes/{Viewport,RenderingServer}.xml`), growing the layer to 156 paths.
The 2026-11-28 rc1 rebase added two new host-shim files to `0004` via this path
(`drivers/webgpu/tint_cli/core/os/os.h`,
`drivers/webgpu/tint_cli/core/string/ustring.h`; `0004` layer 32 → 34 paths).

**Layer completeness (important).** The per-layer round-trip check only re-diffs
the paths *listed* in `layers/*.files`, so it **cannot detect an omitted file**.
Three SDFGI files had been modified in the worktree but were missing from every
layer list (`servers/rendering/renderer_rd/environment/gi.h`,
`…/shaders/environment/gi.glsl`,
`…/shaders/environment/sdfgi_integrate.glsl`), so the previously tagged stack
could not reproduce the SDFGI build even though its round-trip looked clean.
The regeneration added them to `0005` and they are **kept after the temporal
revert** (layer list 156 → 159 paths, `0005` 5,936,829 → 5,940,065 B). The
threads fix then added `servers/rendering/renderer_rd/renderer_compositor_rd.h`
to `0005` (→ **160** paths, `0005` **5,941,474 B**). After the 2026-11-28 rc1
rebase the totals are **1,790 layer paths** (`0004` **34**, `0005` 160) and
**1,790/1,790** worktree-changed paths covered. Regeneration must cross-check
`git -C /tmp/godot48-rc1 status` paths against the union of all layer lists in
addition to the round-trip.

**M2 temporal attempt — reverted.** The pmndrs temporal milestone 2
(`cb0aa639c7`) briefly added mode 7, the `PmndrsTemporal` effect, its WGSL and
M2 raw-WGSL driver extensions to this stack. It was **fully reverted on
2026-11-28** after the owner's browser test showed severe ghosting: the
temporal patches/files are gone, no `PMNDRS_TEMPORAL`/`PmndrsTemporal`/
`pmndrs_temporal` symbol remains in the patches, `0004` is back to 891,250 B and
`SCALING_3D_MODE_MAX` is 7 again. The two research docs are kept as archived
contracts (`PMNDRS_TEMPORAL_PORT.md`, `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` —
IMPLEMENTED THEN REVERTED, NOT SHIPPED).

**Web occlusion culling enabled (2026-11-28).** `0003` now overrides upstream
`platform/web/detect.py` (`module_raycast_enabled: False` → `True`, with an
explanatory comment): upstream disables the `raycast`/Embree module for web
because "Embree is heavy and requires too much memory" (GH-70621, upstream
commit `8c40edf8c0`), so web templates shipped the stub and printed
"Occlusion culling is disabled at build-time". With the override the CPU
Embree culler is compiled in (vendored in-tree under `thirdparty/embree`, 337
tracked files), TPS's `use_occlusion_culling=true` + baked occluders are
honoured, and the warning disappears. Cost: web template wasm 50,471,223 →
51,648,024 B (+1.18 MB, +2.3%) plus Embree's memory; remove the override to
restore upstream's memory-optimized default. Verified 2026-11-28 02:17: 0
"disabled at build-time" lines in a 40 s TPS run, 0 GPU validation errors, 0
shader errors, 59-60 fps, real `RaycastOcclusionCull`/Embree strings in the
wasm. Measured TPS cull yield is zero (the test camera never fully occludes an
instance AABB; instrumentation removed) — see `WEBGPU_HANDOFF.md`
web-platform section. `threads=yes` is now **fixed and verified** (see below;
opt-in because it needs COOP/COEP hosting); the default preset stays
`variant/thread_support=false`.

Verified 2026-11-27 ~21:55 for the first-class pmndrs spatial scaling mode: the
regenerated stack (milestone 1 `d8b3d583ae` + spatial mode `91a0a3e8e4`) applied
cleanly to a fresh checkout of the pinned commit and re-diffed byte-identically
for all 8 layers (`0005` = 5,936,829 B; `/tmp/applytest`, 1,784 staged paths).
Patch deltas from that era: milestone 1 was `0004` `+247/−21`, `0005`
`+1288/−32`; the spatial mode added `0005` `+174/−41` and six paths.

**Verified 2026-11-28 ~00:52 for the temporal revert:** applying the full
regenerated **spatial-only** stack to a pristine `67421f36e5` checkout
(`/tmp/applytest`, all 1,787 layer paths staged) and re-diffing every layer with
its `layers/*.files` list reproduces **all 8 `patches/*.patch` files
byte-identically** (independently re-checked). Sizes: `0001` 508,662 B, `0002`
203,592, `0003` 45,856, `0004` **891,250** (back to the pre-M2 size), `0005`
**5,940,065** (spatial-only + the three SDFGI completeness files), `0006`
15,131,334, `0007` 3,541,178, `0099` 29,700; layer list 159 paths. The revert
was committed and pushed as the revert stack commit with a docs refresh above
it, and tag `4.8-beta5` was force-moved from `cb0aa639c7` to it so fresh
toolchain installs bootstrap the spatial-only driver/template. (History: that
revert stack's `0003` was 45,856 B; the web occlusion-culling change below grew
it to 46,870 B.)

**Verified 2026-11-28 ~02:16 for the web occlusion-culling update:** only
`patches/0003` changed (`+22/−5` in `platform/web/detect.py`, 46,870 B).
Applying the full stack to a pristine `67421f36e5` checkout (`/tmp/applytest`,
all 1,787 layer paths staged) and re-diffing every layer with its
`layers/*.files` list reproduces **all 8 `patches/*.patch` files
byte-identically** (independently re-checked). The editor is unchanged
(`detect.py` is web-build config only). That change became stack commit
`96a71a19bf`.

**Verified 2026-11-28 ~09:58 for the fixed HTML5 threads support:** only
`patches/0003` and `patches/0005` changed. `0003` is **46,060 B** (`detect.py`:
restores upstream's `-pthread` + `PTHREAD_POOL_SIZE=Module['emscriptenPoolSize']||8`
+ `MAXIMUM_MEMORY=2048MB` instead of the legacy `-sUSE_PTHREADS`/pool=0 scheme;
+12/−22 vs the occlusion-only commit). `0005` is **5,941,474 B** (+26:
`renderer_compositor_rd.h` `can_create_resources_async()` now returns false
under `WEBGPU_ENABLED`, so threaded resource creation is queued on the
render/main thread instead of calling emdawnwebgpu from a pthread). The `0005`
layer list gained `servers/rendering/renderer_rd/renderer_compositor_rd.h`
(159 → 160 paths, 1,788 total layer paths). Applying the full stack to a
pristine `67421f36e5` checkout (`/tmp/applytest`, all 1,788 layer paths staged)
and re-diffing every layer reproduces **all 8 `patches/*.patch` files
byte-identically** (independently re-checked), and coverage is **57/57**. The
evidence build: threaded TPS export boots, loads and plays at 60 fps VSync with
0 GPU/shader/worker errors (uncapped 137.3 vs 124 fps single-threaded). Stack
commit **`a7a5aefbe2`**; a docs commit follows, the branch is pushed and tag
`4.8-beta5` force-moved from `96a71a19bf` to `a7a5aefbe2`.

**Rebased to Godot 4.8-rc1 (2026-11-28).** The stack now applies to upstream
`godotengine/godot` master **`7c97c29779`** ("Bump version to 4.8-rc";
`version.py` major 4 / minor 8 / patch 0 / status `rc`), **86 commits ahead** of
the previous 4.8.0-beta pin `67421f36e5`. Upstream has **no git tag `4.8-rc1`**
(its tags stop at `4.7.2-stable`), so `4.8-rc1` is our branch/tag name for this
master rc-state commit; `patches/upstream.lock` records
`godot_tag = "master (4.8.0-rc1)"` + `godot_commit = 7c97c29779…`. The old stack
was applied to a fresh worktree at `7c97c29779` with **`git apply --3way`**: all
files applied cleanly except **one conflict** in
`servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl`
— rc1 made `prev_data.inv_view_precision` / `data.inv_view_precision`
precision-aware vec4 accesses and our patch's context expected the old form.
Resolution: keep our `batch_instance_index` rename and take rc1's new `.xyz` on
those two accesses.

**Latent fresh-build bug fixed (host tool, patch `0004`).**
`drivers/webgpu/spirv_preprocess.cpp` includes `core/os/os.h` and
`core/string/ustring.h`, and `spirv_preprocess.h` uses `String`, but the
standalone shim tree the `tint_convert_cli` build uses
(`drivers/webgpu/tint_cli/`) only had `core/templates/{vector,hash_map,hash_set}.h`.
Old trees only worked because `bin/tint_convert_cli` was prebuilt; a fresh
worktree could not build the build-time host tool. Fix: added minimal
`drivers/webgpu/tint_cli/core/os/os.h` + `core/string/ustring.h` shims (a small
`String` over `std::string` + `itos`) and made the shim
`core/templates/vector.h` include `core/string/ustring.h`. Both shims are in
the `0004` layer list (32 → 34 paths); `0004` grew 891,250 → **893,686 B**.

**Provisioner/tag (patch `0001`).** `modules/grit/editor/grit_provisioner.cpp`
now uses `PATCH_STACK_REF = "4.8-rc1"` and
`PATCH_STACK_ARCHIVE_URL = …/archive/refs/tags/4.8-rc1.tar.gz` (was
`4.8-beta5`), so fresh toolchain boots fetch the `4.8-rc1` stack tag. That tag
must exist on stack commit `76d82e9993` (created/pushed with this change); the
old `4.8-beta5` tag stays as history (its branch was deleted 2026-11-28). The
repo default branch was switched from `4.8-beta5` to `4.8-rc1` on 2026-11-28.

**Verified 2026-11-28 ~12:31 for the 4.8-rc1 rebase:** the regenerated stack
(`0001` 508,734 B, `0002` 203,698, `0003` 46,094, `0004` **893,686**, `0005`
**5,941,786**, `0006` 15,133,768, `0007` 3,541,650, `0099` 29,738 B; 1,790
layer paths) applied to a pristine `7c97c29779` checkout
(`/tmp/applytest-rc1`, all 1,790 layer paths staged) and re-diffed per layer
reproduces **all 8 `patches/*.patch` files byte-identically** (independently
re-checked), with **1,790/1,790** changed worktree paths covered. Editor
`4.8.rc.custom_build.7c97c2977` (sha `19682014…`); nothreads template zip
`c61caa74…` (wasm `f28e7f77…`, site 12:36:00); threaded template zip
`f80d59e6…` (TPS export wasm 51,133,278 B). Runtime: default rc1 TPS Forward+
WebGPU 59-60 fps VSync, pmndrs spatial active, no occlusion warning, 0 GPU
validation/shader errors; threaded rc1 likewise (0 GPU/shader/worker errors),
uncapped threaded 131.3 fps.

Layout change 2026-11-26: the stack moved from `patches/4.8-beta5/` to the
self-contained `patches/` bundle (patch files + `layers/` + `upstream.lock`,
no version subfolder). The editor's toolchain bootstrap reads `patches/*.patch`
and `patches/upstream.lock` from the stack archive; editor builds that predate
the layout change look for `patches/4.8-beta5/` and cannot bootstrap from the
current tag, so the editor and the layout are updated together.

## `4.8-rc1` patches

| patch | files | contents |
|---|---:|---|
| `0001-modules-grit` | 39 | Fast/Grit AOT export compiler, runtime and editor plugins (adapted to 4.8 GDScript internals: `GDType` member maps, const `MethodBind` codegen, parser `resolved_type`/`type_constraint`). **4.8-rc1 rebase (2026-11-28)**: the provisioner uses `PATCH_STACK_REF = "4.8-rc1"` and the archive URL `…/archive/refs/tags/4.8-rc1.tar.gz` (was `4.8-beta5`), so fresh toolchain boots fetch the rc1 stack tag |
| `0002-modules-cffi` | 53 | CFFI module |
| `0003-web-html5-fast-export` | 17 | "HTML5 Fast" export platform + web platform hooks (detect.py, js/engine, display server, export options). **Web occlusion culling enabled (2026-11-28)**: overrides upstream's `detect.py` `module_raycast_enabled: False` to `True` so the `raycast`/Embree CPU culler is compiled into web builds and `rendering/occlusion_culling/use_occlusion_culling` is honoured (upstream disables it for memory reasons, GH-70621 / commit `8c40edf8c0`; cost ≈+1.18 MB wasm + Embree memory; remove the override to return to the upstream stub + build-time warning). **HTML5 threads fix (2026-11-28)**: `env["threads"]` restored to upstream's config (`-pthread` in CCFLAGS/LINKFLAGS, `-sPTHREAD_POOL_SIZE=\"Module['emscriptenPoolSize']||8\"`, `-sMAXIMUM_MEMORY=2048MB`; the legacy `-sUSE_PTHREADS`/`-sPTHREAD_POOL_SIZE=0` scheme had wedged threaded startup) |
| `0004-webgpu-driver` | 34 | komm64/4.7 WebGPU driver (MIT) rebased onto 4.8: `buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`, readback/upload conversion, layered copy/initialize, per-stage SPIR-V version; push-constant ring (8 MB), sRGB view siblings, per-frame debug counters and the `rendering/webgpu/*` diagnostic settings. Forward+ additions: compute-pass usage-scope splitting (per-subresource writable+read conflicts), Tint-risk pre-scan/`OpBitcast` rewrite in `spirv_preprocess`, storage-texture format table (`rgb10a2unorm/uint`, integer 8-bit, B10G11R11→RGBA16F promotion + `rg11b10ufloat` WGSL rewrite), WGSL component-driven sample types, physical-format texture views, dimension/sample-type-correct fallback views, `depth32float-stencil8` feature gating, fragment-stage `read_write` storage buffers, unused-colour-target handling, non-negative LOD clamps; per-binding required sampler type map (combined sampler+texture layouts) so NonFiltering MSAA/depth samplers are always satisfied, in both bind-group creation and cross-shader rebinds. **Raw-WGSL compute path** (pmndrs milestone 1): `set_code_from_wgsl_compute()` container factory (fills reflection + per-stage WGSL, mechanically rewrites authored `@binding(N)` → `@binding(2*N)`, checks reflection↔WGSL pairs), and `_shader_build()` recognises the raw-stage marker (WGSL present, SPIR-V empty) and skips the Tint-output WGSL rewrites for it. **4.8-rc1 host-build fix (2026-11-28)**: added standalone shims `drivers/webgpu/tint_cli/core/os/os.h` + `drivers/webgpu/tint_cli/core/string/ustring.h` (the shim `core/templates/vector.h` now includes the ustring shim) so a **fresh** worktree can build the build-time `tint_convert_cli` host tool (`spirv_preprocess.cpp`/`.h` need `os.h`, `ustring.h` and `String`); `0004` grew 891,250 → 893,686 B |
| `0005-engine-renderer-patches` | 160 | engine/renderer/editor patches the driver needs: batching (`batch_instance_index`), shader container format override, driver traits, GDScript native-call hooks, export window, WGSL shader baker plugin, frame profiler, **white-box PCK encryption** (`core_builders.py` table network, `FileAccessEncrypted::open_and_parse_keystream`, pack loader keystream branches), icons/splash. Forward+: HTML5 Fast accepts `forward_plus`; cluster builder renders with plain atomics and a real colour attachment (`USE_ATTACHMENT` variant) when side-effect-only passes are unsupported; scene shader identity subgroup macros + `NO_IMAGE_ATOMICS`; FSR1/FSR2 fp16 variant gating; FSR2 WebGPU paths (B10G11R11→RGBA16F via the driver, no image atomics, no-subgroup SPD, rgba16f output); FSR1 upscales through a storage-capable intermediate; SDFGI enabled on WebGPU (RGBA16F light/probe storage+sample textures, dummy occlusion binding with occlusion disabled, integer voxel-GI default + GI binding pin, colour-framebuffer SDF pipeline with an explicit pass); volumetric-fog dead binding removed; TAA previous-velocity buffer uses RGBA16F on WebGPU (RG16F has no storage format). **pmndrs milestone 1**: raw-WGSL RenderingDevice API (`WGSLUniform`/`WGSLShaderData`, `RenderingDevice::shader_create_from_wgsl()`, shared private `_shader_register()`, default-failing non-pure `RenderingDeviceDriver::shader_create_from_wgsl()`, `SUPPORTS_RAW_WGSL_COMPUTE` feature), the `RendererRD::PmndrsSpatial` spatial upscaler (EASU+RCAS WGSL vendored from pmndrs/upscaler v0.6.0 commit `f6e52098…`, new `effects/pmndrs_upscaler*` files) and the **first-class pmndrs scaling mode** (`Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` / `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL` = 6, appended after `SCALING_3D_MODE_NEAREST` so old values are unchanged, `*_MAX` = 7; `rendering/scaling_3d/mode` hint `pmndrs (Spatial - Fast):6`; enum docs in `doc/classes/{Viewport,RenderingServer}.xml`; Forward+-only guard + spatial fallback semantics in `renderer_viewport.cpp`; mode-6 → PmndrsSpatial selection with FSR1 fallback in `renderer_scene_render_rd.cpp`; the old `rendering/scaling_3d/pmndrs_spatial` bool was removed. **HTML5 threads fix (2026-11-28)**: `renderer_compositor_rd.h` `can_create_resources_async()` returns false under `WEBGPU_ENABLED`, so texture/buffer creation during threaded scene loading is queued to the render/main thread instead of running emdawnwebgpu on a pthread (which failed with "Cannot read properties of undefined (reading 'createTexture')"). Layer list is 160 paths: the 156 above + the three SDFGI completeness files `environment/gi.h`, `shaders/environment/gi.glsl`, `shaders/environment/sdfgi_integrate.glsl` + `renderer_compositor_rd.h` for the threads fix; the M2 temporal entries were removed by the revert) |
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
  `patches/upstream.lock`'s commit plus the `4.8-rc1` stack archive and applies
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
shows no `running` line and runs FSR1 unchanged with 0 errors. The 2026-11-28
temporal revert rebuilt the editor/template/site from the reverted tree and
re-exported the TPS demo spatial-only (111 s export → wasm 50,471,223 B sha
`98ac9e82…`); the served wasm contains the spatial `[PMNDRS] spatial upscaler
running` string and **no** temporal-upscaler/`pmndrs_temporal` strings, and the
TPS settings screen has only the "pmndrs Spatial" button (a stale saved
`scale_filter >= SCALING_3D_MODE_MAX` is sanitized back to FSR 1).
Web platform (2026-11-28): occlusion culling is compiled into the web template
(commit `96a71a19bf`) — the final TPS runs have 0 "Occlusion culling is
disabled at build-time" lines, 0 GPU validation errors, 0 shader errors, 59-60
fps and real `RaycastOcclusionCull`/Embree strings in the wasm — but the
measured TPS cull yield is zero. HTML5 `threads=yes` is **fixed and
browser-verified** (commit `a7a5aefbe2`): the threaded TPS export boots, loads
and plays at 60 fps VSync with 0 GPU/shader/worker errors (uncapped 137.3 vs
124 fps single-threaded), via upstream pthread flags restored in `detect.py`
plus `can_create_resources_async()=false` under `WEBGPU_ENABLED`. It stays
**opt-in** (needs COOP/COEP hosting for `crossOriginIsolated`), so the shipped
preset keeps `variant/thread_support=false`. Details in `WEBGPU_HANDOFF.md`.

4.8-rc1 rebase (2026-11-28): the stack applies to upstream master `7c97c29779`
(4.8-rc state; no upstream tag — our name), editor `4.8.rc.custom_build.7c97c2977`.
The rc1 editor/templates were built and the TPS demo re-exported: default rc1
nothreads export 59-60 fps VSync with pmndrs spatial active, no occlusion
warning, 0 GPU/shader errors; threaded rc1 export likewise (0 GPU/shader/worker
errors) and 131.3 fps uncapped. The one shader conflict
(`scene_forward_mobile.glsl`) was resolved by keeping `batch_instance_index`
and taking rc1's `.xyz` precision-aware accesses; the `0004` host shims fix a
latent fresh `tint_convert_cli` build failure. Round-trip on a pristine
`7c97c29779` reproduces all 8 patches byte-identically. See
`WEBGPU_HANDOFF.md`.

## Known gaps

- Push-constant ring flush path is mitigated (8 MB ring) but not structurally
  hardened; see `WEBGPU_HANDOFF.md`.
- Forward+ limitations on WebGPU: SDFGI is enabled (occlusion disabled,
  RGBA16F light/probe path). Volumetric fog tints the SDFGI scene differently
  from the native renderer, so the TPS demo defaults it off on WebGPU.
  FSR 1/FSR 2 and the first-class `pmndrs Spatial` scaling mode are
  browser-verified.
  See `WEBGPU_HANDOFF.md`.
- pmndrs spatial (milestone 1, shipped) is post-tonemap in the spatial-upscaler
  slot, has no `sharpness == 0` blit fallback, is Forward+-only and needs a
  render scale below 1.0 (at 1.0 spatial modes are internally switched OFF); the
  fixed-camera perf A/B has not been run. See `WEBGPU_HANDOFF.md` and
  `PMNDRS_UPSCALER_SPATIAL.md`.
- pmndrs temporal (milestone 2) was **implemented, briefly shipped, then fully
  reverted on 2026-11-28** — severe ghosting in the owner's browser test. The
  retry (milestone 2b) must add a reactive mask, a shading-change pass,
  camera-cut reset, deterministic jitter/velocity validation and a ground-truth
  A/B harness (FSR 2 is not usable as a WebGPU temporal reference), plus human
  review. The archived contract is in `PMNDRS_TEMPORAL_PORT.md` and
  `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` (IMPLEMENTED THEN REVERTED, NOT
  SHIPPED).
- Web occlusion culling is **enabled** (patch `0003`) at a cost of ≈1.18 MB
  extra wasm plus Embree's memory (upstream disabled it for memory reasons,
  GH-70621). It is verified working but the measured TPS cull yield is **zero**
  (the test camera never fully occludes an instance AABB) — benchmark a scene
  with genuinely occluded geometry before relying on it, and remove the
  `detect.py` override to go back to the upstream stub. See `WEBGPU_HANDOFF.md`.
- Web `threads=yes` is **fixed and browser-verified but opt-in** (2026-11-28):
  threaded templates boot, load and play (60 fps VSync, 0 errors; 137.3 vs 124
  fps uncapped at 960×540). They require COOP `same-origin` + COEP
  `require-corp` hosting (`crossOriginIsolated`) for SharedArrayBuffer, which a
  plain static host does not provide — so the shipped preset keeps
  `variant/thread_support=false`. Threaded gains only matter above 60 Hz or in
  CPU-heavy scenes. See `WEBGPU_HANDOFF.md`.
- **4.8-rc1 is an upstream master rc-state commit, not a tagged release.**
  Upstream has no `4.8-rc1` git tag (its tags stop at `4.7.2-stable`), so
  `4.8-rc1` here is our branch/tag name for `7c97c29779` ("Bump version to
  4.8-rc"). When upstream cuts a real tag, re-pin `patches/upstream.lock` and
  regenerate. The repo default branch is `4.8-rc1` (switched 2026-11-28).
  `BUILD.txt` was refreshed
  for rc1 (12:40:30); fresh bootstrap was verified with `GRIT_ENGINE_PROBE=1`
  on an isolated HOME ("prepared engine source from Godot 7c97c297793c +
  4.8-rc1 patches", "engine probe ok").
- The stock editor-side CFB writer is reused for encrypted packs; the runtime
  only needs the keystream decoder (see the handoff).
