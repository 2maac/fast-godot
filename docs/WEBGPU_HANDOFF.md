# Fast Godot WebGPU handoff — 4.8-beta5 port

Resume doc. Last updated 2026-11-28 ~01:00. **Forward+ and Forward Mobile both
run on the WebGPU device, SDFGI is enabled on the WebGPU device, and
pmndrs/upscaler milestone 1 (spatial EASU+RCAS) is implemented and
browser-verified as a first-class Viewport scaling mode (mode 6).** The
pmndrs/upscaler **milestone 2 (temporal path) was attempted and then fully
reverted** (2026-11-28): the owner's browser test showed severe ghosting, the
temporal code was removed (not just the UI), and the stack is back to the
spatial-only mode-6 state plus the SDFGI layer-completeness fix. The two
temporal research docs are kept as archived research, clearly marked
**IMPLEMENTED THEN REVERTED — NOT SHIPPED**.

Everything up to the spatial mode-6 stack is committed and **pushed** to
`origin` (commits `d8b3d583ae`, `70cc14782e`, `91a0a3e8e4`, `59b7ef60a9`,
`d097fa2ce3`); the temporal milestone-2 commit `cb0aa639c7` was pushed and is
now **superseded**: the revert is committed and pushed as the **revert stack
commit with a docs refresh above it**, and tag `4.8-beta5` was force-moved from
`cb0aa639c7` to the revert stack commit. Branch and tag share the simple name
but are different commits — use `refs/heads/4.8-beta5` / `refs/tags/4.8-beta5`
when the distinction matters. Fresh toolchain installs fetch the tag, i.e. the
current (spatial-only) driver/template. Verify with
`git ls-remote origin refs/tags/4.8-beta5`. `patches/` is a self-contained
portable bundle (patch files + `layers/` + `upstream.lock`); the volatile build
worktree is `/tmp/godot48` (HEAD `582d619026`) **with the
Forward+/MSAA/TAA/FSR/SDFGI/pmndrs-spatial working-tree changes and the reverted
spatial-only state (plus the three kept SDFGI layer-completeness files),
captured in the regenerated patch stack — the worktree itself is not
committed**. Editor version stays `4.8.beta.custom_build.582d61902`. This file
lives in `docs/` together with `PATCH_STACK.md`, `WGSL_COMPUTE_PATH.md`,
`PMNDRS_UPSCALER_SPATIAL.md`, the two archived temporal research docs
(`PMNDRS_TEMPORAL_PORT.md`, `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` — NOT
SHIPPED) and the archived migration history.

Current state in one screen:

- Forward Mobile: TPS boots with 0 validation errors (baseline from 2026-11-26).
- Forward+: TPS boots, full 3D level, 0 validation errors, 0 SPIR-V→WGSL
  failures, ~5.8k draws/frame at 60 fps (see `docs/webgpu_forward_plus_tps.png`).
- Forward+ extras verified 2026-11-27: **MSAA 4x** and **TAA** (both 0 validation
  errors, full level, ~60 fps). The MSAA/depth non-filtering-sampler bug reported
  from a real browser is fixed (see the dated note below).
- **FSR 1 and FSR 2 work on WebGPU** (browser-verified 2026-11-27, 960x540,
  Quality 1/1.5, 0 validation errors, 60 fps): FSR 2's B10G11R11 storage images
  are promoted to RGBA16F, its image-atomic/subgroup paths have WebGPU
  fallbacks, and FSR 1 upscales through a storage-capable intermediate before
  blitting to the non-storage render target. See the FSR section below.
- **SDFGI is enabled on WebGPU** (tag `529407b9fc`, 2026-11-27): the packed
  `R32_UINT`/`R16_UINT` storage textures were replaced by RGBA16F
  light-cascade/lightprobe storage+sample textures (`SDFGI_LIGHT_FLOAT`), the
  occlusion texture by a dummy float 3D binding with occlusion disabled, and
  the SDF pipelines are compiled against a colour framebuffer with an explicit
  colour pass on attachment-only drivers. Browser-verified 2026-11-27 17:19 on
  the TPS Forward+ export with volumetric fog off: 60 fps, 0 GPU validation
  errors, 0 shader errors. Remaining differences: SDFGI occlusion is disabled,
  and the packed light path is not bit-identical. See the Forward+ section.
- **pmndrs/upscaler milestone 1 works on WebGPU and is a first-class scaling
  mode** (browser-verified 2026-11-27 21:54 through the TPS settings button; the
  underlying effect was first verified 20:48-20:49): the spatial EASU+RCAS pass
  runs as hand-written WGSL compute through the new raw-WGSL RenderingDevice path
  (no SPIR-V, no Tint), selected as
  `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` = 6 (the
  `rendering/scaling_3d/mode` "pmndrs (Spatial - Fast)" option). The TPS demo's
  Settings → Resolution Scale Filter screen now has a dedicated **"pmndrs
  Spatial"** button (shown only on the WebGPU driver, applied live). At render
  scale 0.667, 960x540: 640x360 -> 960x540, `[PMNDRS] spatial upscaler running`
  logged, 59-60 fps, 0 validation/shader errors; the default FSR1 selection on
  the same build shows no running line and 0 errors. See the milestone-1
  section below.
- **pmndrs/upscaler milestone 2 (temporal path) — ATTEMPTED, THEN REVERTED
  (2026-11-28), NOT SHIPPED.** A first working temporal slice (Viewport mode 7,
  `RendererRD::PmndrsTemporal` in the FSR2 slot, TPS "pmndrs Temporal" button)
  was built, exported and browser-verified in automation, but the owner's
  browser test showed **severe ghosting**. The code was removed in full and the
  stack reverted to spatial-only mode 6 (+ the SDFGI layer-completeness fix).
  The research is kept in `PMNDRS_TEMPORAL_PORT.md` /
  `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` (marked NOT SHIPPED) for a future
  retry; see the milestone-2 history subsection below.
- The Godot Web Server site export is regenerated after each build (last
  written 2026-11-28 00:54:36, the reverted spatial-only build; wasm sha256
  `98ac9e82…`); browsers need a hard reload to pick it up.

**2026-11-27 (later) — browser-reported validation error fixed.** A real-Chrome
report showed `Filtering sampler ... is incompatible with non-filtering sampler
binding` (ResolveShaderRD, thousands of cascaded errors) after enabling MSAA in
the demo settings. Root cause: combined sampler+texture layout entries stored only
the texture half in `bind_group_infos`, so the NonFiltering substitution for
MSAA/depth samplers never fired. The driver now records the required sampler type
per binding (`WGShader::sampler_binding_types`) and applies it at bind-group
creation and in the cross-shader rebind path. MSAA 4x and TAA are now
browser-verified with zero validation errors. TAA also needed its
previous-velocity buffer changed from RG16F to RGBA16F (WebGPU has no RG16F
storage format). The patch stack, staged editor and the Godot Web Server site
export were regenerated.

**2026-11-27 — Forward+ is working.** Forward+
(`rendering/renderer/rendering_method.web="forward_plus"`) now boots and renders
the TPS demo on the WebGPU device: zero WebGPU validation errors, zero
SPIR-V→WGSL failures, 60 fps (VSync) in Chrome for Testing/Dawn, full 3D level
(see `docs/webgpu_forward_plus_tps.png`). The patch stack was regenerated from
the same base commit plus working-tree changes, so the editor version stays
`4.8.beta.custom_build.582d61902`; see "Forward+ on WebGPU (2026-11-27)" below
for what changed and the remaining limitations.

## Where to start (docs map)

| doc | status |
|---|---|
| `README.md` | repo mechanics (patch stack, tools, rules) |
| **this file** | current state: toolchain, build/export, Forward+ (incl. MSAA/TAA/SDFGI), pmndrs spatial upscaler (mode 6), browser verification, MSAA/ring fixes, encryption, temporal-attempt history, open work |
| `PATCH_STACK.md` (same folder) | what every patch layer contains; apply/regenerate commands |
| `WGSL_COMPUTE_PATH.md` (same folder) | raw-WGSL compute path in the WebGPU driver/RD: API, binding convention, as-built notes |
| `PMNDRS_UPSCALER_SPATIAL.md` (same folder) | pmndrs/upscaler spatial-path port spec: vendored WGSL provenance, constants layout, per-pass bindings, integration plan (shipped, mode 6) |
| `PMNDRS_TEMPORAL_PORT.md` (same folder) | **ARCHIVED RESEARCH — IMPLEMENTED THEN REVERTED, NOT SHIPPED.** pmndrs/upscaler temporal-path contract for a future retry |
| `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` (same folder) | **ARCHIVED RESEARCH — IMPLEMENTED THEN REVERTED, NOT SHIPPED.** mode-7 engine-seam guide for a future retry |
| `WEBGPU_DRIVER_MIGRATION.md` | archived history of the komm64 driver adoption (4.7) |

## What this repository is

The repo is a **patch stack**, not a Godot fork checkout:

- ~~`patches/4.7.2`~~ — the old 4.7.2 stack, **dropped and deleted 2026-11-26**
  from GitHub (as was `integrate/komm64-webgpu`). The project is consolidated on
  the 4.8 stack. The full-source `4.7.2` branch remains on the remote for
  history only (`origin/4.7.2` = `d8cb1fd77c`, a Godot 4.7.2 tree); a leftover
  local branch `fresh` still points at that same commit and can be deleted.
- The 4.8 stack (branch and tag `4.8-beta5`, repo default branch) is a
  **self-contained portable bundle in `patches/`**: patch files
  (`0001-…0099-*.patch`), the `layers/*.files` regeneration lists and
  `upstream.lock` (the Godot pin) all live there; ported onto godot `master`
  @ `67421f36e5` (4.8.0-beta). Copying `patches/` + `tools/apply.sh` reproduces
  the stack elsewhere. Only one stack exists now.
- `tools/apply.sh <godot-checkout>` applies the bundle (every `patches/*.patch`
  in order); `tools/make-patches.sh <godot-checkout>` regenerates it with
  `git diff --binary`
  (icons/splash/logo/SPIR-V fixtures are binary patches: 8 hunks in `0005`, 15
  in `0007`). The script was fixed 2026-11-26 (it lacked `--binary`, used
  bash-4-only `mapfile`, and rejected linked worktrees); its round-trip from
  `/tmp/godot48` is byte-identical to the committed patches.
- `patches/upstream.lock` pins the Godot base (inside the bundle).

| ref | commit | notes |
|---|---|---|
| branch `4.8-beta5` = `origin/4.8-beta5` | revert stack commit + docs refresh | docs tip (HEAD) is the docs refresh above the **revert stack commit**; both are committed and pushed. History below them: temporal milestone-2 commit `cb0aa639c7` (pushed, then superseded by the revert), `d097fa2ce3`/`59b7ef60a9` docs, `91a0a3e8e4` (first-class pmndrs spatial mode 6), `d8b3d583ae` (raw-WGSL path + pmndrs spatial upscaler). Branch and tag share the simple name (use `refs/heads/4.8-beta5` / `refs/tags/4.8-beta5` when the ambiguity matters) |
| tag `4.8-beta5` | revert stack commit | engine/patch-stack tip: raw-WGSL compute path + **pmndrs spatial upscaler + first-class pmndrs spatial Viewport scaling mode (mode 6)** on top of Forward+/MSAA/TAA/FSR/SDFGI, **plus the SDFGI layer-completeness fix**; the pmndrs temporal milestone 2 is **not** in this stack. **Force-moved 2026-11-28 from `cb0aa639c7` (the temporal stack) to the revert stack commit**, and the branch pushed, so fresh toolchain installs bootstrap the spatial-only driver/template. Lightweight tag (`git cat-file -t refs/tags/4.8-beta5` → `commit`); verify with `git ls-remote origin refs/tags/4.8-beta5` |
| `origin/4.7.2` | `d8cb1fd77c` | full-source **Godot 4.7.2** snapshot (single parentless commit), history only, not maintained |
| ~~tag `4.8-beta5-source`~~ | — | **release + tag removed 2026-11-26.** It held a parentless 4.8 source snapshot (`35e42f7d`, re-cut from the wrong 4.7.2 tree earlier that day) and the `fast-godot-4.8-beta5-src.zip` asset. Editors built before 2026-11-26 hardcoded that asset URL for first-use provisioning and can no longer bootstrap a fresh toolchain. New builds use Godot's repo + the `4.8-beta5` patch tag |

**Toolchain bootstrap (2026-11-26) no longer uses a source release.** New editor
builds download the patch stack tag archive (~8 MB), read `godot_commit` from its
`patches/upstream.lock`, download Godot's own source at that commit from
`godotengine/godot` (~76 MB), apply the stack's layers with `git apply`, and move
the result into `<toolchain>/engine/source` — see the Toolchain bootstrap
subsection under Build / toolchain. The old release asset and its tag were
**removed 2026-11-26**: editors built before that date hardcoded the asset URL
and can no longer provision a fresh toolchain (exports from machines that
already have a toolchain are unaffected).

## Environment snapshot (verified on this machine, 2026-11-28 ~01:00)

| item | actual value |
|---|---|
| patch stack repo | branch `4.8-beta5` = `origin/4.8-beta5` = docs refresh above the **revert stack commit** (**repository default branch**; both committed and pushed 2026-11-28). Tag `4.8-beta5` = the same revert stack commit — **force-moved from `cb0aa639c7`** (the temporal milestone-2 stack, pushed then superseded) so fresh toolchain installs bootstrap the spatial-only driver/template. The reverted stack is spatial-only mode 6 + the SDFGI layer-completeness fix: `patches/0004` 891,250 B, `patches/0005` 5,940,065 B, `0005` layer list 159 paths, **0 `PMNDRS_TEMPORAL`/`PmndrsTemporal`/`pmndrs_temporal` references** in the patches. Branch and tag share the simple name — `refs/heads/4.8-beta5` / `refs/tags/4.8-beta5` disambiguate |
| engine base | Godot `master` @ `67421f36e5` (4.8.0-beta), pinned in `patches/upstream.lock`; still an ancestor of the worktree HEAD |
| object model | this repo is a **partial clone** of `godotengine/godot` with `blob:none` promisor packs, so Godot-history blobs are fetched from upstream on demand. `port48` carries 87,467 commits not on `origin`, so full-source refs are pushed as single parentless snapshots instead (as `origin/4.7.2` is) |
| build worktree | `/tmp/godot48` — branch `port48`, HEAD `582d619026` (bundle upstream.lock bootstrap; `cdc0907bf6` patches/ layout, `9267990dcd` ring hardening, `130ed7f48d` bootstrap, `a495fc3e93` ring mitigation and `834c3d4fc2` pack-key port are ancestors) **reverted to the spatial-only mode-6 state: the uncommitted Forward+/MSAA/TAA/FSR/SDFGI/pmndrs-spatial changes (52 modified files + 3 new `effects/pmndrs_upscaler*` files) plus the three kept SDFGI layer-completeness files; all temporal files/code removed (0 grep hits for `PmndrsTemporal`/`PMNDRS_TEMPORAL`/`pmndrs_temporal` in `servers/ scene/ drivers/ platform/ modules/`)**. Volatile and not pushed; the revert patch stack was regenerated from it 2026-11-28 ~00:5x, so a fresh checkout + `tools/apply.sh` reproduces the build tree |
| patch round-trip | 2026-11-28 ~00:52: applying the full regenerated **revert** stack to a pristine `67421f36e5` checkout (`/tmp/applytest`, all 1,787 layer paths staged) and re-diffing each layer with its `patches/layers/*.files` path list reproduces **all 8 `patches/*.patch` files byte-identically** (independently re-checked; `0001` 508,662 B, `0002` 203,592 B, `0003` 45,856 B, `0004` 891,250 B, `0005` 5,940,065 B, `0006` 15,131,334 B, `0007` 3,541,178 B, `0099` 29,700 B; `0005` layer list 159 paths) |
| layer completeness | all **55/55** worktree-changed paths are covered by the union of `patches/layers/*.files` (checked after the revert). The per-layer round-trip check only diffs the *listed* paths, so it cannot detect an omitted file: three SDFGI files (`servers/rendering/renderer_rd/environment/gi.h`, `…/shaders/environment/gi.glsl`, `…/shaders/environment/sdfgi_integrate.glsl`) had been missing from every layer list and were added in the (kept) M2 completeness fix — that is why the spatial-only layer list is 159 paths (156 pre-M2 + 3) and `0005` is 5,940,065 B rather than the pre-M2 5,936,829 B |
| other worktree | `/tmp/ref472` — detached at `d8cb1fd77c` (the 4.7.2 full-source tree, reference only) |
| toolchain bootstrap | downloads `godotengine/godot` @ `67421f36e5` (~76 MB) + the `fast-godot` tag `4.8-beta5` archive (~8 MB) and applies the stack with the system **git**; `GRIT_ENGINE_PROBE=1` runs just this step headlessly |
| toolchain (single SDK) | `~/fast_godot_toolchain`, 6.4 GB: Emscripten 6.0.12, Python 3.12.16 (+ zstandard 0.25.0), SCons 4.11.1, MoltenVK 1.4.2, `engine/source` = 4.8.0-beta + the current stack, **synced from the reverted `/tmp/godot48` before the revert builds** (revert file mtimes on disk 2026-11-28 00:50:56; the temporal effect files are gone from the toolchain tree) |
| staged editor | `out/editor-staging/Fast_Godot_4.8.app` = `4.8.beta.custom_build.582d61902`, **reverted-tree build 2026-11-28 00:52:28** (`/tmp/editor_revert.log`: `scons: done building targets.`); `Contents/MacOS/Godot` is 110,961,808 B, sha256 `cd72898f…`. `Fast_Godot_4.8-macos.zip` written 00:54:48 (58,090,118 B) |
| installed editor | `~/Applications/Fast_Godot_4.8.app` is **updated to the same reverted binary** (sha256 `cd72898f…`) |
| web template | rebuilt from the reverted tree (`/tmp/web_revert.log` 00:52:24, `scons: done building targets.`, 35.22 s) and packaged by the 00:54 revert export: `out/editor-staging/godot.web.template_release.wasm32.nothreads.zip` written 00:54:48, 14,088,322 B; `godot.wasm` 50,471,223 B, wasm sha256 `98ac9e82…`, zip sha256 `84202e5d…`. It contains only the pmndrs **spatial** effect (mode 6): the wasm has the `[PMNDRS] spatial upscaler running` string and **no** temporal-upscaler/`pmndrs_temporal` strings |
| export test site | `~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/` — TPS demo HTML5 Fast **Forward+** export re-run from the reverted tree, written 2026-11-28 00:54:36 (`/tmp/export_revert2.log`, 111 s, spatial-only): `index.wasm` 50,471,223 B sha256 `98ac9e82…` (same as the template), `index.pck` 987,358,768 B. Hard-reload browsers to pick it up |
| build record | `out/editor-staging/Fast_Godot_4.8_BUILD.txt` was **refreshed 2026-11-28 00:56:07** for the revert (M2 changed to a revert note; editor sha `cd72898f…`, template zip sha `84202e5d…`, wasm `98ac9e82…`). Its **Site export paragraph is stale**, though: it still says the site was updated 00:28-00:36 with the M2 temporal build and still mentions a "pmndrs Temporal" button — trust the site row above, not that paragraph. BUILD.txt is not part of `patches/` |
| source release | **removed 2026-11-26** (was tag `4.8-beta5-source` = `35e42f7d` + `fast-godot-4.8-beta5-src.zip`). New builds bootstrap from Godot's repo + the `4.8-beta5` patch tag; editors built before the removal can no longer provision a fresh toolchain |

## 4.8 layer map

1. `0001-modules-grit` — Fast/Grit AOT compiler (adapted to 4.8 GDScript
   internals: GDType member maps, const MethodBind, parser resolved_type).
2. `0002-modules-cffi`.
3. `0003-web-html5-fast-export` — HTML5 Fast platform + web hooks.
4. `0004-webgpu-driver` — komm64/4.7 WebGPU driver rebased to 4.8
   (`buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`,
   readback/upload conversion, layered copy/init, per-stage SPIR-V version).
   Forward+ additions: compute-pass usage-scope splitting, Tint-risk bake
   pre-scan/`OpBitcast` rewrite, per-binding required sampler types
   (`sampler_binding_types`), texture-format/sample-type/fallback-view fixes,
   B10G11R11→RGBA16F storage promotion + WGSL rewrite (FSR 2 history), SDFGI
   integer sample-type pin for `GiShaderRD` binding 28 (Tint drops that unused
   declaration). **Raw-WGSL compute path** (pmndrs milestone 1): container
   `set_code_from_wgsl_compute()` fills reflection + per-stage WGSL and
   mechanically rewrites authored `@binding(N)` → `@binding(2*N)` (the driver's
   SPIR-V binding-doubling convention), plus a reflection↔WGSL pair check;
   `_shader_build()` recognises the raw stage marker (WGSL present, SPIR-V
   empty) and skips the Tint-output WGSL rewrites for it. (The M2 temporal
   attempt briefly added raw-WGSL sample-type auto-layout and depth-only views
   here; **both were reverted with milestone 2** — `0004` is back to 891,250 B.)
5. `0005-engine-renderer-patches` — batching (`batch_instance_index`), shader
   container format override, driver traits, GDScript native-call hooks, export
   window, WGSL shader baker plugin, icons/splash, frame profiler. Forward+:
   HTML5 Fast accepts `forward_plus`; cluster builder plain-atomics +
   `USE_ATTACHMENT` fallback; scene-shader identity subgroup macros; FSR1/FSR2
   fp16 gating; FSR2 WebGPU paths (no image atomics / no subgroup SPD, rgba16f
   output) and the FSR1 intermediate blit; **SDFGI enabled on WebGPU** (RGBA16F
   light-cascade/lightprobe storage+sample textures via `SDFGI_LIGHT_FLOAT`,
   dummy float 3D occlusion binding with occlusion off, integer voxel-GI
   default, colour-framebuffer SDF pipelines with an explicit colour pass);
   volumetric-fog dead binding removed; TAA RGBA16F previous-velocity buffer for
   WebGPU storage compatibility. **pmndrs milestone 1**: raw-WGSL RD API
   (`WGSLUniform`/`WGSLShaderData`, `RenderingDevice::shader_create_from_wgsl()`,
   shared private `_shader_register()`, non-pure
   `RenderingDeviceDriver::shader_create_from_wgsl()` default-failing hook,
   `SUPPORTS_RAW_WGSL_COMPUTE` feature), the `RendererRD::PmndrsSpatial` effect
   (new `effects/pmndrs_upscaler{,_wgsl}.{h,cpp}` files) and the **first-class
   pmndrs scaling mode**: `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` and
   `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL` = 6 (appended after
   `SCALING_3D_MODE_NEAREST`, so pre-existing values are unchanged and `*_MAX`
   is 7), the `rendering/scaling_3d/mode` hint `pmndrs (Spatial - Fast):6`, the
   `doc/classes/{Viewport,RenderingServer}.xml` enum docs, the Forward+-only
   guard + spatial fallback semantics in `renderer_viewport.cpp`, and mode-6 →
   PmndrsSpatial selection with FSR1 fallback in `renderer_scene_render_rd.cpp`.
   The old `rendering/scaling_3d/pmndrs_spatial` bool setting was removed.
   **Layer completeness (kept):** the `0005` list is **159 paths** — the 156 of
   the spatial-only stack plus three SDFGI files that had been missing from
   every layer list (`environment/gi.h`, `shaders/environment/gi.glsl`,
   `shaders/environment/sdfgi_integrate.glsl`), added so the stack can
   reproduce the SDFGI build (`0005` is 5,940,065 B for that reason). The M2
   temporal effect/mode-7 entries were removed with the revert.
6. `0006-thirdparty-vendored` — Tint + SPIRV-Tools.
7. `0007-tools-fixtures-docs` — tools/, toolchain, test_project, docs.
8. `0099-misc`.

## Build / toolchain

### Single toolchain

There is exactly **one toolchain** for the whole project (4.7.2 was dropped):

- Path: `~/fast_godot_toolchain` (~6.4 GB as of 2026-11-26, including the 4.7 GB
  `engine/source` copy and build caches), provisioned by
  `tools/toolchain/download_toolchain.sh` (the same script is in the 4.8 patch
  stack; the editor runs it automatically on first use).
- Contents: Python 3.12.16 + **zstandard 0.25.0**, SCons 4.11.1, MoltenVK 1.4.2,
  **Emscripten 6.0.12** (SHA-256 pinned emsdk tarball), and the 4.8 engine source
  tree at `engine/source`.
- No other SDK version is pinned or supported in this project: 6.0.12 is used
  for both editor-side building and Grit HTML5 Fast template builds. (Godot 4.8
  requires >= 6.0.1; the older 4.0.11 pin died with the 4.7.2 stack.)
- Grit builds use the engine source **inside** the toolchain
  (`engine/source`). It is re-synced from the 4.8 worktree after driver/tooling
  changes (see the `/tmp/godot48` note below); the 4.8 provisioner rejects any
  non-4.8 tree via the `version.py` minor check.
- The provisioning script sets `EMSDK_PYTHON` internally (via grit_build.py) to
  the toolchain's own Python 3.12; manual `scons platform=web` runs need
  `PATH=$HOME/fast_godot_toolchain/python/bin:$PATH` because macOS's python3 is
  3.9 and emcc refuses it. Verified 2026-11-26: `emcc --version` asserts under
  the system 3.9 and prints `6.0.12` once the toolchain python is first on
  `PATH`. `emsdk/.emscripten` still records a nonexistent
  `python/3.13.3_64bit` interpreter; ignore it — the toolchain is launched via
  `PATH`, not that config.

### Toolchain bootstrap (2026-11-26)

The first-use bootstrap (`modules/grit/editor/grit_provisioner.cpp`, patch 0001)
builds the toolchain's engine source the same way this editor is built:

1. download `https://github.com/2maac/fast-godot/archive/refs/tags/4.8-beta5.tar.gz`
   (~8 MB) — the patch stack;
2. read `godot_commit` from its `patches/upstream.lock` (the single pin);
3. download `https://github.com/godotengine/godot/archive/<commit>.tar.gz` (~76 MB);
4. apply the archive's `patches/*.patch` files in order with the system
   `git apply` (the tree is a plain directory; the patches target the exact
   pinned commit);
5. validate `SConstruct` + `tools/grit_build.py` + `version.py` `minor = 8`, move
   the tree to `<toolchain>/engine/source`, then run its `download_toolchain.sh`.

Consequences:

- **git is required** on a machine that installs build tools (clear error
  otherwise); first-use downloads total ~84 MB;
- the source-release asset/tag were **removed 2026-11-26**; editors built before
  that date lose fresh-toolchain provisioning (existing toolchains keep working);
- **the `4.8-beta5` tag is what fresh installs fetch**: it is a lightweight tag
  (`git cat-file -t refs/tags/4.8-beta5` → `commit`). History: last moved on
  2026-11-27 to `529407b9fc` (SDFGI tip), then to `91a0a3e8e4` (pmndrs spatial
  mode 6), then briefly to `cb0aa639c7` (pmndrs temporal milestone 2), and then
  **force-moved on 2026-11-28 from `cb0aa639c7` to the revert stack commit**
  (spatial-only mode 6 + SDFGI layer-completeness fix), with the branch pushed
  and the docs refresh above it. So new toolchains build the
  SDFGI/FSR/Forward+/pmndrs-spatial driver/template (no temporal mode) and read
  the self-contained `patches/` bundle. Move the tag again when engine-side
  fixes land; docs commits after a move sit on the branch above it, and
  editor-only changes don't matter for template builds;
- `GRIT_ENGINE_PROBE=1` exercises just the source step headlessly
  (`GRIT_DOWNLOAD_NOW=1` still runs the whole toolchain bootstrap).

Verified 2026-11-26: the source step produced `engine/source` with 16,064 files
byte-identical to the built worktree (apart from the provisioner files of the
then-uncommitted change), and replaying the same download+apply+compare sequence
from a fresh Godot tarball reproduced all layers with zero mismatches.

Editor (macOS, from a checkout + applied stack):
```
scons platform=macos arch=arm64 target=editor \
  vulkan_sdk_path=$HOME/fast_godot_toolchain/moltenvk \
  tests=no debug_symbols=no angle=no production=yes
```
The current 4.8 build tree is a **volatile linked git worktree of this repo at
`/tmp/godot48`** (`git worktree list` shows it; branch `port48`, HEAD
`582d619026` **plus uncommitted Forward+/MSAA/TAA/FSR/SDFGI/pmndrs-spatial
changes and the three kept SDFGI layer-completeness files** — not pushed; the
pmndrs temporal code was removed by the 2026-11-28 revert). Those engine changes
were regenerated into `patches/*.patch` and the committed part is on the repo
branch (`b81f3de2c1` MSAA/TAA, `f799881f28` FSR/SDFGI injection, `529407b9fc`
SDFGI enabled, `d8b3d583ae` raw-WGSL path + pmndrs milestone 1, `91a0a3e8e4`
first-class pmndrs spatial Viewport mode 6; the temporal commit `cb0aa639c7` was
superseded by the revert stack commit); the patch stack is the source of
truth. Ancestors:
bundle-lock bootstrap `582d619026`, patches/ layout `cdc0907bf6`, ring hardening
`9267990dcd`, source bootstrap `130ed7f48d`, MSAA mitigation `a495fc3e93`,
pack-key port `834c3d4fc2`. If `/tmp` is wiped, rebuild by applying the repo's
`patches/*.patch` files to a fresh Godot checkout.

Staged build (gitignored): `out/editor-staging/`
- `Fast_Godot_4.8.app` (`4.8.beta.custom_build.582d61902`, Fast Godot icon,
  ad-hoc signed; **reverted-tree build 2026-11-28 00:52:28**, 110,961,808 B) and
  `Fast_Godot_4.8-macos.zip` (written 00:54:48, 58,090,118 B)
- `godot.web.template_release.wasm32.nothreads.zip` (rebuilt from the reverted
  tree; written 2026-11-28 00:54:48, 14,088,322 B; its `godot.wasm` sha256
  `98ac9e82…` is the one the site export runs; spatial mode 6 only, no temporal)
- `Fast_Godot_4.8_BUILD.txt` (refreshed 2026-11-28 00:56:07 for the revert build;
  its Site-export paragraph is stale — it still mentions the 00:28-00:36 M2
  export and a "pmndrs Temporal" button — trust the binary/template hashes)
The launchable install `~/Applications/Fast_Godot_4.8.app` is **updated to the
same reverted binary** (sha256 `cd72898f…`).
Note: the app folder's mtime can read like an old timestamp — use
`Contents/MacOS/Godot` mtime/version.

## Export flow (HTML5 Fast)

Preset platform `HTML5 Fast`, `shader_baker/enabled=true`. Export from a
**windowed** editor (the baker needs a renderer), and pick the engine-side
renderer with `--rendering-method`. The launched project must also have the
matching web renderer setting (`rendering/renderer/rendering_method.web`), e.g.
TPS uses `"forward_plus"`:
```
GRIT_JOBS=6 Fast_Godot_4.8.app/Contents/MacOS/Godot --path <project> \
  --rendering-method forward_plus --rendering-driver vulkan \
  --export-release "HTML5 Fast" "<site>/index.html"
```
Exports go straight to the Godot Web Server site folder:
`~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/`.
Results: test_project cold export 5m22s, warm 12s (SCons object cache + host
tint-cli cache); TPS demo Mobile export 2m37s. The TPS **Forward+** export bakes
~1524 shader-stage variants and takes ~3-4 min warm (up to ~6 min cold);
it boots at 59-60 fps with 0 GPU validation errors. The SDFGI verification
export (volumetric fog off) completed 2026-11-27 17:17 — its log bakes the
`Sdfgi*ShaderRD` WebGPU caches — and its browser run at 17:19 reported 60 fps
with 0 GPU validation errors and 0 shader errors (throwaway harness under
`/tmp/node-test/`). The current site export is the **post-revert spatial-only
build**: exported 2026-11-28 00:54:36 from the reverted tree
(`/tmp/export_revert2.log`, 111 s) into the Web Server site folder — wasm
50,471,223 B sha256 `98ac9e82…`, pck 987,358,768 B; the exported wasm contains
the pmndrs spatial path and **no** temporal upscaler. It is the build used for
the post-revert check (see the milestone-2 history section). Earlier exports
remain as history: the M2 temporal build (2026-11-28 00:28:52, wasm
`3fab49b3…`), the spatial-mode build (2026-11-27 21:53:27, wasm `3b25ba0b…`) and
the 20:47 bool-setting A/B (`/tmp/site-pmndrs`, `/tmp/site-fsr1`).

### Browser verification recipe (Forward+)

1. Export the TPS project with `--rendering-method forward_plus` (project setting
   `rendering_method.web="forward_plus"`) into the site folder above.
2. Serve it on a **fresh port** (`python3 -m http.server <port> --bind 127.0.0.1`
   from the site dir — never reuse a port that may still have an old server).
3. Open `index.html` in a Chromium with WebGPU (Chrome for Testing or Chrome/
   Brave with `--enable-unsafe-webgpu` if needed) and capture the console.
4. Expect: `WebGPU 1.0 - Forward+ - Using Device #0`, the TPS menu, then the
   level after PLAY; `[PERF] fps≈60 draws/f≈5.7k RingOF=0` in gameplay; and
   **0** `GPUValidationError` / `SPIR-V to WGSL failed` lines.
5. Repeat with the demo's settings screen to exercise MSAA 4x, TAA and SDFGI
   (all three were verified 2026-11-27 with 0 errors; SDFGI was verified with
   volumetric fog off and the GI type set to SDFGI). Enabling a setting persists
   it in the browser's `user://` config, which is why a previously-saved MSAA or
   fog setting can change what a plain reload runs.
6. To exercise the **pmndrs spatial upscaler**, use the TPS **Settings** screen:
   **Resolution Scale Filter → "pmndrs Spatial" → Apply**, then PLAY. The
   resolution scale must be below Native (a spatial upscaler at scale 1.0 is
   internally switched to mode OFF, so nothing runs). Expect the one-time
   `[PMNDRS] spatial upscaler active (EASU+RCAS, raw WGSL)` line and
   `[PMNDRS] spatial upscaler running (EASU+RCAS) 640x360 -> 960x540, 120x68
   groups` at 960x540, plus the usual 0 validation errors. The default FSR1
   selection on the same build shows only the `active` line (no `running` line).
   Verified 2026-11-27 21:54 with `/tmp/node-test/tps_pmndrs_button.js`; logs
   `/tmp/pmndrs_button_console.log` and `/tmp/mode_fsr1_baseline_console.log`,
   screenshot of the screen `/tmp/pmndrs_button_settings.png`. The button is
   demo-side code (not in the patch stack) and only appears when the web
   rendering driver is `webgpu`. At project level the same mode is
   `rendering/scaling_3d/mode=6` (`pmndrs (Spatial - Fast)`). **The "pmndrs
   Temporal" button/mode is gone after the 2026-11-28 revert**; a stale saved
   `scale_filter` >= `SCALING_3D_MODE_MAX` is sanitized back to FSR 1 by the
   demo (`settings.gd`), and the unknown-value fallback selects FSR 1.

7. For a revert build, also confirm the **absence** of the temporal mode in the
   served wasm and UI: the demo settings screen has only "pmndrs Spatial" in
   Resolution Scale Filter, and the wasm contains the
   `[PMNDRS] spatial upscaler running` string but **no**
   `temporal upscaler running` / `pmndrs_temporal` strings (checked 2026-11-28
   00:54 build, wasm sha `98ac9e82…`).

Grit build performance/cancel fixes in the 4.8 stack:
- host `tint_convert_cli` cached under `$toolchain/tint-cli-cache`
  (~/.cache/fast-godot/tint-cli fallback), serialized by a lock, capped to 6
  jobs, -O1;
- SCons objects shared across profile trees via `cache_path`;
- cancel (Esc) kills the whole process group, so compilers stop too.

## PCK encryption — white-box keystream (2026-11-26)

HTML5 Fast exports support encrypted PCKs again, with the 4.7.2 white-box design
ported to 4.8:

- `core/core_builders.py` generates `script_encryption_key.gen.cpp` from the
  **per-export pack key** (`GRIT_PACK_KEY`): a Chow-style white-box AES-256 table
  network (`_wb_make_tables`/`_wb_emit_tables`, random invertible byte encodings
  between rounds). The pack key never exists as bytes at runtime - a dump yields
  tables, a breakpoint yields one CFB keystream block. An editor/plain build (no
  pack key) instead emits the split-share key form so its editor-side code paths
  keep working.
- `FileAccessEncrypted::open_and_parse_keystream()` decodes the stock CFB stream
  through the provider (CFB decryption only needs AES **block encryption**),
  keeping the stock MD5 check. The stock editor-side writer is unchanged, so
  nothing in the export format moved.
- `file_access_pack.cpp` uses the keystream provider on both load paths when the
  build has no recoverable key; otherwise it falls back to the plain key.
- `core/grit_script_key.cpp` (the 4.8 shim with an all-zero key) is gone; the
  generated file supplies `script_encryption_key_{get,keystream_available,encrypt_block}`.
- Per-export wiring was already in the export plugin: with preset
  `encrypt_pck`/`encrypt_directory`, the plugin generates a fresh pack key,
  writes `script_pack_key.txt`, sets `GODOT_SCRIPT_ENCRYPTION_KEY`, and passes
  `GRIT_PACK_KEY`/`GRIT_WB_SEED` to the helper so the template is rebuilt for
  exactly that pack key (only the generated file's rebuild is triggered).

Verified: an encrypted `test_project` export contains no plaintext resource
paths, boots in Chrome/WebGPU with **0 errors and no key present**; an
unencrypted export is byte-identical to before the port (25,917,148-byte PCK)
and still boots.

Template-build caveat: encrypted builds of the **editor** (no pack key in the
environment) still compile, using the split-share key form; only Grit template
builds for an encrypted export get the table network.

## MSAA flicker — root cause and fix (2026-11-26)

Symptom: TPS demo flickered with MSAA 2x/4x in gameplay only (geometry
disappearing/reappearing, later confirmed as whole frames presenting black
except the 2D HUD); menu and MSAA-off were stable.

How it was found: a debug autoload orbiting a camera around the level Core in a
scratch copy produced a deterministic reproduction (2-10 black frames per 48 at
MSAA 4x, none with MSAA off), and per-frame driver counters showed the frames
were fully recorded - passes, draws and resolve attachments all present, **zero
GPU validation errors**.

Root cause: `PUSH_CONSTANT_RING_SIZE` was 512 KB = 2048 slots. A Core-facing TPS
frame issues ~2.2k draws, so the ring overflowed mid-frame. The overflow flush
ends the active render pass (which resolves its MSAA colour), submits, and
restarts the pass with `LoadOp_Load`; with MSAA that corrupts the resolved
target, so the frame presents without its 3D content. Without MSAA there is no
resolve target, which is why the bug never appeared there. Facing the Core is
the worst case because it has the most draws.

Mitigation (first step, 2026-11-26): the ring grew to 8 MB (32768 slots, ~15x
headroom). Verified with the orbit capture: before, 2-10 black frames per 48;
after, none and `RingOF=0`. User-verified in a real browser on TPS with MSAA 4x.

**Structural hardening (2026-11-26): the flush never runs from inside a pass.**
`_ring_pass_boundary()` runs before every render/compute pass: it measures the
pass that just ended, keeps `2x` the largest pass usage free (1 MB floor), and
when the next pass would not fit it freezes the shadow data, finishes the
encoder, submits **between passes** (no pass active) and resets the ring. A pass
therefore always starts with room for a pass like recent ones, so an
MSAA-resolving pass is never ended early. Residual: a *single* pass needing more
than the whole ring (>32768 slots — vs ~2.2k draws for a whole TPS frame) still
hits the old fallback, which now warns "overflowed inside a single pass" and
counts as `RingOF`.

Verification: the wasm template rebuilt clean and contains the new warning plus
the `passFlush=` DBGFRAME field; TPS was exported and the owner's browser check
passed (2026-11-26, re-verified through the 2026-11-27 Forward+ runs, where
`RingOF=0` in gameplay). For counters
enable `rendering/webgpu/debug_frames`: `passFlush` may be >0 on a heavy frame,
`ringOF` must stay 0. Verified separately that the moved `4.8-beta5` tag archives
(`529407b9fc` at the SDFGI/FSR tip, and `91a0a3e8e4` for the pmndrs stack) contain
the hardened driver patch (`_ring_pass_boundary` in `0004`) and the pmndrs/SDFGI
patches in `0004`/`0005`, so fresh toolchain installs get the same driver.

2026-11-27: Forward+ with MSAA 4x was also browser-verified end-to-end (full
level, 0 validation errors, ~60 fps) after the non-filtering-sampler fix described
in the Forward+ section — MSAA is no longer a Mobile-only verified path.

Diagnostics kept in the driver: `rendering/webgpu/debug_frames` prints per-frame
pass/draw/resolve/ring counters (including `passFlush`) to the browser console;
`rendering/webgpu/disable_batching` disables the batch/firstInstance traits and
was used to rule the batching path out.

## FSR 1 / FSR 2 on WebGPU (2026-11-27)

Both 3D upscalers now run on the WebGPU device. Verified with TPS in Chrome for
Testing/Dawn at 960x540, `resolution_scale` 1/1.5 (Quality), VoxelGI and
Forward+: `[PERF] fps≈60`, zero `GPUValidationError`, zero shader/SPIR-V errors,
and the selected upscaler actually active (`window.scaling_3d_mode` = FSR / FSR2).

FSR 2:

- B10G11R11 has no WebGPU storage texel format. The driver now promotes
  storage-usage B10G11R11 textures to RGBA16F and rewrites the WGSL storage
  declaration (`rg11b10ufloat`/`r11f_g11f_b10f` → `rgba16float`); FSR2's
  previous-colour history is the only user.
- FSR2's fp16 variants stay disabled when `SUPPORTS_HALF_FLOAT` is false, and the
  remaining WebGPU-hostile paths are compiled out: the image atomics in
  reconstruct-previous-depth (`imageAtomicMin/Max`) become a load/store, and the
  SPD luminance pyramid uses `SPD_NO_WAVE_OPERATIONS` + `SPD_NO_IMAGE_ATOMICS`
  (deterministic last-workgroup election, no subgroup quad swaps) when the
  device has no image atomics/subgroups.
- The app-controlled upscaled output is declared `rgba16f` in the FSR2 shaders,
  matching the Forward+ target; a format-less storage image is invalid WGSL.
- The effect is no longer skipped: `FSR2Effect::is_supported()` now probes true
  on WebGPU, and the viewport FSR2→FSR1 fallback no longer triggers. The
  fallback remains for devices that really lack B10G11R11 storage support.

FSR 1:

- FSR 1's RCAS pass writes its destination as a storage image, but WebGPU render
  targets (swap-chain textures) are not storage-capable. The renderer now
  upscales into a storage-capable RGBA16F intermediate and blits that to the
  render target; the old path remains for devices whose render target supports
  storage.

Performance: FSR 2 and FSR 1 both hold 60 fps (VSync) in the TPS run. FSR2's
WebGPU shaders are compiled at runtime (the offline bake uses the host's feature
set, so the WebGPU-gated defines do not match the baked cache) and are then
cached for the session; this affects startup, not the frame rate.

The first-class **`pmndrs Spatial`** scaling mode (mode 6) runs the **pmndrs
spatial upscaler** (milestone 1) in the same spatial-upscale slot; mode 1 is
real AMD FSR 1 again — see the pmndrs milestone-1 section below.

**2026-11-28 note:** during the temporal milestone-2 work FSR 2 was reported as
disabled / falling back to FSR 1 on the WebGPU build (its B10G11R11 storage
requirement), so it was not usable as a temporal A/B reference. The FSR 2 text
above is the dated 2026-11-27 record; re-check FSR 2 status if a future retry
wants it as a reference.

## pmndrs/upscaler spatial upscaler on WebGPU (milestone 1, 2026-11-27)

Milestone 1 is **implemented and browser-verified**: the pmndrs spatial path
(EASU → RCAS) runs as hand-written WGSL compute on the WebGPU device through a
new raw-WGSL RenderingDevice path (no SPIR-V, no Tint), selected by the
first-class `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` scaling mode (6) in the
spatial-upscaler slot. The design/spec docs are
`WGSL_COMPUTE_PATH.md` (driver/RD API as built) and
`PMNDRS_UPSCALER_SPATIAL.md` (vendored WGSL provenance, constants layout,
per-pass bindings, integration plan).

### What landed

- **Raw-WGSL compute path** (layer `0004` for `drivers/webgpu/`, layer `0005`
  for `servers/rendering/`; details in `WGSL_COMPUTE_PATH.md` §11):
  `RenderingDevice` gained a public `shader_create_from_wgsl()` with
  `WGSLUniform`/`WGSLShaderData` reflection structs, a private
  `_shader_register()` shared with the bytecode path, and the WebGPU driver
  implements the new non-pure virtual
  `shader_create_from_wgsl()` (all other drivers' default returns an invalid
  `ShaderID` + error). The new RD feature flag is
  `SUPPORTS_RAW_WGSL_COMPUTE` (true only on WebGPU). The WebGPU shader container
  factory `set_code_from_wgsl_compute()` fills reflection + per-stage WGSL and
  mechanically rewrites the authored `@binding(N)` to `@binding(2*N)` (driver
  convention: the SPIR-V preprocessing doubles bindings), checking every
  reflected binding has a matching declaration. The driver's `_shader_build()`
  accepts a stage with WGSL and empty SPIR-V (raw marker) and skips the
  Tint-output WGSL rewrites for it.
- **The effect**: `RendererRD::PmndrsSpatial` in the new
  `servers/rendering/renderer_rd/effects/pmndrs_upscaler.{h,cpp}`, with the
  WGSL embedded as C++ raw strings in `pmndrs_upscaler_wgsl.h` (vendored
  verbatim from pmndrs/upscaler **v0.6.0, commit
  `f6e52098edffb66e16298a30c53b4da37b4b7d7f`**, MIT + AMD FidelityFX notice
  included; no runtime shader files). One 96-byte `FsrConstants` payload per
  `process()` in a 256-byte UBO, 64-slot ring; EASU writes a render-res →
  display-res RGBA16F storage intermediate under render-buffer scope
  `PMNDRS/easu_output` (SAMPLING|STORAGE); RCAS writes the FSR1 slot's
  destination (WebGPU's existing storage-capable RGBA16F intermediate + blit);
  dispatch `ceil(display/8)` with 8×8×1 workgroups and a barrier between the
  two passes. Two one-time console lines: `[PMNDRS] spatial upscaler active
  (EASU+RCAS, raw WGSL)` when the effect is available, and `[PMNDRS] spatial
  upscaler running (EASU+RCAS) WxH -> WxH, NxM groups` the first time it
  actually upscales.
- **First-class scaling mode + selection** (layer `0005`):
  `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` = 6 and
  `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL` = 6, appended after
  `SCALING_3D_MODE_NEAREST` so all pre-existing integer values are unchanged
  (`*_MAX` is now 7); the `RenderingServer` enum is bound, the `Viewport`
  property hint shows `pmndrs (Spatial - Fast):6`, and both enums are documented
  in `doc/classes/{Viewport,RenderingServer}.xml`. The old
  `rendering/scaling_3d/pmndrs_spatial` bool setting was **removed** — no dual
  control: mode 1 is real AMD FSR1 again, mode 6 is pmndrs.
  `renderer_scene_render_rd.cpp` selects PmndrsSpatial for mode 6 when the
  effect reports available, otherwise warns once and renders the slot through
  FSR1; `renderer_viewport.cpp` enforces the Forward+-only guard in the
  `Viewport` setter ("pmndrs spatial 3D scaling is only available when using
  the Forward+ renderer"), and the existing spatial fallbacks apply in the
  render-buffer config: render scale 1.0 → internally mode OFF (nothing runs),
  scale > 1.0 → bilinear, `gl_compatibility` → bilinear (generic
  compatibility fallback), upscaler unavailable → bilinear. The effect object
  is only created when the device has storage textures and
  `SUPPORTS_RAW_WGSL_COMPUTE`, so non-WebGPU RD drivers never instantiate it.
  Sharpness reuses `rendering/scaling_3d/fsr_sharpness` with pmndrs'
  `s = 1 - x/2` mapping; the destination must be a storage-capable RGBA16F
  texture (otherwise the effect warns once and skips the frame).
- **TPS settings-screen button** (external test project, not in this repo): the
  Settings screen's "Resolution Scale Filter" row gained a **"pmndrs Spatial"**
  button between "AMD FSR 1.0" and "AMD FSR 2.2" (the MetalFX buttons are hidden
  on non-Metal platforms). It is hidden unless
  `RenderingServer.get_current_rendering_driver_name() == "webgpu"`, restores
  from the saved `user://` settings, and **Apply** switches
  `window.scaling_3d_mode` live (no restart). Screenshot:
  `/tmp/pmndrs_button_settings.png`; harness
  `/tmp/node-test/tps_pmndrs_button.js`.
- **Patch bookkeeping**: the committed milestone-1 stack (`d8b3d583ae`) was
  `0004` +247/−21 and `0005` +1288/−32 patch-file lines (three new
  `effects/pmndrs_upscaler*` files plus `renderer_scene_render_rd.h` in the
  `0005` layer list). The first-class-mode commit (`91a0a3e8e4`) adds `0005`
  +174/−41 and **six existing paths** to the `0005` layer list (150 → 156):
  `scene/main/viewport.{h,cpp}`, `servers/rendering/rendering_server_enums.h`,
  `servers/rendering/storage/render_scene_buffers.cpp`,
  `doc/classes/{Viewport,RenderingServer}.xml`. `tools/make-patches.sh` marks
  untracked layer-listed files intent-to-add (`git add -N -f`) so new files
  (including `*.gen.*`-ignored names) land in the patches.

### Verification (2026-11-27)

**First-class scaling mode (latest build, 21:43-21:56):**

- **Builds/exports**: web template rebuilt 21:43
  (`/tmp/mode_impl_web_build.log` → `scons: done building targets.`,
  1m58.8s) and native editor 21:47 (`/tmp/mode_impl_macos_build.log` → done,
  3m11s) from the synced toolchain engine source; TPS export at 21:53
  (`/tmp/export_mode.log`, 227 s) → `/tmp/site-mode` and the Web Server site
  folder (21:53:27). Site/template `index.wasm`: 50,470,376 B, sha256
  `3b25ba0b…`; pck 987,358,704 B. Staged editor sha256 `be477327…`; template
  zip sha256 `076dad47…`.
- **Live TPS button run**: `/tmp/node-test/tps_pmndrs_button.js` (Chrome for
  Testing 158.0.8127.88/Dawn, headless, 960×540, TPS Forward+, render scale
  0.667, volumetric fog off) — SETTINGS → Resolution Scale Filter →
  "pmndrs Spatial" → Apply → PLAY:
  `[PMNDRS] spatial upscaler running (EASU+RCAS) 640x360 -> 960x540, 120x68
  groups`; `[PERF] fps=59-60 draws/f≈5.7-5.8k RingOF=0`; **0**
  `GPUValidationError`, **0** shader/SPIR-V errors, 0 FSR-fallback lines.
  Log `/tmp/pmndrs_button_console.log`; screenshots
  `/tmp/pmndrs_button_settings.png` (button visible) and
  `/tmp/pmndrs_button_game1.png`.
- **Default FSR1 on the same build** (fresh profile): only the
  `[PMNDRS] spatial upscaler active` availability line, **no** `running` line,
  0 errors, 59-60 fps — `/tmp/mode_fsr1_baseline_console.log`.

**Underlying effect A/B (first verification, 20:48-20:49, before the scaling
mode; kept as history):** `/tmp/node-test/pmndrs_spatial_test.js` with the old
bool setting on/off produced the same `[PMNDRS] … running 640x360 -> 960x540,
120x68 groups` line, 59-60 fps and 0 errors, with near-identical gameplay
screenshots (pmndrs slightly different sharpening than AMD FSR1); logs
`/tmp/pmndrs2_console.log` and `/tmp/fsr1b_console.log`, screenshots
`/tmp/pmndrs2_game1.png` and `/tmp/fsr1b_game1.png`.

**Round-trip**: the regenerated stack (milestone-1 + first-class scaling-mode
commits) applies to a pristine `67421f36e5` checkout
(`/tmp/applytest`) and re-diffs byte-identically per layer — all 8 layers,
`0005` = 5,936,829 B, `0005` layer list 156 paths (see the Environment
snapshot).

### Remaining limits / next steps

- **Spatial only**: no temporal history, jitter, depth/velocity, exposure or
  shading-change. The temporal path (milestone 2) was implemented and then
  reverted after severe ghosting — see the milestone-2 history section; a retry
  is milestone 2b.
- **Post-tonemap placement**: the effect lives in the spatial-upscaler slot
  (display-referred RGBA16F, sRGB-encoded SDR) while upstream documents the
  spatial path for pre-tonemap linear HDR; tone-mapping A/B is still to be done.
- **Forward+ only**, and it needs a render scale below 1.0: at scale 1.0 the
  viewport logic internally switches spatial modes OFF, so no upscale runs;
  scale > 1.0 falls back to bilinear, `gl_compatibility` has the generic
  bilinear fallback, setting mode 6 on another rendering method is rejected by
  the `Viewport` setter with a warning (previous mode kept), and on non-WebGPU
  RD drivers the renderer uses FSR1 in the same slot with one warning.
- Sharpness reuses `fsr_sharpness` with `s = 1 - x/2`; upstream's
  `sharpness == 0` blit fallback is not implemented (RCAS always runs).
- Alpha is filtered by EASU and passed through by RCAS (matches upstream);
  multiview/multi-viewport use is untested (the constants ring covers it, but
  no test yet). The "pmndrs Spatial" button itself is TPS-demo code (external
  test project), not part of the patch stack.
- **Perf A/B against FSR1 on the fixed-camera harness has not been run yet**
  (milestone 3); fps figures above are VSync-capped gameplay numbers.
- FSR1 remains the fallback everywhere the effect is unavailable; the AMD FSR2
  path is untouched.

## pmndrs/upscaler milestone 2 (temporal) — attempted then REVERTED (2026-11-28)

**Not shipped. The temporal path was implemented, then fully removed the same
day after the owner tested it and reported severe ghosting** ("the ghosting is
terrible"). It was reverted in full, not tuned.

**What was built (now removed).** Between 2026-11-28 ~00:00 and ~00:35 the
temporal upscaler was implemented as Viewport scaling mode 7
(`Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL`), running pre-tonemap in the FSR2
slot inside `RenderForwardClustered`: `RendererRD::PmndrsTemporal`
(`effects/pmndrs_temporal.{h,cpp}` + `pmndrs_temporal_wgsl.h`, 5 WGSL modules
from pmndrs/upscaler v0.6.0), a per-render-buffers context (history/locks/
exposure ping-pong, dilated depth/motion/masks, atomic depth-scatter buffers),
the raw-WGSL driver extensions it needed (sampled-texture sample-type
auto-layout and depth-only aspect views), and a second TPS settings-screen
button "pmndrs Temporal". It was exported and browser-verified in automation
(mode 7 log, ~60 fps, 0 validation errors, automated still/motion captures) and
committed as `cb0aa639c7` / tag `4.8-beta5` on 2026-11-28 ~01:00.

**Why it was reverted.** In the owner's real browser test the temporal output
ghosted badly. The first working slice shipped without the parts a temporal
upscaler needs to avoid ghosting, and there was no reference implementation to
compare against:

- no **reactive mask** → transparent/particle ghosting;
- no **shading-change detector** → animated-lighting ghosting;
- **sky/background ghosting under camera rotation** (zero motion vectors there);
- no **camera-cut reset signal** (history resets only on render-buffer
  reconfigure/resize/mode switch);
- verification was automated captures only — **no human Subjective review**
  before it was called done;
- FSR 2 is not usable as a WebGPU temporal reference, so there was no
  ground-truth A/B target.

**What the revert did (verified 2026-11-28 ~01:00-01:25).** Deleted
`effects/pmndrs_temporal.{h,cpp}` and `pmndrs_temporal_wgsl.h`; removed mode 7
and `VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL` from the RSE/Viewport enums,
Viewport/`RenderingServer` bindings and property hints, `renderer_viewport.cpp`,
`render_forward_clustered.{h,cpp}`, `renderer_scene_render_rd.cpp`,
`render_scene_buffers.cpp` and `doc/classes/{Viewport,RenderingServer}.xml`;
reverted the M2 raw-WGSL driver extensions (`wgsl_tex_sampled_with_sampler`
sample-type auto-layout, `_depth_only_view` depth-only views and the
`WGTexture::depth_view` field) in `drivers/webgpu/*`. Result: grep for
`PmndrsTemporal`/`PMNDRS_TEMPORAL`/`pmndrs_temporal` over `servers/ scene/
drivers/ platform/ modules/` → **0 hits**; mode 7 absent; `SCALING_3D_MODE_MAX`
= **7**; patch stack spatial-only (`0004` 891,250 B, `0005` 5,940,065 B, layer
list 159 paths, 0 temporal symbols in the patches; all 55/55 changed worktree
paths covered; round-trip all 8 patches byte-identical). The kept SDFGI
layer-completeness files came from the same regeneration and stay (see the
environment snapshot). The TPS demo was re-exported spatial-only (temporal
button and all `menu.gd`/`settings.gd` temporal references removed; a stale
saved `scale_filter >= SCALING_3D_MODE_MAX` is now sanitized back to FSR 1) and
the editor/template/site rebuilt from the reverted tree.

**Archived research (kept).** The two temporal design docs are retained because
they contain the full contract for any future retry, clearly marked
**IMPLEMENTED THEN REVERTED — NOT SHIPPED**:
`PMNDRS_TEMPORAL_PORT.md` (pass graph, vendored WGSL, constants, bindings,
historical as-built notes) and `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md`
(mode-7 engine seam, jitter/velocity/depth contracts, historical as-built notes).
`WGSL_COMPUTE_PATH.md` §11 (the raw-WGSL RD API) is still current; its §12 was
added for the M2 driver extensions and therefore now records reverted work —
treat it as historical until it is cleaned up (that file is outside this
handoff's edit scope).

**Milestone 2b (future retry) must cover, at minimum:** reactive mask;
shading-change pass; camera-cut reset; deterministic jitter/velocity sign-scale
validation; a ground-truth A/B harness (a captured temporal reference sequence,
since FSR 2 is not available on WebGPU); human Subjective ghosting review before
shipping; and the deferred passes/limitations recorded in the archived docs
(every-texel exposure, moments, debug view, linear-depth mode, split-frame,
blit fallback at `sharpness <= 0`). Multi-view/XR remains unsupported.

## WebGPU-native upscaler port (pmndrs/upscaler) — plan (milestone 1 done; temporal retry next)

**Goal:** replace/offer an alternative to the AMD FSR2 WebGPU path with
[`@pmndrs/upscaler`](https://github.com/pmndrs/upscaler), whose **engine-free core is
hand-written WGSL compute** and is designed around WebGPU's limits. This is a C++/Dawn
port, not an npm dependency: the package is TypeScript and our renderer is C++.

**Status (2026-11-28): milestone 1 (spatial path) is DONE and shipped. Milestone 2
(temporal path) was attempted, shipped briefly as mode 7, then fully reverted after
severe ghosting in the owner's browser test — it is NOT shipped; the next task is
milestone 2b (temporal retry) with the follow-ups listed in the milestone-2 history
section above. The archived contract lives in `PMNDRS_TEMPORAL_PORT.md` /
`GODOT_TEMPORAL_UPSCALER_INTEGRATION.md`.**

Why it is promising (from the repo's README, engine contract and architecture docs):

- `@pmndrs/upscaler/core` (`UpscalerCore`) takes a bare `GPUDevice`, plain inputs and a
  `FrameCamera`; no Three.js in the shaders. MIT licenced (AMD FSR EASU/RCAS derived,
  AMD notice included).
- It avoids exactly the features WebGPU lacks: **no storage-texture atomics**
  (reconstructed previous depth uses `atomicMin` on a storage *buffer*), **no
  subgroups**, **no f16 requirement**, and no B10G11R11 storage. Passes use 8×8
  workgroups. This sidesteps our current FSR2 fallbacks (`SPD_NO_WAVE_OPERATIONS`,
  `SPD_NO_IMAGE_ATOMICS`, reconstruct-depth load/store, B10G11R11→RGBA16F history
  promotion), which are the expensive part of the WebGPU FSR2 path.
- Its temporal pass graph is a fused FSR 2/3-style chain (reconstruct+dilate,
  exposure, shading-change, accumulate, RCAS) measured leaner than a literal FSR
  3.1.5 graph; the spatial path is a faithful EASU/RCAS WGSL port.
- It deliberately omits perf-only micro-optimizations (f16, tap packing, bind-group
  caching). We measured `shader-f16`/`subgroups` enabling on our current FSR2 and saw
  no gain, so expect the wins from the pass graph/shader structure, not from fp16.

### Integration shape

1. **Vendor the core shaders and constants layout. — DONE (milestone 1.)** Shaders are in
   `packages/core/src/shaders/` (one WGSL module per pass, assembled without
   `#include`); all passes share one 96-byte-payload `FsrConstants` UBO in a 256-byte
   buffer at `@group(0) @binding(0)`. The CPU writer
   (`packages/core/src/internal/ConstantsBuffer.ts`) must stay byte-identical to the
   WGSL layout. The spatial modules are vendored as C++ strings in
   `pmndrs_upscaler_wgsl.h` with provenance in `PMNDRS_UPSCALER_SPATIAL.md` §2.
2. **Add a raw-WGSL compute path to the WebGPU driver. — DONE (milestone 1), in a
   different shape than first recommended.** Rather than a private driver-level
   pipeline/dispatch API, the path is `RenderingDevice::shader_create_from_wgsl()`
   plus the WebGPU container factory `set_code_from_wgsl_compute()`; existing uniform
   sets, compute pipelines and compute lists are reused unchanged. See
   `WGSL_COMPUTE_PATH.md` §11 for the as-built API, the `@binding(N)`→`@binding(2*N)`
   convention and the rejected alternatives.
3. **Port `UpscalerCore` orchestration to C++. — Spatial part DONE (milestone 1);
   temporal part ATTEMPTED THEN REVERTED (milestone 2, retry = 2b).** The spatial
   graph (EASU → RCAS, constants write, ring, two dispatches) is
   `RendererRD::PmndrsSpatial`. The temporal graph — pass graph, ping-pong
   history, exposure, shading-change/reactive passes, `prepare()`/pipeline
   compilation, `configure()` reallocation, jitter, `encode()` — was implemented
   in a first working slice and then removed with the revert (it ghosted; the
   missing pieces are listed in the milestone-2 history section). Read the
   archived contract (`PMNDRS_TEMPORAL_PORT.md`) before retrying. The
   [engine contract](https://github.com/pmndrs/upscaler/blob/main/docs/engine-contract.md)
   is the spec; `examples/webgpu/hello` is the executable reference.
4. **Wire into the renderer. — DONE (milestone 1).** The spatial effect is an
   alternative `SpatialUpscaler` selected by
   `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` (mode 6) in
   `renderer_scene_render_rd.cpp`; the AMD FSR1 effect stays as the fallback.
   The FSR2 path is untouched.
5. **Make the core the single jitter source. — Attempted in milestone 2 (reverted);
   required for a retry.** `core.beginFrame()` returns this frame's offset (Halton
   sequence, render-pixel units, x right/y down; apply as `clip.x +=
   (-2x/renderWidth)*clip.w`, `clip.y += (2y/renderHeight)*clip.w`). The M2 slice
   reused the engine Halton jitter and mapped signs instead; deterministic
   jitter/velocity sign-scale validation is a required retry item. The history
   reconciles against exactly one sequence — do not mix sources.
6. **Inputs/outputs. — Milestone 1 uses only color in, RGBA16F storage out,
   post-tonemap. The M2 temporal slice used pre-tonemap color + hardware depth +
   motion vectors + a self-metered exposure, but shipped without a reactive mask
   or shading-change input; those are required retry items.** Temporal inputs:
   `color` rgba16f linear/HDR pre-tonemap; `depth` hardware depth (or `r32float`
   linear view distance with `depthMode: 'linear'`); `velocity` NDC
   current−previous, y up (use `motionScale` if our velocity convention differs);
   optional `reactive`, `reactiveOpaqueColor`, `exposureTexture`,
   `preExposureTexture`, `resetTexture`. `configure({displayWidth, displayHeight,
   qualityMode, path})` sets `renderWidth`/`renderHeight`; map our
   `resolution_scale`/FSR quality. Output is display-sized rgba16f; allocate it
   (and the guides) through a texture allocator returning our RD textures (require
   `TEXTURE_BINDING|STORAGE_BINDING`, one mip, one sample; r32float is not
   filterable). Submit the upscale after the scene pass and before presentation; the
   core can encode into our command encoder (queue order, one frame in flight).
7. **Camera facts. — Settled during the M2 attempt (kept for the retry): Godot 4.8
   Forward+ is reversed-Z, so `reversedDepth = true`.** `FrameCamera { near, far,
   perspective, projection, reversedDepth }`; `near`/`far` must be positive finite
   (no infinite far plane). Depth is hardware `[0,1]` (reversed-Z) or linear view
   distance in `r32float`; a mismatched input is an explicit dispatch error. The
   reversed-Z fact (and the correction to the archived doc's earlier standard-depth
   statement) is recorded in `PMNDRS_TEMPORAL_PORT.md` §5.6.

### Milestones

1. **Spatial path first** (`upscaleSpatial`, EASU/RCAS WGSL) — **DONE
   (2026-11-27)**: it runs as the first-class `pmndrs Spatial` scaling mode
   (`Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` = 6), browser-verified live from
   the TPS settings button at 21:54 with 0 validation errors and near-identical
   output to FSR1 (see the milestone-1 section). This also validated the
   raw-WGSL pipeline path with the smallest pass graph.
2. **Temporal path — ATTEMPTED, THEN REVERTED (milestone 2, 2026-11-28).**
   A first working slice (mode 7, FSR2 slot, reconstruct/depth-clip/exposure/
   accumulate/RCAS) was built and browser-verified in automation, but ghosted
   severely in the owner's browser test and was fully removed. **Retry =
   milestone 2b**, and it must add the missing pieces listed in the milestone-2
   history section (reactive mask, shading-change, camera-cut reset, deterministic
   jitter/velocity validation, ground-truth A/B harness, human review). The
   research contract is archived in `PMNDRS_TEMPORAL_PORT.md` /
   `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md`.
3. **Quality/perf A/B — PENDING (milestone 3)**: fixed camera and frozen level
   rotation (the TPS level rotates every 30 s — timing-based comparisons are
   invalid otherwise), equal `resolution_scale`/quality, measure the spatial mode
   vs FSR1 and the (future) temporal retry vs a captured reference, and check
   `GPUValidationError`/shader logs. The milestone-1/2 runs were VSync-capped at
   60 fps; the earlier fixed-camera harness measured the AMD FSR2 path at ~103 fps
   (uncapped) — historical anchor only, since FSR 2 is not usable on WebGPU.
4. **Keep the AMD paths as fallback — DONE**: FSR1 is the fallback whenever the
   pmndrs spatial effect is unavailable. FSR2 stays for devices that support it
   (it is not usable on WebGPU); the temporal slot is empty until a milestone-2b
   retry ships.

### Risks

- ~~New raw-WGSL pipeline infrastructure~~ — **landed and browser-verified in
  milestone 1** (see `WGSL_COMPUTE_PATH.md`); keep it isolated and covered by the
  binding/reflection checks when the temporal path adds more shaders/bindings.
- **Temporal ghosting is the #1 risk and it already killed milestone 2** (owner
  report: "the ghosting is terrible"). Do not ship a temporal retry without a
  reactive mask, a shading-change detector, camera-cut reset, a ground-truth A/B
  harness and human review. Jitter/velocity sign-scale errors silently double
  edges or smear history; validate them deterministically.
- Color-domain/placement: the spatial mode runs post-tonemap on display-referred
  RGBA16F; a temporal retry runs pre-tonemap like FSR2. The tone-mapping A/B is
  still open.
- Performance is unproven: milestone-1 runs were VSync-capped; the fixed-camera
  A/B against FSR1/FSR2 is milestone 3.
- The core is newer than the stable Three.js adapter; the spatial path is the
  smallest validated subset.
- MIT licence is compatible; the upstream MIT + AMD FidelityFX notices are
  already reproduced in `pmndrs_upscaler_wgsl.h` — keep them there.

References: [README](https://github.com/pmndrs/upscaler),
[engine contract](https://github.com/pmndrs/upscaler/blob/main/docs/engine-contract.md),
[architecture](https://github.com/pmndrs/upscaler/blob/main/docs/architecture.md),
[inputs and contracts](https://github.com/pmndrs/upscaler/blob/main/docs/inputs-and-contracts.md).
Local docs: `WGSL_COMPUTE_PATH.md` (raw-WGSL RD/driver path as built) and
`PMNDRS_UPSCALER_SPATIAL.md` (shipped spatial mode 6: vendored WGSL, constants,
bindings, integration plan). The temporal contract is archived in
`PMNDRS_TEMPORAL_PORT.md` and `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` —
**IMPLEMENTED THEN REVERTED, NOT SHIPPED**, kept for a future retry.

## Forward+ on WebGPU (2026-11-27)

Forward+ is the second rendering method running on the WebGPU device (after
Forward Mobile). Verified with the TPS demo exported through HTML5 Fast in a
browser (Chrome for Testing 158 / Dawn, headless + WebGPU): the level renders
fully (player, environment, shadows, reflections, HUD), the console shows
**0 WebGPU validation errors** and **0 SPIR-V→WGSL failures**, and the build
holds 60 fps (VSync) at ~5.8k draws/frame in-game. Startup counters for the TPS
Forward+ export (warm shader cache in the PCK): `shaders=360/1556/338ms`,
`tint=309/7878ms` (runtime translations of the non-baked stages),
`render_pipelines=338/7843ms`, `compute_pipelines=198/656ms`; gameplay
`[PERF] fps≈60 draws/f≈5.7k RingOF=0`.

What had to change (all landed in the `0004`/`0005` layers; regenerate the stack
from a tree that contains them):

- **Bake safety.** Tint's SPIR-V reader aborts (editor crash / browser wasm trap)
  on `gl_HelperInvocation`, `OpImageTexelPointer` (image atomics), 16-bit integer
  types, unknown decorations and width-changing integer converts.
  `spirv_preprocess::scan_tint_risks()` now pre-scans every baked stage and skips
  translation for such stages instead of killing the bake; the preprocessor also
  rewrites same-width `Op[US]Convert` to `OpBitcast` and strips the `Volatile`
  decoration. The eight stages that are still skipped are FSR2/FSR1 fp16
  variants which the WebGPU device never compiles.
- **Forward+ shaders.** The cluster builder uses plain atomics (no subgroup
  election and no `gl_HelperInvocation`); the clustered scene shader uses
  identity subgroup-reduction macros plus `NO_IMAGE_ATOMICS`; the volumetric fog
  shader lost a dead `shadow_sampler` binding.
- **Feature-variant gating.** `ShaderRD` compiles every *enabled* variant, and
  Tint aborts on fp16 arithmetic, so FSR1/FSR2 disable their fp16 variants when
  `SUPPORTS_HALF_FLOAT` is false. FSR2 is skipped entirely when B10G11R11 storage
  images are unsupported (WebGPU); the viewport falls back to FSR 1 when the 3D
  scale differs from 1.0, otherwise to no scaling (a no-op FSR 1 pass would read
  and write the same texture subresource, which WebGPU forbids).
- **Attachment-less passes.** WebGPU requires at least one color or depth
  attachment. The cluster builder gets a real colour attachment (the
  `USE_ATTACHMENT` shader variant); SDFGI's SDF depth pass is compiled against a
  colour-framebuffer format with an explicit colour pass on drivers without
  fragment-shader-with-only-side-effects support.
- **Driver (0004).** Combined sampler+texture layout entries record the required
  sampler binding type separately (`sampler_binding_types`); MSAA/depth samplers
  need `NonFiltering`, and Godot always supplies a filtering sampler RID, so the
  driver substitutes its dedicated non-filtering dummy. This also applies when a
  uniform set created for one shader variant is rebound to another
  (`_get_compatible_bind_group`). Fragment-stage `read_write` storage buffers are preserved
  (only the vertex stage is demoted); unused colour targets emit
  `WGPUTextureFormat_Undefined`; sampler LOD clamps are non-negative; WGSL
  component types drive sampled-texture `sampleType` (Uint/Sint); the storage
  texture format table covers `rgb10a2unorm`/`rgb10a2uint`; texture views inherit
  the physical (promoted) format; `D32_SFLOAT_S8_UINT` is gated on the
  `depth32float-stencil8` feature; 8-bit integer storage formats are accepted;
  dimension/sample-type-correct fallback textures replace invalid 3D↔2D views;
  and **compute passes are split when the next dispatch would use one texture
  subresource with conflicting writable+read usages** (WebGPU validates this per
  compute pass).
- **Shell.** `platform/web/js/engine/engine.js` requests the adapter limits
  Forward+ needs (`maxStorageBuffersPerShaderStage`, storage textures, compute
  workgroup limits, inter-stage variables, ...) plus `depth32float-stencil8`.

Forward+ **MSAA 4x and TAA are browser-verified** (0 validation errors, full
level, ~60 fps; MSAA `draws/f≈5.8k`, TAA `draws/f≈5.6k`, both `RingOF=0`). MSAA
needed the per-binding sampler-type fix described above (real-Chrome report:
`Filtering sampler ... is incompatible with non-filtering sampler binding` on
`ResolveShaderRD`). TAA on WebGPU uses an RGBA16F previous-velocity buffer and an
RGBA16F TAA-shader binding, because RG16F is not a WebGPU storage texture format
(the device would otherwise promote it to RG32F and break the copy-shader write
binding).

Remaining Forward+ limitations (documented, intentional):

- **SDFGI is enabled** on WebGPU (tag `529407b9fc`, 2026-11-27). Its packed UINT
  storage textures (`R32_UINT` for light/lightprobe data, `R16_UINT` for
  occlusion) were replaced by RGBA16F light-cascade/lightprobe storage+sample
  textures (`SDFGI_LIGHT_FLOAT`); the occlusion texture is a dummy float 3D
  binding with occlusion disabled; on attachment-only drivers the SDF pipelines
  are compiled against a colour framebuffer with an explicit colour pass; and
  the GI `MODE_SDFGI` binding needs the integer `voxel_gi_buffer` default plus
  the driver-side pin of binding 28 to the integer sample type (Tint drops that
  unused declaration). Browser-verified 2026-11-27 17:19 on the TPS Forward+
  export with volumetric fog off: 60 fps and 0 GPU validation/shader errors. The
  editor-vs-HTML5 palette difference seen earlier was **volumetric fog**, not
  SDFGI: the TPS demo now leaves volumetric fog off on WebGPU, and with fog off
  the HTML5 palette matches the native editor. Remaining differences: SDFGI
  occlusion is disabled, and the editor's packed light path is not
  bit-identical to the RGBA16F path. Volumetric fog, SSAO/SSIL/SSR and VoxelGI
  work as far as these TPS runs exercised them.
- **FSR 1 and FSR 2 are available** on WebGPU (verified 2026-11-27, 60 fps,
  0 validation errors); see the FSR section above.
- **Bake cost**: Forward+ bakes ~1524 shader-stage variants (mostly clustered
  scene variants), ~3-6 min on the dev machine, versus ~200 for Mobile. Skipped
  stages are harmless (the device never compiles them).

Historical note: the studio-foundation 4.7.1 stack (patches 0001-0033) was built
locally on 2026-11-26 for comparison and could not run TPS in Forward+ (Tint
traps on image atomics, vertex-stage storage-buffer limits, resolve layout
mismatches, and their bake missed scene materials). Those four classes are each
addressed in our stack as described above; the comparison document is kept in
this file's history.

## Open issues / next steps

1. ~~**MSAA flicker**~~ — **RESOLVED** (see the section above). Root cause was
   the WebGPU push-constant ring overflowing mid-frame; the flush broke the
   in-pass MSAA resolve. Fixed by an 8 MB ring (plus a one-shot warning).
2. **Push-constant ring flush — HARDENED 2026-11-26**, browser-checked by the
   owner ("the TPS export works fine").
   Flushes now happen only between passes (`_ring_pass_boundary()`), never from
   inside one, so a mid-frame submit can no longer interrupt an in-pass MSAA
   resolve. The whole-frame case that caused the flicker is fixed; residual is a
   single pass needing >32768 slots, which hits the old warned fallback
   (`RingOF`). Verify with `rendering/webgpu/debug_frames`: `passFlush` may be
   >0 on heavy frames, `ringOF` must stay 0.
3. **LightmapGI/GI probes**: komm64's 4.7.2 build had them working; the 4.8
   port has not been checked.
4. **Perf comparison**: 4.8 TPS stats - shaders=168/607/247ms, tint=370/13.7s,
   baked=280, spec_modules=788/27.5s, render_pipelines=409/34s,
   draws/frame ~1750, fps 60. The 4.7.2 stack is no longer buildable from this
   repo, and the archived table in `WEBGPU_DRIVER_MIGRATION.md` compares the
   komm64/4.7 driver, not the 4.7.2 stack — treat the 4.8 figures above as the
   baseline unless an old 4.7.2 build turns up.
5. The **4.7.2 stack is dropped** (2026-11-26): one toolchain, one engine line.
   No fixes are mirrored there; `patches/4.7.2` is frozen for history only
   (`origin/4.7.2` = `d8cb1fd77c`; a stale local `fresh` branch points there).
6. Bake-time noise: `Index p_pass out of bounds (draw_passes.size() = 1)` for
   particle shaders and a few leaked particle shader RIDs at editor exit; the
   exported build is unaffected so far.
7. `~/notarize-work/Fast_Godot.app` is an old 4.7.2 Developer-ID build with the
   stock icon; a Dock tile pinned to it shows the old icon. Either re-pin
   `~/Applications/Fast_Godot_4.8.app` or re-sign a refreshed copy.
8. ~~**Release/tag consistency**~~ — **FIXED, then RETIRED 2026-11-26.** The
   source asset was correct but the tag pointed at `d8cb1fd77c` (the 4.7.2
   tree); it was re-cut to a parentless snapshot of the `834c3d4fc2` tree
   (`35e42f7d`). Once the bootstrap stopped using it, the whole release was
   deleted (`gh release delete 4.8-beta5-source --cleanup-tag`), so neither
   ref nor asset exists on `origin` anymore. Only editors built before the new
   bootstrap used it.
9. **Toolchain bootstrap integrity**: the source step pins its inputs by commit
   and tag (`patches/upstream.lock`'s `godot_commit`, patch tag `4.8-beta5`) and applies
   them with system git, but nothing verifies the downloaded tarballs beyond
   HTTPS, and the patch tag is mutable (force-pushable). A silently moved tag
   changes what fresh installs build. Pinning the stack by commit inside the
   editor build is awkward (the commit containing the provisioner only exists
   after the build), so the practical guard is: never move the patch tag
   silently, and keep the provisioner validating the resulting tree.
10. **Forward+ on WebGPU — WORKING, with limits (2026-11-27).** TPS boots and
    renders the full level at ~60 fps with zero validation errors; MSAA 4x, TAA,
    FSR 1 and FSR 2 were verified too (see the Forward+ and FSR sections).
    **SDFGI is enabled on WebGPU** (tag `529407b9fc`; browser-verified
    2026-11-27 17:19 on the TPS Forward+ export with volumetric fog off: 60 fps,
    0 GPU validation errors, 0 shader errors). The packed formats were replaced
    with RGBA16F light-cascade/lightprobe storage+sample textures
    (`SDFGI_LIGHT_FLOAT`), the occlusion texture is replaced by a dummy float
    3D binding with occlusion disabled, the SDF pipelines on attachment-only
    drivers are compiled against a colour framebuffer with an explicit colour
    pass, and the GI process MODE_SDFGI binding needs an integer
    `voxel_gi_buffer` default plus a driver-side pin of binding 28 to the
    integer sample type (Tint drops that unused declaration). The visual
    difference seen earlier when comparing the native editor SDFGI with HTML5
    was **volumetric fog**, not SDFGI: the TPS demo now leaves volumetric fog
    off on WebGPU, and with fog off the HTML5 palette matches the native
    editor. Remaining differences: SDFGI occlusion is disabled, and the
    editor's packed light path is not bit-identical to the RGBA16F path.
    Forward+ bake is ~1524 stages (~3-6 min).
11. **MSAA/depth sampler bindings + TAA (2026-11-27, later) — FIXED.** A
    real-Chrome run with MSAA enabled failed bind-group creation on
    `ResolveShaderRD` (`Filtering sampler ... NonFiltering`); combined
    sampler+texture layouts stored only the texture half, so the driver now
    records `WGShader::sampler_binding_types` and enforces it at creation and in
    cross-shader rebinds. TAA's RG16F previous-velocity buffer (no WebGPU storage
    format) was changed to RGBA16F. Both re-verified with 0 validation errors.
12. The Forward+ / pmndrs exports are verified with the editor/template built
    from `/tmp/godot48` at HEAD `582d619026` plus the working-tree changes now
    captured in the regenerated patch stack. The current stack is the **revert
    stack** (spatial-only mode 6 + SDFGI layer-completeness fix; tag
    `4.8-beta5` force-moved from the temporal commit `cb0aa639c7` to it). The
    staged editor reports `4.8.beta.custom_build.582d61902` (reverted-tree build
    2026-11-28 00:52:28); the web template was rebuilt from the reverted tree
    and the site export written 00:54:36 runs that template's wasm byte-for-byte
    (`98ac9e82…`). If a distinct version stamp is wanted, commit the stack
    changes in the build tree and rebuild (see the
    `GODOT_VERSION_HASH_OVERRIDE`/`version_hash` note at the end).
13. **WebGPU-native upscaler (pmndrs/upscaler) — milestone 1 (spatial) DONE and
    shipped; milestone 2 (temporal) ATTEMPTED, GHOSTED, REVERTED; next task is
    milestone 2b (temporal retry).** The spatial EASU+RCAS path is the
    first-class `pmndrs Spatial` scaling mode (6), browser-verified 2026-11-27
    21:54 from the TPS settings button (0 validation errors, 59-60 fps). The
    temporal path shipped briefly as mode 7 and was fully removed on 2026-11-28
    after severe ghosting in the owner's browser test — see the milestone-2
    history section. **Milestone 2b must add:** a reactive mask, a
    shading-change pass, camera-cut reset, deterministic jitter/velocity
    sign-scale validation, a ground-truth A/B harness (captured temporal
    reference; FSR 2 is not available on WebGPU), human Subjective ghosting
    review before shipping, and the deferred passes/limits recorded in the
    archived research docs. Then the fixed-camera quality/perf A/B (milestone 3).

## Practical notes

- Never run two exports concurrently (they used to stampede the machine; now
  serialized by the tint-cli lock, but still).
- Building/staging inside Dropbox triggers FileProvider/Spotlight churn; keep
  build trees outside Dropbox (`/tmp` or `~`), stage the final app only.
- Tag pushes on `patches/*` were force-pushed historically (branch renamed
  from `4.7.2`/`4.8-beta5`); `.gitignore` on those branches must exclude `out/`
  or a 100 MB app binary gets committed and GitHub rejects the push.
- Release tags can be force-updated (one was re-cut and then removed on
  2026-11-26). Verify with `git ls-remote origin refs/tags/<tag>` after any move.
- The `4.8-beta5-source` release/asset was **removed 2026-11-26**. New builds
  bootstrap from Godot's repo + the `4.8-beta5` patch tag; editors built before
  the removal can no longer provision a fresh toolchain, so keep their existing
  toolchains or reinstall from a current build.
- **Keep the `4.8-beta5` tag pointing at the stack you ship**: fresh toolchain
  installs download it. It was moved on 2026-11-27 ~23:45 to `91a0a3e8e4`
  (raw-WGSL path + pmndrs milestone 1 + first-class pmndrs spatial mode),
  briefly to `cb0aa639c7` for the pmndrs temporal milestone 2, and then
  **force-moved on 2026-11-28 from `cb0aa639c7` to the revert stack commit**
  (spatial-only mode 6 + SDFGI layer-completeness fix) with the branch pushed
  and the docs refresh above it. The tag is lightweight; after any move,
  force-update the remote tag and re-check the archive
  (`git ls-remote origin refs/tags/4.8-beta5`), and keep the docs commit above
  it (docs-only changes don't affect template builds).
  First-use provisioning also needs system `git` on the machine.
- Layout (2026-11-26): the patch stack became a **self-contained portable
  bundle in `patches/`** (patch files + `layers/` + `upstream.lock`, no version
  subfolder), and this handoff + the migration history moved into `docs/` (the
  handoff is `docs/WEBGPU_HANDOFF.md` now). The bootstrap and the editor are
  coupled: editor builds before the layout change look for `patches/4.8-beta5/`
  (and `upstream.lock` at the archive root) and cannot bootstrap from the current
  tag; rebuild the editor whenever the bundle layout changes (current matching
  build: `4.8.beta.custom_build.582d61902`).
- **Adding files to a layer**: a new source file only lands in `patches/` if its
  path is listed in the matching `patches/layers/*.files`. `tools/make-patches.sh`
  marks every untracked layer-listed file intent-to-add before diffing
  (`git add -N -f`), so new files are emitted as full additions and the `-f`
  also covers names matched by `.gitignore` (`*.gen.*` etc.). The intent-to-add
  entries live only in the source tree's index, not in the working tree. The
  spatial work used this for the three `effects/pmndrs_upscaler*` files (plus
  `renderer_scene_render_rd.h` and six scaling-mode paths in `0005`, 150 → 156).
- **Layer completeness (important)**: the per-layer round-trip check only
  re-diffs the paths *listed* in `layers/*.files`, so it **cannot detect an
  omitted file**. Three SDFGI files (`environment/gi.h`,
  `shaders/environment/gi.glsl`, `shaders/environment/sdfgi_integrate.glsl`) had
  been missing from every layer list; they were added in the M2 regeneration and
  are **kept after the revert** (that is why the spatial-only `0005` layer list
  is 159 paths and `0005` is 5,940,065 B instead of the pre-M2 5,936,829 B).
  When regenerating, cross-check every `git -C /tmp/godot48 status` path against
  the union of all layer lists in addition to running the round-trip — after the
  revert that is **55/55** covered.
- **Round-trip check after regenerating**: apply the full `patches/` bundle to a
  pristine checkout of the pinned commit and re-diff each layer with its
  `layers/*.files` list; the 2026-11-28 revert stack round trip is
  byte-identical for all 8 layers (`0004` 891,250 B, `0005` 5,940,065 B; see the
  Environment snapshot).

Verified 2026-11-26: copying **only** `patches/` + `tools/apply.sh` and applying
them to a fresh Godot `67421f36e5` checkout reproduced all 16,064 worktree files
with zero mismatches, and the moved tag archive contains
`patches/{*.patch,layers/,upstream.lock}` with no root-level lock or layers.
- Ref naming (2026-11-26): the remote branch was renamed from `patches/4.8-beta5`
  to plain `4.8-beta5` (same name as the tag and the local branch; the patch
  files live at the repo root). Use full refs when the ambiguity matters.
  `4.8-beta5` is the repository **default branch** since 2026-11-26 (was
  `4.7.2`), so pushes to it no longer trigger GitHub's "recent pushes → compare
  & pull request" banner; `4.7.2` stays history-only.
- One SDK (Emscripten 6.0.12) serves everything; do not pin a second version.
- The macOS editor build tree `/tmp/godot48` also carries the ring mitigation,
  the pass-boundary ring flush, the white-box pack-key port, the raw-WGSL
  compute path, the pmndrs spatial effect (mode 6) and the three kept SDFGI
  layer-completeness files; the pmndrs **temporal** code was removed with the
  revert (see the milestone-2 history section). Re-apply the repo's
  `patches/*.patch` files to a fresh checkout to reproduce it.
- Ring diagnostics: `rendering/webgpu/debug_frames` prints `passFlush=` and
  `ringOF=` per frame; `passFlush` is normal on heavy frames, `ringOF` must stay
  0 (it now means a single pass exceeded the whole ring).
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
- After committing in `/tmp/godot48`, explicitly force `core/version_hash.gen.cpp`
  to regenerate (delete it and its object, rebuild, check `--version` shows the
  new HEAD). On 2026-11-26 a rebuild after committing kept the previous hash
  (`834c3d4fc`) until forced; a stale hash changes ShaderRD cache keys, so
  editor-baked shader caches would miss at runtime.
- **Milestone-2 revert note**: the pmndrs temporal path was implemented,
  briefly shipped as Viewport mode 7, and then **fully removed** on 2026-11-28
  after severe ghosting in the owner's browser test — do not re-enable or
  reference the temporal mode without doing milestone 2b properly. The two
  research docs are archived (banner: IMPLEMENTED THEN REVERTED — NOT SHIPPED);
  the driver is spatial-only mode 6. The TPS demo was re-exported spatial-only,
  so its settings screen has only the "pmndrs Spatial" button.
- **BUILD.txt caveat**: `out/editor-staging/Fast_Godot_4.8_BUILD.txt` is not
  part of `patches/`. It was refreshed for the revert build (00:56:07; editor
  sha `cd72898f…`, template `84202e5d…`), but its Site-export paragraph was not
  updated — it still describes the 00:28-00:36 M2 temporal export and mentions
  a "pmndrs Temporal" button. Trust the site row and the template/editor hashes
  in the Environment snapshot over that paragraph.
- **Browser checks:** the throwaway Playwright harness used for the Forward+
  verification lives outside the repo (`/tmp/node-test/`, Chrome for Testing at
  `/tmp/chrome-test/chrome-mac-arm64/`). It serves the site on a fresh port,
  captures console output, clicks PLAY, and counts `GPUValidationError` lines.
  The pmndrs A/B uses `/tmp/node-test/pmndrs_spatial_test.js`
  (`PORT=… OUT=/tmp/pmndrs2 node …`; logs/screenshots `<OUT>_console.log`,
  `<OUT>_menu.png`, `<OUT>_game1.png`, `<OUT>_game2.png`), and the
  first-class-mode button run uses `/tmp/node-test/tps_pmndrs_button.js`
  (SETTINGS → "pmndrs Spatial" → Apply → PLAY; screenshots
  `<OUT>_settings.png`, `<OUT>_menu_after_apply.png`, `<OUT>_game1.png`).
  Kill old `python3 -m http.server` instances before testing: a stale server on a
  reused port will happily serve an old wasm build and produce confusing results.
- **Site reload:** the Godot Web Server site folder is updated in place on every
  export, but browsers cache `index.wasm`/`index.pck`; always hard-reload
  (Cmd+Shift+R) or use a fresh profile before judging a new export.
- Demo graphics settings (MSAA/TAA/FSR mode, etc.) are saved into the browser's
  `user://` config by the TPS settings screen and persist across reloads — reset
  them from the demo menu when comparing runs. The scaling filter is part of
  that saved config: the demo's Settings → Resolution Scale Filter row now has
  the **"pmndrs Spatial"** button (WebGPU only) and **Apply** switches the mode
  live, so A/B runs can toggle it in the browser without a re-export; the TPS
  `scale_filter` is stored as the `Viewport.Scaling3DMode` value. The TPS demo
  now also **sanitizes a stale saved filter**: after the temporal revert a
  saved value `>= SCALING_3D_MODE_MAX` (e.g. the removed temporal mode 7) is
  reset to FSR 1 in `settings.gd`, and the menu's unknown-value fallback selects
  FSR 1 instead of FSR 2. The current site build is the reverted spatial-only
  export.
