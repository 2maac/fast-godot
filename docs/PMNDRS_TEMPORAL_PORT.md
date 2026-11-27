# pmndrs/upscaler temporal path — raw-WGSL port contract (Godot 4.8 / WebGPU)

Status: research + implementation contract for milestone 2 of the pmndrs/upscaler port
(the `temporal` path). Companion documents:

- `docs/PMNDRS_UPSCALER_SPATIAL.md` — milestone 1 (shipped): EASU + RCAS spatial path,
  the C++/RD integration pattern this document builds on.
- `docs/WGSL_COMPUTE_PATH.md` §11 — the raw-WGSL rendering-device API **as built**
  (`RD::WGSLShaderData`, `RD::shader_create_from_wgsl`, binding-number convention).
- `/tmp/pmndrs-vendor/PROVENANCE.md` — milestone 1's vendor tree (do not modify).

Vendored WGSL for this milestone: **`/tmp/pmndrs-vendor-temporal/`** (extractor
`tools/extract-shaders.mjs`, `manifest.json`, `PROVENANCE.md` there).

All upstream `file:line` references below are against the pinned checkout
`/tmp/pmndrs-upscaler` at commit `f6e52098edffb66e16298a30c53b4da37b4b7d7f`
(`v0.6.0`); engine references are against `/tmp/godot48` (Godot 4.8 WebGPU fork).
Every WGSL module named here exists in the vendor tree and was validated against the
upstream repo's own reviewed `BASELINE_FINGERPRINTS` (`packages/core/src/shaders/
shaders.test.ts`, FNV-1a 32-bit); 12 of 14 match a baseline exactly, the two generated
exposure-reduction modules are shape-checked against the test's pinned expectations.

---

## Status (2026-11-28): implemented + browser-verified — first working slice

Milestone 2 is shipped as the **first-class Viewport scaling mode 7**
(`Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL` /
`RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL` = 7, appended after
mode 6 so existing values are unchanged; `*_MAX` is now 8), classified
`VIEWPORT_SCALING_3D_TYPE_TEMPORAL`, with the mode hint
`pmndrs (Temporal - Slow):7`. It runs pre-tonemap in the FSR2 slot inside
`RenderForwardClustered` through `RendererRD::PmndrsTemporal` and the engine
seam documented in `GODOT_TEMPORAL_UPSCALER_INTEGRATION.md`. The effect and its
5 vendored WGSL modules live in
`servers/rendering/renderer_rd/effects/pmndrs_temporal{,_wgsl}.{h,cpp}`
(pinned upstream `f6e52098edffb66e16298a30c53b4da37b4b7d7f`, MIT + AMD
FidelityFX notices, per-file sha256).

**Implemented passes:** `reconstruct_hardware` (3×3 depth dilate +
`atomicMin` scatter), `depth_clip`, `exposure_taps` (internal 1×1 log-average
eye adaptation), `accumulate` (jitter-aware Lanczos2 + Catmull-Rom history +
YCoCg clamp + locks) and `rcas_temporal`.
**Deferred with hooks:** `generate_reactive`, `shading_change`, every-texel
exposure, debug view, linear-depth mode, moments, split-frame, and the
`sharpness <= 0` blit fallback (RCAS always runs).

**As-built deviations from this contract:**

- **Jitter ownership**: the effect does not own the jitter as §5.6/§2.6
  proposed. The engine's Halton jitter is reused; the renderer passes
  `scene_data->taa_jitter` / `prev_taa_jitter` × internal size × 0.5
  (FSR2-style render pixels) and the effect converts to its x-right/y-down
  convention as `(x, -y)`. `motionScale = (-1, -1)` for Godot's UV-delta
  prev−cur velocity. Deterministic jitter-sign/shift validation remains a
  recommended follow-up.
- **Depth**: `reversed_depth = true` — the §5.6 step 6 statement in the
  original contract was wrong; see the erratum below.
- **Exposure**: taps exposure (internal 1×1 log-average) is used, not the
  every-texel form; host `exposure` is an optional input (null RID in the TPS
  v1 path because it has no auto-exposure).
- **No reactive mask** on the WebGPU v1 path (`reactive = RID()`).
- **Engine slot/context**: FSR2 slot inside `RenderForwardClustered` with a
  per-render-buffers `PmndrsTemporalContext` (history/locks/exposure ping-pong,
  dilated depth/motion/masks, atomic depth-scatter buffers), ~32 B/display-px +
  ~28 B/render-px per viewport.
- **Driver extensions**: raw-WGSL sampled-texture sample-type auto-layout and
  depth-only aspect views were needed; they are documented in
  `WGSL_COMPUTE_PATH.md` §12.

**Verified 2026-11-28 00:28-00:35** on the TPS Forward+ WebGPU export: mode 7
selected through the TPS settings screen logs `[PMNDRS] temporal upscaler
running (reconstruct/clip/exposure/accumulate/RCAS) 640x360 -> 960x540, frame 0
(reset)`, ~60 fps, 0 GPU validation/shader errors; strong-motion captures show
no visible ghosting and the default FSR1 selection shows no temporal-running
line. Evidence: `/tmp/temporal_run2_console.log`,
`/tmp/temporal_move_console.log`, `/tmp/fsr1_move_console.log`,
`/tmp/ab_console.log`, `/tmp/temporal_run2_settings.png`,
`/tmp/temporal_move_{still,move}*.png`, harnesses
`/tmp/node-test/tps_pmndrs_temporal.js`, `tps_temporal_motion.js`,
`tps_temporal_ab.js`. Known limitations (reactive/shading-change/sky ghosts,
no camera-cut reset, single-view only, deferred passes) are listed in the
milestone-2 section of `WEBGPU_HANDOFF.md`.

---

## 0. Headline findings (read this first)

1. **The temporal path is 5–8 compute dispatches per frame, not a monolithic library.**
   Monolithic order: `reconstruct` → `depth clip` → *(generate reactive)* → `exposure` →
   *(shading change)* → `accumulate` → `debug | RCAS | blit`. The early two
   (`reconstruct`, `depth clip`) can be split into their own submit ("guides" stage).
   Dispatcher code: `UpscalerCore.ts:1231-1594` (`_encodeTemporal`, `_encodeGuides`,
   `_encodeLate`), `:1597-1617` (`_encodeRcas`).
2. **Every pass is `@compute @workgroup_size(8, 8) fn main`, one bind group at
   `@group(0)` (exposure-every-texel additionally uses `@group(1)`), constants at
   `@group(0) @binding(0)`.** Entry point is always `main` — except the generated
   every-texel exposure module, which has **two** entry points (`partials`, `combine`).
   That is the only multi-entry-point module in the graph.
3. **There is no luma SPD pyramid and no shading-change SPD pyramid.** Exposure is one
   8×8 dispatch (1024 bilinear taps) or a two-dispatch every-texel scalar reduction; the
   shading-change detector is one fused half-resolution dispatch. This is a measured,
   documented divergence from FSR 3.1.5 (`shaders/README.md`, "Reference pass graph" and
   the per-pass audit).
4. **The reconstructed-previous-depth scatter uses a storage buffer with `atomic<u32>`
   atomics** (`atomicMin`), ping-ponged with no clear pass (`reconstruct.ts:57-78`,
   `depth_clip.wgsl:29-37`). This is the first temporal feature that needs
   `RD::UNIFORM_TYPE_STORAGE_BUFFER` with `writable = true` through the raw-WGSL path.
5. **Two raw-WGSL driver gaps are on the critical path** (details §12):
   (a) sampled `texture_2d<f32>` bindings must resolve to WebGPU
   `unfilterable-float` when statically used only with `textureLoad` — the temporal path
   binds `r32float` dilated depth / luma history / shading signal, and WebGPU rejects a
   `float` BGL entry for those; (b) `texture_depth_2d` bindings of the engine's scene
   depth need a **depth-only view** (the WebGPU scene depth is a combined depth-stencil
   format, and the raw path currently binds the default aspect-all view).
6. **The exposure pass (`EveryTexelExposure`) is not one repo template.** It is WGSL
   generated by `@pmndrs/downsampler@0.1.1`'s `createScalarKernel()` from three
   repo-provided chunks; it needs **two bind groups** and **two entry points**. It is
   vendored mechanically (`exposure_every_texel.wgsl`, `exposure_every_quad.wgsl`) and
   fully specified in §8, but the first working version should use the single-entry
   `exposure_taps` form (1024 taps) instead (§14).
7. **No CPU readback is required.** Exposure stays on the GPU (1×1 `rgba16float`);
   the reset verdict can also stay on the GPU (`resetTexture`). The engine does not need
   to know the metered luma for correctness.
8. **The temporal path's color input must be scene-referred linear HDR *before* tone
   mapping**, and its output is linear HDR at display resolution. The milestone-1 spatial
   effect sits post-tonemap (display-referred); a temporal port must move to the FSR2-style
   slot instead (engine evidence in §11.1): `rb->get_internal_texture(v)` in,
   `rb->get_upscaled_texture(v)` out, then the engine's normal tonemap.
9. **The engine already renders jitter-free velocity and applies Halton jitter** for
   FSR2/TAA (`get_velocity_buffer`: `rg16float`; `scene_data.taa_jitter`; jitter phase count
   `8·ratio²`). These can be reused, but the **conventions differ** from what the upstream
   core produces (velocity sign/scale, jitter centering/indexing) — §5.6 gives the exact
   conversions and the failure modes. Getting them wrong shows up as blur/smear, never as
   a validation error.
10. **Memory is not free**: history + locks at display resolution, two each, are
   2 × 2 × 8 B/texel = 32 B/display-pixel, plus ≈44 B/render-pixel of working set —
    roughly 100 MB at 1080p output / 2× render (§6.4). That is the main new engine-side
   cost versus milestone 1.

---

## 1. Upstream pin and what the temporal path is

### 1.1 Pin

| field | value |
| --- | --- |
| repository | https://github.com/pmndrs/upscaler |
| tag | `v0.6.0` |
| pinned commit | `f6e52098edffb66e16298a30c53b4da37b4b7d7f` (`release: v0.6.0 [skip ci]`) |
| clone used | `git clone --depth 1 https://github.com/pmndrs/upscaler.git /tmp/pmndrs-upscaler` |
| core package | `packages/core` (private, version `0.0.0`, MIT) |
| exposure reducer dependency | `@pmndrs/downsampler@0.1.1`, integrity `sha512-WIeAeJn4CAfDpZaXo0yXyrrqF8tehcpnzVJUPX//S+V1r5+7FcqxZp+4PQsU3Rphoxg0/3z4x2aBY2T44us1sg==` (`pnpm-lock.yaml:378-379`) |

Re-verify:

```sh
git -C /tmp/pmndrs-upscaler rev-parse HEAD
# f6e52098edffb66e16298a30c53b4da37b4b7d7f
```

### 1.2 What the upstream `temporal` path is (and is not)

`UpscalerCore` (`packages/core/src/UpscalerCore.ts`, 1936 lines) owns the whole graph on
a caller-supplied `GPUDevice`. `upscale({ path: 'temporal' })`:

- requires `color` + `depth` + `velocity` (`UpscalerCore.ts:1236-1238`);
- keeps **two constants uniform buffers** per frame, one per stage (`_constantsEarly`,
  `_constantsLate`, `:277-278`, written at `:978-979`, `:1068`, `:1136`);
- keeps history/locks/exposure/shading state and ping-pong indices (`:347-422`);
- dispatches the graph (§2) and has no CPU readbacks.

It is **not** a full FSR 3.1.5 resolver: the local graph deliberately replaces FSR's
SPD luma pyramid, SPD shading-change pyramid, prepare-inputs pass and source lock
semantics with fused local stages. `packages/core/src/shaders/README.md` ("Temporal
pipeline vs. FSR Upscaler 3.1.5") audits each divergence; a C++ port must reproduce the
**local** graph, not AMD's, because the vendored WGSL is the local one.

---

## 2. Pass graph and execution order

### 2.1 Monolithic frame (`dispatch()` / `encode()`)

All dispatches for one frame, in encode order (`_encodeTemporal` :1231-1249 →
`_encodeGuides` :1254-1325 → `_encodeLate` :1367-1594):

| # | pass | vendored WGSL | grid | writes | reads | required? |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | reconstruct (dilate + scatter) | `reconstruct_hardware.wgsl` **or** `reconstruct_linear.wgsl` | render | `dilatedDepth[cur]`, `dilatedMotion`, `scatter[cur]` | scene depth, velocity, constants(early) | **mandatory** |
| 2 | depth clip (disocclusion + scatter clear) | `depth_clip.wgsl` | render | `masks`, `scatter[next]` (cleared) | `dilatedDepth[cur]`, `dilatedMotion`, `scatter[cur]`, resetSignal, constants(early) | **mandatory** |
| 3 | generate reactive | `generate_reactive.wgsl` | render | `reactiveGenerated` | `reactiveOpaqueColor`, `color`, incoming reactive | **conditional**: only when `reactiveOpaqueColor` is supplied |
| 4 | exposure | `exposure_taps.wgsl` **or** `exposure_every_texel.wgsl` (2 dispatches) | 1×1 | `exposure[cur]` | `color`, `exposure[prev]`, external exposure, host pre-exposure, resetSignal | **mandatory** (temporal) |
| 5 | shading change | `shading_change.wgsl` | ⌈render/2⌉ | `shadingLumaOut`, `shadingSignal`, `shadingBlockMemory[out]` | `color`, `shadingLumaIn`, `dilatedMotion`, exposure cur/prev, `masks`, `shadingBlockMemory[in]` | **conditional**: `settings.detectShadingChanges` **and** pipeline ready |
| 6 | accumulate | `accumulate.wgsl` | display | `history[out]`, `locks[out]` | `color`, `dilatedMotion`, `masks`, `history[in]`, `locks[in]`, exposure cur, reactive view, exposure prev, shading signal (or dummy) | **mandatory** (core) |
| 7 | output (exactly one of) | `debug.wgsl` / `rcas_temporal.wgsl` / `blit_temporal.wgsl` | display | output | history[out], locks[out]/alpha, exposure cur, + pass-specific | **mandatory**: debug iff `debugView != None`; else RCAS iff `sharpness > 0`; else blit |

Not dispatches in this graph: `moments.wgsl` (standalone `MomentsCore`, §10.4),
`easu.wgsl` (`spatial` path only), `reconstructVariants.ts` (`bench`-only candidates;
not production, not vendored).

### 2.2 Split frame (guides early / upscale late)

The same two halves are exposed separately so that SSGI/SSR/denoiser-style consumers can
read the geometry guides before the beauty color exists (`docs/temporal-guides.md`):

| stage | encodes | ends with |
| --- | --- | --- |
| early `dispatchGuides()` / `encodeGuides()` | passes **1–2** only (constants **early**) | `_guidesPending = true` |
| late `dispatchUpscale()` / `encodeUpscale()` | passes **3–7** (constants **late**) | ping-pong flips, `_frameIndex++` |

`dispatch()` = early + late in one encoder and one submit (`_encodeTemporal`). The split
requires no extra state except `_guidesPending` and the retained `_frameResetSignal`
(`:1273`). Ping-pong flips happen once, at `_endGuides` for `guides` path, else at
`_endUpscale` (`:1071-1081`, `:1139-1146`).

Published early products: `dilatedMotion` (display? no — render), `dilatedDepth`,
`previousDepth`, `disocclusion`. Published late products: `reactive`, `shadingChange`,
`exposure`, `lockStatus`, `history` (previous-frame priors).

### 2.3 Ordering constraints (why this order)

- 1 must precede 2 because the scatter must be complete for every pixel before any pixel
  reads it (separate dispatch + barrier; `reconstruct.ts:82-87`).
- 3 (reactive) and 5 (shading change) must precede 6 (`accumulate` reads their outputs).
- 4 (exposure) must precede 5, 6, 7: it writes this frame's conditioning exposure and,
  crucially, the **resolved GPU reset** into `exposure.a`, which shading change and
  accumulate read.
- 7 must be last: it converts conditioned tonemap-space history to the caller's
  linear/HDR domain and writes the only externally visible output.

### 2.4 Fused / absent relative to FSR 3.1.5

| FSR 3.1.5 stage | local implementation |
| --- | --- |
| prepare inputs (dilated motion/depth, reactive merge) | fused into reconstruct/depth-clip + optional generate-reactive pass |
| luma SPD pyramid | replaced by one 8×8-dispatch exposure (or the every-texel reduction) |
| shading-change SPD + resolve | fused into one half-resolution detector dispatch |
| prepare reactivity | generate-reactive pass only when `reactiveOpaqueColor` is given; incoming mask max-merged |
| luma instability | fused into accumulate (locks + shading-change aging) |
| accumulate | `accumulate.wgsl` (Lanczos2 upsample, Catmull-Rom history, YCoCg AABB, locks, alpha) |
| optional RCAS | `rcas_temporal.wgsl`, or `blit_temporal.wgsl` at `sharpness == 0` |

---

## 3. Vendored WGSL and provenance

Vendor tree: **`/tmp/pmndrs-vendor-temporal/`** — `wgsl/` (assembled modules),
`upstream/` (verbatim TS + docs + licences), `tools/extract-shaders.mjs`,
`manifest.json`, `PROVENANCE.md`. Milestone 1's `/tmp/pmndrs-vendor/` is untouched.

### 3.1 Assembled modules (the shader deliverables)

SHA-256 prefixes below are of the vendored files; full values in `manifest.json` and
`PROVENANCE.md`. "baseline" = upstream `shaders.test.ts` `BASELINE_FINGERPRINTS` match.

| file | bytes | sha256 (prefix) | upstream FNV-1a | fingerprint status | used by |
| --- | --- | --- | --- | --- | --- |
| `wgsl/reconstruct_hardware.wgsl` | 5497 | `a88e522c…` | `193dec98` | **OK (baseline)** | pass 1, `depthMode: 'hardware'` |
| `wgsl/reconstruct_linear.wgsl` | 5265 | `391666a6…` | `6add6913` | **OK (baseline)** | pass 1, `depthMode: 'linear'` |
| `wgsl/depth_clip.wgsl` | 6017 | `66313508…` | `4300b381` | **OK (baseline)** | pass 2 |
| `wgsl/generate_reactive.wgsl` | 3031 | `39faefa8…` | `9d0739e5` | **OK (baseline)** | pass 3 |
| `wgsl/exposure_taps.wgsl` | 10046 | `18b1125b…` | `b2d0d970` | **OK (baseline)** | pass 4 (production alternative) |
| `wgsl/exposure_taps_serial.wgsl` | 7806 | `76f6cd6d…` | `a1719dab` | **OK (baseline)** | frozen bench identity only |
| `wgsl/exposure_every_texel.wgsl` | 10741 | `2bb5d709…` | — | shape-checked (generated) | pass 4, formal production form |
| `wgsl/exposure_every_quad.wgsl` | 10893 | `6d6648d3…` | — | shape-checked (generated) | pass 4, bench-only form |
| `wgsl/shading_change.wgsl` | 17969 | `33710add…` | `9de820c9` | **OK (baseline)** | pass 5 |
| `wgsl/accumulate.wgsl` | 22313 | `2e36ab1f…` | `733e241f` | **OK (baseline)** | pass 6 |
| `wgsl/rcas_temporal.wgsl` | 7937 | `e68ac0f9…` | `b0405308` | **OK (baseline)** | pass 7 (sharpness > 0) |
| `wgsl/blit_temporal.wgsl` | 2771 | `0b13bfa5…` | `ef2eec52` | **OK (baseline)** | pass 7 (sharpness == 0) |
| `wgsl/debug.wgsl` | 4756 | `4c99e9ad…` | `e30ebd6c` | **OK (baseline)** | pass 7 (debug view) |
| `wgsl/moments.wgsl` | 3915 | `0ca4d75a…` | `ec1952d4` | **OK (baseline)** | standalone `MomentsCore`, **not** in the temporal graph |

Cross-checks: `rcas_temporal.wgsl` is **byte-identical** to milestone 1's
`wgsl/rcas_spatial.wgsl` (same sha256 `e68ac0f9…`), and `blit_temporal.wgsl` is
byte-identical to milestone 1's `wgsl/blit.wgsl` (`0b13bfa5…`): the temporal path reuses
the production conditioned-input RCAS and blit unchanged. `easu.wgsl` is not needed by
the temporal path and is not duplicated in this tree.

### 3.2 How the modules were extracted (mechanical, reproducible)

Upstream has **no `.wgsl` files**: WGSL lives in TypeScript template strings assembled by
`assembleShader()` (`shaders/wgsl.ts`: trim chunks, deduplicate by trimmed string
identity, join with `\n\n`, append `\n`). `tools/extract-shaders.mjs` (extended from the
milestone-1 extractor):

1. strips the single-line imports and `export` keywords from the 11 shader TS modules;
2. applies the handful of TS-type fix-ups the small subset needs (each fix-up asserts it
   found exactly one occurrence, so upstream drift fails loudly);
3. evaluates `common`, `wgsl`, `reconstruct`, `accumulate`, `blit`, `rcas`, `debug`,
   `generateReactive`, `luminancePyramid`, `moments`, `shadingChange` in **one scope** in
   dependency order (top-level `const` collisions would throw);
4. writes each exported module and validates it against the upstream repo's reviewed
   FNV-1a fingerprints;
5. for the every-texel exposure, imports the **pinned** `@pmndrs/downsampler@0.1.1`
   `dist/webgpu/scalar.js` and calls `createScalarKernel()` with the repo's own
   `EXPOSURE_EVERY_*_BINDINGS` / `..._LOAD` / `exposureEveryTexelFinish()` chunks,
   transcribed from `internal/EveryTexelExposure.ts`; it then re-checks the generated
   module against the shape assertions in `shaders.test.ts:617-645` (two entry points,
   `DsParams` layout, group-0 bindings 0/1, exact `origin = group.xy * 64u + local.xy`);
6. runs structural checks on every module (one 8×8 `main`, constants at binding 0, no
   `enable`/`f16`/subgroups, balanced braces/parens) and writes `manifest.json`.

Reproduce (Node 26; the tarball can be fetched with `npm pack @pmndrs/downsampler@0.1.1`
and verified against the `pnpm-lock.yaml` integrity):

```sh
node /tmp/pmndrs-vendor-temporal/tools/extract-shaders.mjs \
  /tmp/pmndrs-upscaler /tmp/pmndrs-vendor-temporal/wgsl /tmp/downsample-pkg/package
# → 14 modules written; fingerprints OK.
```

### 3.3 Licence / attribution (must ship with the engine copy)

Upstream is MIT (`Copyright (c) 2026 Dennis Smolek`). The EASU/RCAS shaders are derived
from AMD FidelityFX (`ffx_fsr1.h`) and the temporal pipeline follows FSR 2/3's
architecture: `Copyright (c) 2021-2023 Advanced Micro Devices, Inc.`, MIT. Full notices
are in `/tmp/pmndrs-vendor-temporal/upstream/LICENSE` (upstream `LICENSE`) and
`upstream/downsampler_LICENSE` (the generator used to produce the two every-texel modules,
also MIT). Keep the AMD attribution in the engine copy; do not strip headers.

### 3.4 What is not vendored (and why)

- `reconstructVariants.ts` (`RECONSTRUCT_CROSS_FRAME_SHADER`,
  `RECONSTRUCT_CAMERA_SHADER`, `DEPTH_CLIP_VARIANTS`): bench-only A/B candidates;
  production is `RECONSTRUCT_*` + `DEPTH_CLIP`. `UpscalerCore` exposes them only via
  `UpscalerCoreInternalOptions` (`:160-195`).
- RCAS experiment/legacy variants (`RCAS_LEGACY_SHADER`, `RCAS_PER_TAP_SHADER`,
  `RCAS_HOISTED_EXPOSURE_SHADER`, `RCAS_TONEMAP_SPACE_SHADER`): production temporal RCAS
  is `RCAS_SHADER` (`rcas.ts:314`), already vendored.
- `easu.ts`: spatial path only.
- `packages/three|babylon|pixi`: engine adapters, not used by a C++/Dawn port.
- `shaders.test.ts`: reference only (its fingerprints are reproduced in `manifest.json`).

---

## 4. Constants buffers

### 4.1 Buffer contract

- Every pass binds one 256-byte uniform buffer as `@group(0) @binding(0)` (the generated
  every-texel exposure binds it as `@group(1) @binding(0)`), declared as
  `var<uniform> C : FsrConstants`.
- The struct payload is **96 bytes**; the GPU allocation is **256 bytes**
  (`ConstantsBuffer.SIZE = 256`, `ConstantsBuffer.ts:11-13`). WGSL layout: `common.ts:30-49`;
  CPU writer: `internal/ConstantsBuffer.ts:38-100`.
- The core keeps **two buffers per frame**, `_constantsEarly` (guides stage:
  reconstruct + depth clip) and `_constantsLate` (everything after), each written once
  per frame (`UpscalerCore.ts:277-278`, `:507-508`). Both have the same layout and are
  written with the same values in a monolithic frame; keeping them separate lets a split
  frame's two stages live in one command encoder without the late write racing the early
  dispatches (`queue.writeBuffer` lands before the encoder runs).
- The engine effect must use a **ring** of these buffers because `process()` is called
  per view and multiple frames' recordings can be in flight before one submit
  (milestone-1 pattern `pmndrs_upscaler.h` `CONSTANTS_RING_SIZE = 64`;
  `docs/PMNDRS_UPSCALER_SPATIAL.md` §9.6). For temporal: two stage buffers × a ring.
- `exposureTaps` (offset 92) is written **once at buffer creation** by the core
  (`ConstantsBuffer.ts:27-28`, `_u32[23] = 32`) and never changed. The shared
  `WGSL_CONSTANTS` chunk names it `_pad`; only the exposure shaders rename it to
  `exposureTaps` (`luminancePyramid.ts:4-7`) — same byte, same layout, so one buffer type
  works for every pass.

### 4.2 `FsrConstants` byte layout (all passes; the one and only layout)

| offset | size | WGSL field | C++ equivalent | meaning / who reads it |
| --- | --- | --- | --- | --- |
| 0 | 8 | `renderSize: vec2f` | `float render_size[2]` | jittered render resolution in px; reconstruct/depth-clip/generate-reactive/shading-change guards; accumulate upsample; reconstruct scatter/bilinear math |
| 8 | 8 | `displaySize: vec2f` | `float display_size[2]` | output resolution; accumulate guard + Catmull-Rom; rcas/blit/debug guard |
| 16 | 8 | `renderSizeInv: vec2f` | `float render_size_inv[2]` | 1/renderSize; reconstruct prevUV; depth clip uv; shading-change reprojection |
| 24 | 8 | `displaySizeInv: vec2f` | `float display_size_inv[2]` | 1/displaySize; accumulate uv; rcas/blit/debug uv |
| 32 | 8 | `jitter: vec2f` | `float jitter[2]` | current sub-pixel offset, render px, x right / y down. accumulate `srcPos`; shading-change jitter-delta compensation |
| 40 | 8 | `jitterPrev: vec2f` | `float jitter_prev[2]` | previous frame's offset. shading-change only |
| 48 | 8 | `motionScale: vec2f` | `float motion_scale[2]` | NDC velocity → UV delta. reconstruct and depth clip only |
| 56 | 8 | `depthNearFar: vec2f` | `float depth_near_far[2]` | camera near, far (positive, finite). reconstruct hardware + linear fallback |
| 64 | 4 | `sharpness: f32` | `float sharpness` | RCAS attenuation 0..1 (1 = sharpest); `peak = exp2(-2·(1-sharpness))` |
| 68 | 4 | `maxAccumulation: f32` | `float max_accumulation` | history age cap in frames; accumulate |
| 72 | 4 | `exposure: f32` | `float exposure` | fixed conditioning exposure, used when `FLAG_AUTO_EXPOSURE` clear |
| 76 | 4 | `deltaTime: f32` | `float delta_time` | seconds; auto-exposure adaptation rate |
| 80 | 4 | `flags: u32` | `uint32_t flags` | `FLAG_*` bits, §4.4 |
| 84 | 4 | `frameIndex: u32` | `uint32_t frame_index` | completed temporal frames since configure. **Written but no temporal shader reads it** (upstream writers only) |
| 88 | 4 | `debugMode: u32` | `uint32_t debug_mode` | `DebugView` enum; debug pass only |
| 92 | 4 | `_pad: u32` / `exposureTaps: u32` | `uint32_t exposure_taps` | **must be 32**; only exposure taps/serial read it (`y < C.exposureTaps`) |
| **96** | | struct end | | 96-byte payload; 160 bytes of the 256-byte allocation are padding |

Uniform layout rules: WGSL std140-like rules make this exactly 96 bytes (vec2f align 8,
max align 8 → size rounds to 96). Do **not** use `alignas(16)`, `bool`, or `size_t` in the
C++ mirror; `static_assert(sizeof(FsrConstants) == 96)`.

### 4.3 Which passes bind which constants buffer

| pass | group/binding | buffer | notes |
| --- | --- | --- | --- |
| reconstruct | `@group(0) @binding(0)` | **early** | jitter fields unused here |
| depth clip | `@group(0) @binding(0)` | **early** | reads `renderSize`, `renderSizeInv`, `flags(FLAG_RESET)` |
| generate reactive | `@group(0) @binding(0)` | late | reads `renderSize` only |
| exposure (taps / serial) | `@group(0) @binding(0)` | late | reads `displaySize`, `deltaTime`, `exposure`, `flags`, `exposureTaps` |
| exposure (every-texel/quad) | `@group(1) @binding(0)` | late | same fields; `@group(0) @binding(0)` is `DsParams` (§4.6, dispatched in §8.4) |
| shading change | `@group(0) @binding(0)` | late | reads sizes, jitter/jitterPrev, flags |
| accumulate | `@group(0) @binding(0)` | late | reads render/display sizes+inverses, jitter, sharpness? no — **not sharpness**; `maxAccumulation`, `flags` |
| RCAS | `@group(0) @binding(0)` | late | `displaySize`, `sharpness`, flags |
| blit | `@group(0) @binding(0)` | late | `displaySize`, `displaySizeInv`, flags |
| debug | `@group(0) @binding(0)` | late | sizes, `debugMode` |
| moments (standalone, its own buffer) | `@group(0) @binding(0)` | own | `renderSize`, `flags(FLAG_MOMENTS_YCOCG)` |

### 4.4 Flag bits — CPU encoding and per-shader consumers

Constants: `common.ts:9-27`; encoder: `UpscalerCore._baseFlags`/`_writeConstants`
`:1643-1698`.

| bit | value | name | set by the core when | read by |
| --- | --- | --- | --- | --- |
| 0 | 1 | `FLAG_RESET` | `_pendingReset` (configure/resetHistory/first frame) **or** `inputs.reset` | depth clip (all-1 disocclusion), exposure (snap, `.a=1`), shading change (neutral compare + memory restart), accumulate (drop history) |
| 1 | 2 | `FLAG_REVERSED_DEPTH` | `FrameCamera.reversedDepth` | reconstruct hardware (via `linearizeDepth`) |
| 2 | 4 | `FLAG_PERSPECTIVE` | camera is perspective (`projection[15]==0`) | reconstruct hardware (via `linearizeDepth`) |
| 3 | 8 | `FLAG_INPUT_REINHARD` | always on `temporal` (`:1685`) | RCAS (`rcasLoad` conditioned branch + single inversion), blit (invert tonemap+exposure) |
| 4 | 16 | `FLAG_INPUT_DISPLAY` | **never set by any path** (dead bit; reserved) | nowhere in the temporal graph |
| 5 | 32 | `FLAG_LOCKS` | `settings.lockThinFeatures` (default true) | accumulate (lock grow/decay/boost/alpha) |
| 6 | 64 | `FLAG_AUTO_EXPOSURE` | `settings.autoExposure` (default true) | exposure publish (use metered vs fixed), accumulate (conditioning-exposure history correction only when off/external) |
| 7 | 128 | `FLAG_SHADING_CHANGE` | `settings.detectShadingChanges` and **not** a prime frame | accumulate (read shading signal) |
| 8 | 256 | `FLAG_REACTIVE` | `inputs.reactive` or `inputs.reactiveOpaqueColor` present | accumulate (read reactive mask) |
| 9 | 512 | `FLAG_RCAS_DENOISE` | `settings.rcasDenoise` | RCAS (lobe attenuation on lone outliers) |
| 10 | 1024 | `FLAG_EXTERNAL_EXPOSURE` | `inputs.exposureTexture` present | exposure (app value overrides both), accumulate (history correction) |
| 11 | 2048 | `FLAG_MOMENTS_YCOCG` | moments pass config (own buffer) | `moments.wgsl` only — **not** in `WGSL_CONSTANTS` |
| 12 | 4096 | `FLAG_SHADING_PRIME` | first frame after the detector turns on (`optionalPassTransition`, `:225-230`) | `shading_change.wgsl` only (declared locally) — seeds luma history + block memory and publishes 0 |

Note on the split frame: the early constants are built from `GuideDispatchInputs`, so
`FLAG_REACTIVE`/`FLAG_EXTERNAL_EXPOSURE` are **not** set in the early buffer; the early
passes do not read them. `FLAG_RESET` is set in both because `_pendingReset` is sticky
until the frame ends.

### 4.5 Values the core supplies (exact)

From `_writeConstants` (`UpscalerCore.ts:1653-1698`):

```
renderSize  = (renderWidth, renderHeight)            // integer pixels as float
displaySize = (displayWidth, displayHeight)
renderSizeInv = 1/renderSize, displaySizeInv = 1/displaySize
jitter      = JitterSequence.current (render px, x right / y down)
jitterPrev  = JitterSequence.previous
motionScale = [0.5, -0.5] default, or the adapter's motionScale option
depthNearFar = (camera.near, camera.far)             // positive, finite; camera.far must be finite
sharpness   = clamp(settings.sharpness, 0, 1)
maxAccumulation = max(1, settings.maxAccumulation)   // default 24
exposure    = settings.exposure                      // default 1.0; ignored while autoExposure
deltaTime   = inputs.deltaTime ?? 1/60
flags       = bits §4.4
frameIndex  = completed temporal frames
debugMode   = settings.debugView (0 == None)         // only when the debug pass is active
exposureTaps = 32                                     // written once at buffer creation
```

`isPerspective(camera)` reads `camera.perspective` when present, else `projection[15] == 0`
(column-major: 0 for perspective, 1 for orthographic). Defaults of `RuntimeSettings`
(`UpscalerCore.ts:430-439`): `sharpness 0.8`, `rcasDenoise false`,
`maxAccumulation 24`, `exposure 1.0`, `autoExposure true`, `lockThinFeatures true`,
`detectShadingChanges true`, `debugView None`.

### 4.6 `DsParams` — the every-texel exposure uniform (`@group(0) @binding(0)`)

Only for the generated exposure reduction. 32 bytes, six `u32` words + 2 zero words
(`EveryTexelExposure.ts:38-43,87-92`; `scalarParamsBytes = 32`):

| offset | field (WGSL) | value |
| --- | --- | --- |
| 0 | `size: vec2u` (`x`,`y`) | metered grid width,height = render size (**texel form**) or `ceil(render/2)` (**quad form**) |
| 8 | `firstLayer: u32` | `0` |
| 12 | `groupsX: u32` | `ceil(width / 64)` (`TILE = 16·4 = 64`) |
| 16 | `partialCount: u32` | `groupsX · groupsY` |
| 20 | `count: u32` | `width · height` (the divisor for the `average` channel) |
| 24 | `_pad: vec2u` | zero |

Dispatch: `partials` with `(groupsX, groupsY, 1)`, then `combine` with `(1,1,1)` —
both into the same compute pass (`EveryTexelExposure.ts:259-280`).

---

## 5. Jitter contract

### 5.1 Sequence and phase count (CPU-owned, exact)

Source: `packages/core/src/math/halton.ts`, `math/jitter.ts`.

```ts
halton(index, base): radical inverse, (0,1)          // index 0 → 0
generateJitterSequence(count) = for i in 1..count: [halton(i,2)-0.5, halton(i,3)-0.5]
getJitterPhaseCount(ratio) = max(1, round(8 · ratio²))   // ffxFsr2GetJitterPhaseCount
```

Values: ratio 1 → 8, 1.5 → 18, 1.7 → 23, 2 → 32, 3 → 72 phases. Offsets are in **render
pixels**, each axis in `[-0.5, 0.5]`, **x right, y down**: the sample for render texel
`(i, j)` sits at `(i + 0.5 + x, j + 0.5 + y)` in the unjittered image.

`JitterSequence` (`jitter.ts:30-100`): an index into the precomputed array.
`beginFrame()`: if the previous frame began but never dispatched, `retreat()`; then
`advance()`; then `current` is read. Consequence: the **first temporal frame after
`configure()` uses phase 1** (array index 1), `jitterPrevious` is phase 0, and sequence index 0
(`(halton(1,2)-0.5, halton(1,3)-0.5)` = `(0, −1/6)`) closes each cycle. `resetHistory()` restarts the sequence
(`_jitter.reset()` sets index 0, so the next `beginFrame()` again uses phase 1).

`configure()` also calls `_jitter.setRatio(ratio)` → rebuild sequence for the new ratio.

### 5.2 Applying the jitter (clip space)

`docs/engine-contract.md` §Jitter, `UpscalerCore.ts:758-767`:

```wgsl
// after projection, before rasterization; clip y is up in WebGPU
clip.x += (-2.0 * jitter.x / renderWidth)  * clip.w;
clip.y += ( 2.0 * jitter.y / renderHeight) * clip.w;
```

Equivalently a translation by `(-2·jx/renderWidth, 2·jy/renderHeight)` pre-multiplied onto
the projection matrix (that is exactly `camera.setViewOffset(w,h,jx,jy,w,h)` in three).
The input color/depth/velocity must be rendered under this offset **every frame**;
the shaders reconcile history against the recorded `C.jitter`/`C.jitterPrev`.

### 5.3 How passes consume jitter / jitterPrev

| pass | formula | notes |
| --- | --- | --- |
| reconstruct | none (scatter uses motion-only `prevUV`) | comment `reconstruct.ts:58-60`: jitter delta is a common offset, cancels between scatter and gather |
| depth clip | none (`prevUV = uv - uvDelta`) | same-footprint gather; test asserts `jitterPrev` is **not** referenced |
| shading change | `previousUv = (uv - motion) + (C.jitter - C.jitterPrev)·renderSizeInv` | explicit jitter-delta alignment (`shading_change.wgsl:157`) |
| accumulate | `srcPos = uv·renderSize - 0.5 - C.jitter` | current-frame Lanczos2 sampling; history reprojection uses motion only |
| exposure / RCAS / blit / debug | none | display-space / 1×1 passes |

So: the CPU must only record the two offsets correctly; no pass needs the renderer to
"undo" jitter anywhere else.

### 5.4 Velocity must be jitter-free

Motion vectors must describe only real motion (`docs/inputs-and-contracts.md` §Velocity):
compute both endpoints with the **unjittered** projections. If the jitter leaks into
velocity, every static pixel reports motion and history lands on the wrong texels —
the failure mode is smearing/blur, not a validation error.

### 5.5 Held frames (no-dispatch frames)

A frame "counts" once `dispatch`/`encode`/`dispatchGuides`/`encodeGuides` runs. If a
`beginFrame()` is never followed by one, the next `beginFrame()` steps the sequence back
before advancing, so held frames consume no jitter phase and `jitterPrevious` stays the
phase the history was rendered with (`UpscalerCore.ts:919-925`,
`docs/engine-contract.md` §Held frames). A C++ port must replicate this if it can render
a scene but skip the upscale dispatch in the same frame; otherwise phase and history
desynchronize.

### 5.6 Mapping the Godot engine's existing jitter/velocity plumbing (important)

Godot 4.8 already has the pieces a temporal upscaler needs, because it ships FSR2/TAA:

- **Velocity**: rendered when `using_upscaling` (`render_forward_clustered.cpp:1928`),
  stored in an `rg16float` (`R16G16_SFLOAT`) velocity buffer
  (`render_scene_buffers_rd.cpp:866-867`), allocated texturable+attachment
  (`ensure_velocity`, `:731-740`). The fragment shader writes
  `motion_vector = prev_position_uv - position_uv` with the jitter subtracted from both
  endpoints (`scene_forward_clustered.glsl:3216-3224`), i.e. **UV delta previous −
  current, jitter-free**. The depth-derived fallback (`motion_vector_inc.glsl`) uses
  `previousUV − uv` as well.
- **Jitter**: `RendererSceneCull::render_camera` builds a Halton(2,3) array
  (`get_halton_value` maps to `[-1,1)`, `renderer_scene_cull.cpp:54-63`) with
  `jitter_phase_count = 8·(target/render)²` (`renderer_viewport.cpp:302-313`), sets
  `jitter = camera_jitter_array[frame_number % phase_count] / viewport_size`
  (`:2729`), and `scene_data.taa_jitter` is pre-multiplied onto the projection as a clip
  translation (`render_scene_data_rd.cpp:43-47,63-67`, `Projection::add_jitter_offset`,
  `projection.cpp:1044-1047`).
- **Integration point/color domain**: FSR2 runs **before** tonemapping, on the
  render-resolution HDR internal texture, into the display-resolution upscaled texture
  (`render_forward_clustered.cpp:2573-2615`, then `Tonemap` at `:2659`). The spatial
  path runs after tonemapping (milestone-1 doc §9.1). A temporal pmndrs port belongs in
  the FSR2-style slot.

Exact conversions if the engine's plumbing is reused (recommended; keeps velocity
jitter-free for free because the forward shader subtracts `taa_jitter` and the previous
taa_jitter before differencing):

1. **Phase count**: use `round(8·ratio²)` (the engine already computes
   `8·(target/render)²`, but truncates with a `uint32_t` cast — use `ROUND` to match
   `getJitterPhaseCount`; at integer ratios it is identical).
2. **Sequence**: replace the engine sequence for this mode with
   `(halton(i,2)-0.5, halton(i,3)-0.5)`, `i = 1..phaseCount`, indexed by the engine's
   frame counter as the upstream core does (phase 1 first, phase 0 last). The engine's
   current sequence is `2·halton(i)-1` and starts at index 0 = `(-1,-1)`; it is a
   different sequence, so do not mix the two.
3. **Applied offset**: the upstream `C.jitter` (render pixels) relates to Godot's
   clip-space `taa_jitter` by

   ```
   taa_jitter.x = -2·jx / renderWidth
   taa_jitter.y =  2·jy / renderHeight        (clip y is up; jitter y is down)
   ```

   Feed that as `taa_jitter`; then `C.jitter = (jx, jy)` is the offset actually applied,
   and velocity stays jitter-free.
4. **Velocity → `motionScale`**: the engine's velocity buffer is consumed as a
   **texture-UV delta (y-down)**, proven by the engine's own TAA resolve:
   `uv_reprojected = uv + velocity` with `uv = pos_screen/resolution`
   (`taa_resolve.glsl:316-325`), i.e. `V_g = prevUV_tex − curUV_tex`. The upstream core
   instead expects `V = curNDC − prevNDC` and computes `uvDelta = V · motionScale`,
   `prevUV = uv − uvDelta`. Matching the two:

   ```
   motionScale = (-1, -1)      // prevUV = uv − V_g·(-1,-1) = uv + V_g = prevUV
   ```

   If the engine instead supplies raw NDC or render-pixel velocity (another producer),
   scale accordingly — the single knob is `C.motionScale`, and the acceptance test is
   the debug pass (`DebugView.MotionVectors`) or a known camera pan: static pixels →
   exactly zero motion, pan → smooth gradient, no per-object noise. **Verify the sign
   and scale empirically; do not trust the derivation alone** (a wrong sign is silent:
   it doubles edges / smears instead of failing validation).
5. **Do not double-jitter**: if the pmndrs temporal mode takes over the jitter (option
   3), the engine must not add any other view offset for that viewport (no TAA, no
   engine-side FSR2).
6. **Depth convention — CORRECTED 2026-11-28 (erratum):** ~~Godot 4.8 renders
   standard depth (`[0,1]`, 0 = near, 1 = far); it is not reversed-Z. So pass
   `reversedDepth = false`, `perspective = !camera->is_orthogonal()`,
   `near`/`far` from the camera, and leave `FLAG_REVERSED_DEPTH` clear.~~
   **This was wrong.** Godot 4.8 Forward+ is **reversed-Z** (`[0,1]`, 1 = near,
   0 = far): the projection's `set_depth_correction` defaults to
   `p_reverse_z = true`, and the engine's FSR2 path enables
   `FFX_FSR2_ENABLE_DEPTH_INVERTED` on the same depth texture. The milestone-2
   implementation therefore passes `reversed_depth = true`,
   `perspective = !camera->is_orthogonal()`, `near`/`far` from the camera, and
   sets `FLAG_REVERSED_DEPTH`. (The `set_depth_correction(true, true, false)`
   in the FSR2 path is a reprojection matrix convention, not evidence about the
   depth buffer.)

---

## 6. Resource inventory, formats, persistence

### 6.1 Textures (all `TEXTURE_BINDING | STORAGE_BINDING` unless noted; 1 sample, 1 mip)

| name | size | format | written by | read by | persistence |
| --- | --- | --- | --- | --- | --- |
| `output` | display | `rgba16float` | pass 7 | engine (sampled later) | reallocated by configure; +`COPY_SRC` on the core allocator |
| `history[2]` | display | `rgba16float` | accumulate (storage write) | accumulate (`texture_2d<f32>` sampled with linear sampler + `textureLoad`), debug | **persists across frames** (ping-pong) |
| `locks[2]` | display | `rgba16float` | accumulate (storage write) | accumulate (linear-sampled), RCAS/blit alpha source (`textureLoad` / sampled), debug | persists |
| `exposure[2]` | 1×1 | `rgba16float` | exposure (storage write) | accumulate (b9/b11), shading change (b4/b5), RCAS/blit (b2/b3), debug | persists |
| `dilatedDepth[2]` | render | `r32float` | reconstruct (storage write) | depth clip, debug (`textureLoad` only) | persists (ping-pong; also a published guide) |
| `dilatedMotion` | render | `rgba16float` | reconstruct (storage write) | depth clip, shading change, accumulate, debug | frame-local (re-written every frame) |
| `masks` (disocclusion) | render | `rgba8unorm` | depth clip (storage write) | accumulate, shading change, debug | frame-local |
| `reactiveGenerated` | render | `rgba8unorm` | generate reactive | accumulate, debug | frame-local |
| reactive dummy | 1×1 | `r8unorm` | — (zero-init) | any pass binding a "no mask"/"no reset" input | constant; sampled-only usage |
| `shadingLuma[2]` | render | `r32float` | shading change (storage write) | shading change (`textureLoad`) | persists |
| `shadingSignal` | ⌈render/2⌉ | `r32float` | shading change (storage write) | accumulate (`textureLoad`), debug | frame-local |
| `shadingBlockMemory[2]` | ⌈rw/4⌉ × (⌈rh/4⌉+⌈rh/8⌉) | `rgba32uint` | shading change (storage write), engine `queue.writeTexture` to zero it when stale | shading change (`texture_2d<u32>`, `textureLoad`) | persists; +`COPY_DST` |
| `scatter[2]` (buffers) | rw·rh·4 B | storage buffer, `array<atomic<u32>>` | reconstruct `atomicMin`; depth clip clears to `0x7f800000` | depth clip (`array<u32>`) | persists; initial content **+inf bits** |
| `constantsEarly`, `constantsLate` | 256 B each | uniform buffer | CPU per frame | passes | per-frame stages (ring in the engine) |
| `DsParams` (every-texel only) | 32 B | uniform buffer | CPU per configure/size | generated exposure partials/combine | per size |
| `ds_partials` (every-texel only) | groupsX·groupsY·16 B | storage buffer (`array<vec4f>`, read_write) | exposure partials | exposure combine | per size |

Notes:

- `r32float` is **not filterable**: it must be bound with a WebGPU
  `unfilterable-float` BGL sample type (see §12.3 gap A) and never with a filtering
  sampler; the shaders only `textureLoad` those.
- The scatter buffer holds `f32` bits and the encoding relies on positive floats ordering
  like their `u32` bits (`reconstruct.ts:71-78`); empty slots are `+inf` bits
  (`0x7f800000`). It has no clear dispatch: the depth clip empties the *other* buffer
  each frame (`depth_clip.wgsl:29-37`).
- WebGPU zero-initializes all textures/buffers, but the scatter buffers must be
  initialized to `0x7f800000`, not zero: `mappedAtCreation` fill upstream
  (`UpscalerCore.ts:1786-1799`), `storage_buffer_create(size, data)` in RD.
- The first frame after `configure()` is a reset frame, so history/locks/shading-memory
  initial contents are never trusted.

### 6.2 Ping-pong / index rules (replicate exactly)

State in the upstream core (`UpscalerCore.ts:328-384`):

| index | initial | flips | selects |
| --- | --- | --- | --- |
| `_historyIndex` | 0 | end of each temporal frame (`_endFrame` / `_endUpscale`, `:1008`, `:1144`) | `history[in]=[_historyIndex]`, `history[out]=[1-index]`; same for `locks`; same index also selects `exposure[prev]/[cur]` and shading luma/memory in/out |
| `_depthIndex` | 0 | end of each temporal frame (`_endFrame`, `:1009`; `guides` path `_endGuides`, `:1077`) | `dilatedDepth[cur]` vs `[prev]` |
| `_scatterIndex` | 0 | start of the guides stage (`_encodeGuides`, `:1285`) | scatter target vs "next" buffer to clear |
| `_latestDepthWrite`, `_latestHistoryWrite` | 0 | set when encoding | guides getters resolve "most recently written" half |
| `_frameIndex` | 0 | end of each temporal frame | `C.frameIndex` |

`exposure[prev]` is deliberately the *same* index as `history[in]` (they ping-pong
together). `_pendingReset`, `_shadingPrime`, `_frameResetSignal` are reset at frame end.

### 6.3 Frame-end state updates

`_endFrame()` (`:1003-1011`): `frameIndex++`, `pendingReset = false`,
`frameResetSignal = null`, flip `historyIndex`/`depthIndex` (temporal only).
`_endGuides()` for the `guides` path does the same without history/locks.
`_endUpscale()` (`:1139-1146`) does the split-frame variant.

### 6.4 Memory footprint (planning)

Per frame the working set is:

```
display-res:   output 8 + history 16 + locks 16                = 40 B / display pixel
render-res:    dilatedDepth 8 + motion 8 + masks 4 + reactive 4
             + shadingLuma 8 + shadingSignal ~1 + blockMemory ~3
             + scatter 8                                        ≈ 44 B / render pixel
```

At 1920×1080 output with ratio 2 (960×540 render): ≈ 83 MB + 23 MB ≈ **106 MB**, plus the
small 1×1 textures. At 1280×720 / ratio 2: ≈ 37 MB + 10 MB ≈ 47 MB. `locks` and
`history` dominate; they are the price of the upstream engine-agnostic design (FSR2 packs
lock state differently). Treat this as a port risk on mobile WebGPU (§13).

---

## 7. Per-pass contracts

Conventions used in this section:

- "groups" = `compute_list_dispatch(list, ceil(gridX/8), ceil(gridY/8), 1)` for every
  8×8 pass (the generated exposure reduction is the exception; §8.4).
- Binding numbers are the **authored WGSL numbers** (`@binding(N)`). The raw-WGSL
  container rewrites them to `@binding(2N)` internally; `RD::WGSLUniform::binding` and
  `RD::Uniform::binding` use the authored numbers (`docs/WGSL_COMPUTE_PATH.md` §11.2).
- "sample type" is the WebGPU BGL texture sample type the raw path must produce; see
  §12.3 gap A for how the driver learns it.
- `RD::Uniform` rows are copied to Appendix A as a consolidated table.
- Tuning constants are baked into the WGSL and are part of the fingerprint — do not edit
  the vendored files. They are listed so an implementer can reason about behaviour.

### 7.1 reconstruct — dilate + reconstructed-previous-depth scatter

- **Files**: `wgsl/reconstruct_hardware.wgsl` (`depthMode: 'hardware'`, default) or
  `wgsl/reconstruct_linear.wgsl` (`depthMode: 'linear'`; identical except binding 1's
  type and the depth linearization, generated from the production body by exact-count
  substitutions upstream — `reconstruct.ts:149-184`).
- **Grid**: render size. **Workgroup** 8×8. **Dispatch** `ceil(rw/8) × ceil(rh/8) × 1`.
- **Writes**: `dilatedDepth[cur]` (linear view depth, r32float), `dilatedMotion`
  (`xy` = UV delta, `z` = 3×3 depth relief in view units — reserved channel used by the
  depth clip, `w` unused), `scatter[cur]` (atomic min `u32` = f32 bits of
  `max(curDepth, 0)`).
- **Behaviour**: 3×3 nearest-depth dilation (reversed-depth aware) selects the closest
  texel's motion vector so thin foreground silhouettes drag their own motion; the
  selected depth is linearized (`linearizeDepth`, see `common.ts:113-126`); the
  bilinear footprint of the motion-only previous position receives the scattered depth
  via `atomicMin` with a weight floor `DEPTH_TAP_WEIGHT_FLOOR = 6.1e-4`. The
  scatter/gather pair uses motion-only reprojection so the jitter delta cancels.

| b | WGSL declaration | kind | format / sample type | usage | RD type / format / writable |
| --- | --- | --- | --- | --- | --- |
| 0 | `var<uniform> C : FsrConstants` | uniform buffer | 96 B payload (256 B allocation) | UNIFORM | `UNIFORM_TYPE_UNIFORM_BUFFER`, length 96 |
| 1 | `var sceneDepth : texture_depth_2d` | sampled depth | depth32float / depth24plus(-stencil8), sampleType `depth` | SAMPLING + DEPTH_STENCIL_ATTACHMENT | `UNIFORM_TYPE_TEXTURE`, `TEXTURE_TYPE_2D`, `DATA_FORMAT_D32_SFLOAT` (or engine depth format); **requires a depth-only view on combined formats, §12.3 gap B** |
| — | *(linear mode alternative)* `var sceneDepth : texture_2d<f32>` | sampled texture | `r32float` (recommended) / `r16float` / any float; sampleType `unfilterable-float` (`textureLoad` only) | SAMPLING | `UNIFORM_TYPE_TEXTURE`, 2D, `DATA_FORMAT_R32_SFLOAT` |
| 2 | `var sceneVelocity : texture_2d<f32>` | sampled texture | `rg16float` (engine velocity) or `rgba16float`; sampleType `unfilterable-float` (`textureLoad` only) | SAMPLING | `UNIFORM_TYPE_TEXTURE`, 2D, `DATA_FORMAT_R16G16_SFLOAT` |
| 3 | `var dilatedDepth : texture_storage_2d<r32float, write>` | storage image | `r32float`, write-only | STORAGE + SAMPLING (published guide) | `UNIFORM_TYPE_IMAGE`, 2D, `DATA_FORMAT_R32_SFLOAT`, **writable = true** |
| 4 | `var dilatedMotion : texture_storage_2d<rgba16float, write>` | storage image | `rgba16float`, write-only | STORAGE + SAMPLING | `UNIFORM_TYPE_IMAGE`, 2D, `DATA_FORMAT_R16G16B16A16_SFLOAT`, writable |
| 5 | `var<storage, read_write> reconstructedDepth : array<atomic<u32>>` | storage buffer | `rw·rh·4` bytes, u32 = f32 bits; empty = `0x7f800000` | STORAGE | `UNIFORM_TYPE_STORAGE_BUFFER`, length 0 (RD skips exact size), **writable = true** |

### 7.2 depth clip — disocclusion + next-scatter clear

- **File**: `wgsl/depth_clip.wgsl`. **Grid**: render size, 8×8, same dispatch as 7.1.
- **Writes**: `masks` (rgba8unorm; `.r` = disocclusion 0 stable … 1 fresh; `1` for
  off-screen and for every pixel on a reset frame), `scatter[next]` cleared to +inf bits.
- **Behaviour**: 4 bilinear taps at the motion-only previous position vote a confidence;
  tolerance is
  `max(1.37e-5 · |renderSize/2| · max(cur,tap), localRelief)` (AMD's viewport/depth
  scaled quantization term widened by the 3×3 depth relief); a tap at/behind the current
  surface is full confidence; the best tap wins (max vote, not a weighted mean);
  `disocclusion = 1 − max(confidence)`. Empty slots read as +inf (farther), no veto.
  The ping-pong clear runs unconditionally before the off-screen early return.
  `DEPTH_SEPARATION_CONSTANT = 1.37e-5`, `DEPTH_TAP_WEIGHT_FLOOR = 6.1e-4`,
  `EMPTY_DEPTH = 0x7f800000`.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var dilatedDepth : texture_2d<f32>` | sampled | `r32float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, `DATA_FORMAT_R32_SFLOAT` |
| 2 | `var dilatedMotion : texture_2d<f32>` | sampled | `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, `DATA_FORMAT_R16G16B16A16_SFLOAT` |
| 3 | `var<storage, read> reconstructedDepth : array<u32>` | storage buffer | `rw·rh·4` B | `UNIFORM_TYPE_STORAGE_BUFFER`, length 0, writable **false** |
| 4 | `var maskOutput : texture_storage_2d<rgba8unorm, write>` | storage image | `rgba8unorm`, write-only | `UNIFORM_TYPE_IMAGE`, 2D, `DATA_FORMAT_R8G8B8A8_UNORM`, writable |
| 5 | `var<storage, read_write> nextReconstructedDepth : array<u32>` | storage buffer | `rw·rh·4` B | `UNIFORM_TYPE_STORAGE_BUFFER`, length 0, writable **true** |
| 6 | `var resetSignal : texture_2d<f32>` | sampled | any float, typically 1×1; `.r > 0.5` = reset; zero dummy when absent; `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D (or the shared 1×1 r8unorm dummy) |

### 7.3 generate reactive (conditional)

- **File**: `wgsl/generate_reactive.wgsl`. **Grid**: render size, 8×8.
- **Dispatch condition**: `inputs.reactiveOpaqueColor` present. Without it, an explicit
  `inputs.reactive` is bound directly to accumulate and this pass does not run; with
  neither, the 1×1 zero dummy is bound and `FLAG_REACTIVE` is clear.
- **Writes**: `reactiveGenerated` (rgba8unorm, `.r`), max-merged with any incoming mask.
- **Behaviour**: `diff = max(|final−opaque|)` per channel, `reactivity =
  clamp((diff − 0.04)·2.0, 0, 0.9)` then `max(generated, clamp(incoming,0,1))`
  (`REACTIVE_THRESHOLD 0.04`, `REACTIVE_SCALE 2.0`, `REACTIVE_MAX 0.9`).

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var opaqueColor : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var finalColor : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 3 | `var reactiveOut : texture_storage_2d<rgba8unorm, write>` | storage image | `rgba8unorm`, write-only | `UNIFORM_TYPE_IMAGE`, 2D, `DATA_FORMAT_R8G8B8A8_UNORM`, writable |
| 4 | `var incomingReactive : texture_2d<f32>` | sampled | render-res or 1×1 dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |

### 7.4 exposure — two interchangeable forms (see §8 for the deep dive)

**Form A — taps (`exposure_taps.wgsl`, recommended for the first version).**
Grid: exactly **one** workgroup (dispatch `(1,1,1)`; the core calls
`ComputePass.dispatch(pass, group, 8, 8)` with workgroup size 8). It takes 1024 bilinear
taps (32×32 grid) across the frame in all 64 lanes and reduces them in workgroup memory;
when `FLAG_AUTO_EXPOSURE` is clear or `FLAG_EXTERNAL_EXPOSURE` is set it does **no
metering** and lane 0 publishes the provided exposure instead (issue #98 form).
`exposure_taps_serial.wgsl` is the frozen single-lane predecessor for bench identity only.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B (offset 92 = `exposureTaps` = 32) | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var inputColor : texture_2d<f32>` | sampled | render `rgba16float`; **sampleType `float`** (used with a filtering sampler) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var linearSampler : sampler` | sampler | filtering, clamp-to-edge | `UNIFORM_TYPE_SAMPLER` |
| 3 | `var prevExposure : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 4 | `var exposureOut : texture_storage_2d<rgba16float, write>` | storage image | 1×1 `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |
| 5 | `var externalExposure : texture_2d<f32>` | sampled | app 1×1 or zero dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |
| 6 | `var hostPreExposure : texture_2d<f32>` | sampled | app 1×1 or zero dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |
| 7 | `var resetSignal : texture_2d<f32>` | sampled | GPU reset signal or zero dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |

**Form B — every-texel (`exposure_every_texel.wgsl` / `exposure_every_quad.wgsl`).**
Production upstream for renders ≤ 2^19 texels (≈960×540); two dispatches, **two bind
groups**, **two entry points**. Full spec in §8. It supersedes Form A only when the
device has ≥256-invocation workgroups and ≥2 bind groups and the size gate passes; any
failure falls back to Form A for the lifetime of the instance (`UpscalerCore.ts:536-583`).

### 7.5 shading change (conditional)

- **File**: `wgsl/shading_change.wgsl`. **Grid**: half render resolution, one invocation
  per 2×2 render block. **Dispatch**:
  `groups = ceil(ceil(rw/2)/8) × ceil(ceil(rh/2)/8) × 1` (equivalently `ceil(rw/16) ×
  ceil(rh/16)`); the core calls `ComputePass.dispatch(pass, …, max(1,ceil(rw/2)),
  max(1,ceil(rh/2)))` (`UpscalerCore.ts:1515-1521`).
- **Dispatch condition**: `settings.detectShadingChanges` and the pass is compiled
  (lazy optional pipeline). First frame after enabling uses `FLAG_SHADING_PRIME`
  instead of `FLAG_SHADING_CHANGE`: seeds luma history + block memory and publishes 0.
- **Writes**: `shadingLumaOut` (r32float, render res, host-domain luma for the next
  frame), `shadingSignal` (r32float, ⌈render/2⌉, response 0..1),
  `shadingBlockMemory[out]` (rgba32uint).
- **Behaviour**: one 8×8 workgroup covers a 16×16 render tile; per-thread 2×2 sums feed
  multi-scale block means (4×4 and 8×8) in workgroup memory; each scale compares
  current vs jitter-delta-compensated reprojected means with a base floor plus a
  contrast-scaled term, gated by an 8-slot per-block memory of past means and a jump
  gate; the strongest gated scale is the response. Constants:
  `SHADING_FLOOR_MID 0.08`, `SHADING_FLOOR_COARSE 0.04`, `SHADING_FLOOR_CV 0.35`,
  `SHADING_MEMORY_SLOTS 8`, `SHADING_MEMORY_JUMP_GAIN 1.5`, `SHADING_MEMORY_STILL_PX
  0.05`.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var inputColor : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var lumaHistoryIn : texture_2d<f32>` | sampled | render `r32float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, R32F |
| 3 | `var dilatedMotion : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 4 | `var exposureTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 5 | `var exposurePrevTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 6 | `var lumaHistoryOut : texture_storage_2d<r32float, write>` | storage | render `r32float` | `UNIFORM_TYPE_IMAGE`, 2D, R32F, writable |
| 7 | `var shadingChangeOut : texture_storage_2d<r32float, write>` | storage | ⌈render/2⌉ `r32float` | `UNIFORM_TYPE_IMAGE`, 2D, R32F, writable |
| 8 | `var masks : texture_2d<f32>` | sampled | render `rgba8unorm`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA8 |
| 9 | `var blockMemoryIn : texture_2d<u32>` | sampled | `rgba32uint`, sampleType **`uint`** | `UNIFORM_TYPE_TEXTURE`, 2D, `DATA_FORMAT_R32G32B32A32_UINT` |
| 10 | `var blockMemoryOut : texture_storage_2d<rgba32uint, write>` | storage | `rgba32uint` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA32UINT, writable |

Workgroup memory: `var<workgroup> tileSums: array<vec4f,64>` (1024 B) +
`tileMoving: array<f32,64>` (256 B) = **1280 B** — the core's `isSupported` requires
`maxComputeWorkgroupStorageSize ≥ 1280` (`UpscalerCore.ts:469`).

### 7.6 accumulate — the core temporal pass (mandatory)

- **File**: `wgsl/accumulate.wgsl`. **Grid**: display size, 8×8,
  `ceil(dw/8) × ceil(dh/8) × 1`.
- **Writes**: `history[out]` (`rgb` = blended color in conditioned invertible-tonemap
  space, `a` = age/maxAccumulation), `locks[out]` (`r` = lock lifetime, `g` = locked
  luma, `b` = shading-change factor for debug, `a` = resolved caller alpha).
- **Behaviour** (full algorithm in `accumulate.ts:191-418`): jitter-aware separable
  Lanczos2 upsample over the 3×3 render footprint; alpha resolves with the same taps;
  Catmull-Rom history reprojection via 5 bilinear fetches + 2×2 texel de-ring bound
  (issue #118); invertible-tonemap history ratio correction for host pre-exposure and
  fixed/external conditioning exposure (not for auto-exposure); YCoCg variance AABB
  rectification with a lock widen and a still/converged relax; disocclusion, shading
  change, reactive and lock effects on the effective sample count; blend in
  conditioned tonemap space; store age. Constants: `REACTIVE_STRENGTH 0.9`,
  `CLIP_GAMMA 1.0`, `LOCK_DECAY 0.08`, `LOCK_GROW 0.5`, `LOCK_CONTRAST_LO/HI
  0.02/0.12`, `LOCK_PEAK_LO/HI 0.6/2.0`, `LOCK_CLAMP_RELAX 12.0`,
  `LOCK_HISTORY_BOOST 0.7`, `STILL_CLAMP_RELAX 8.0`, `STILL_MOTION_LO/HI 0.05/0.5`,
  `SHADING_AGE 0.75`.
- **Reset path** (first branch of `main`): `FLAG_RESET || exposure.a > 0.5 || offscreen`
  → store `current` with age `1/maxAccumulation` and zeroed locks (alpha = currentAlpha);
  no history read.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var inputColor : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var dilatedMotion : texture_2d<f32>` | sampled | render `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 3 | `var masks : texture_2d<f32>` | sampled | render `rgba8unorm`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA8 |
| 4 | `var historyIn : texture_2d<f32>` | sampled | display `rgba16float`; **sampleType `float`** (filtering sampler) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 5 | `var linearSampler : sampler` | sampler | filtering, clamp-to-edge | `UNIFORM_TYPE_SAMPLER` |
| 6 | `var historyOut : texture_storage_2d<rgba16float, write>` | storage | display `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |
| 7 | `var locksIn : texture_2d<f32>` | sampled | display `rgba16float`; **sampleType `float`** | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 8 | `var locksOut : texture_storage_2d<rgba16float, write>` | storage | display `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |
| 9 | `var exposureTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 10 | `var reactiveMask : texture_2d<f32>` | sampled | render-res mask or 1×1 zero dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |
| 11 | `var exposurePrevTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 12 | `var shadingChangeTex : texture_2d<f32>` | sampled | ⌈render/2⌉ `r32float` or 1×1 zero dummy, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D |

13 bindings in one group — the largest pass. `ShadingChangeTex` and `reactiveMask` may
be the 1×1 dummy but must still be bound (flag-gated reads).

### 7.7 RCAS (output, `sharpness > 0`)

- **File**: `wgsl/rcas_temporal.wgsl` (same bytes as milestone-1 `rcas_spatial.wgsl`).
  **Grid**: display size, 8×8.
- **Behaviour**: conditioned-input production RCAS: loads conditioned history texels
  as-is (no per-tap inversion), FSR 3.1.5 lower limiter, `RCAS_LIMIT = 0.25 − 1/16`,
  peak `exp2(-2·(1−sharpness))`, optional denoise (`FLAG_RCAS_DENOISE`), then one
  `tonemapInvert` + divide by exposure with the linear-space lobe cap (issues #32/#50).
  Alpha passes through from binding 4 (the locks buffer's `.a`).
- **Reads**: `history[out]` (previous dispatch) as a **sampled** texture, `exposure[cur]`,
  `locks[out]` alpha.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var inputColor : texture_2d<f32>` | sampled | display `rgba16float`, `unfilterable-float` (`textureLoad` only) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var exposureTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 3 | `var outputColor : texture_storage_2d<rgba16float, write>` | storage | display `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |
| 4 | `var alphaSource : texture_2d<f32>` | sampled | display `rgba16float`, `unfilterable-float` (`textureLoad` only) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |

### 7.8 blit (output fallback, `sharpness == 0`)

- **File**: `wgsl/blit_temporal.wgsl` (same bytes as milestone-1 `blit.wgsl`).
  **Grid**: display size, 8×8.
- **Behaviour**: bilinear sample of the history with the linear sampler; if
  `FLAG_INPUT_REINHARD`, `c = tonemapInvert(c) / exposure`; alpha from binding 5's `.a`.

| b | WGSL declaration | kind | format / sample type | RD type / format / writable |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var inputColor : texture_2d<f32>` | sampled | display `rgba16float`; **sampleType `float`** (sampling) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var linearSampler : sampler` | sampler | filtering, clamp-to-edge | `UNIFORM_TYPE_SAMPLER` |
| 3 | `var exposureTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, `unfilterable-float` | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 4 | `var outputColor : texture_storage_2d<rgba16float, write>` | storage | display `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |
| 5 | `var alphaSource : texture_2d<f32>` | sampled | display `rgba16float`; **sampleType `float`** (sampling) | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |

### 7.9 debug (conditional; replaces RCAS/blit)

- **File**: `wgsl/debug.wgsl`. **Grid**: display size, 8×8.
- **Behaviour**: `switch (C.debugMode)` — 1 motion, 2 disocclusion, 3 log depth, 4
  accumulation age (`history.a`), 5 locks (`locks.r`), 6 exposure-conditioned scene luma,
  7 shading-change factor (`locks.b`), 8 reactive; default black. Writes alpha 1.
  This pass is the cheapest way to validate velocity/depth/jitter integration; implement
  it early even though it is optional.

| b | WGSL declaration | kind | format / sample type | RD type |
| --- | --- | --- | --- | --- |
| 0 | `var<uniform> C` | uniform | 96 B | `UNIFORM_TYPE_UNIFORM_BUFFER`, 96 |
| 1 | `var dilatedMotion : texture_2d<f32>` | sampled | render `rgba16float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 2 | `var masks : texture_2d<f32>` | sampled | render `rgba8unorm`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA8 |
| 3 | `var dilatedDepth : texture_2d<f32>` | sampled | render `r32float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, R32F |
| 4 | `var historyIn : texture_2d<f32>` | sampled | display `rgba16float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 5 | `var locksIn : texture_2d<f32>` | sampled | display `rgba16float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 6 | `var exposureTex : texture_2d<f32>` | sampled | 1×1 `rgba16float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 7 | `var inputColor : texture_2d<f32>` | sampled | render `rgba16float`, unfilterable | `UNIFORM_TYPE_TEXTURE`, 2D, RGBA16F |
| 8 | `var reactiveMask : texture_2d<f32>` | sampled | render mask or 1×1 dummy | `UNIFORM_TYPE_TEXTURE`, 2D |
| 9 | `var outputColor : texture_storage_2d<rgba16float, write>` | storage | display `rgba16float` | `UNIFORM_TYPE_IMAGE`, 2D, RGBA16F, writable |

### 7.10 moments (standalone; **not** in the temporal graph)

- **File**: `wgsl/moments.wgsl`; owner `MomentsCore.ts` (its own constants buffer,
  pipeline and outputs; touches nothing in `UpscalerCore`).
- **Grid**: source size, 8×8. **Writes**: `moments` (source res, rgba16float,
  `.rg = (s, s²)`; `.ba` reserved) and `coarseOut` (⌈source/4⌉, 4×4 block means).
- **Scalar**: Rec.709 luma (`linear`) or YCoCg Y (`ycocg`), selected by
  `FLAG_MOMENTS_YCOCG` (2048) in the pass's own constants buffer.
- Bindings: 0 UBO (own), 1 `texture_2d<f32>` source (any float, `textureLoad` only,
  unfilterable), 2 storage rgba16float `momentsOut`, 3 storage rgba16float `coarseOut`.
- Vendored for completeness of the published primitive; the temporal upscaler's own
  variance clip keeps its fused inline 3×3 moments (no moments pass in the graph).

### 7.11 Pass duration/workgroup summary

| pass | invocation count | workgroup | workgroup memory | bindings | storage buffers | storage images | samplers |
| --- | --- | --- | --- | --- | --- | --- | --- |
| reconstruct (hw/linear) | rw·rh | 8×8 (64) | 0 | 6 | 1 (read_write, atomics) | 2 | 0 |
| depth clip | rw·rh | 8×8 | 0 | 7 | 2 (read + read_write) | 1 | 0 |
| generate reactive | rw·rh | 8×8 | 0 | 5 | 0 | 1 | 0 |
| exposure taps | 64 | 8×8 | 256 B | 8 | 0 | 1 | 1 |
| exposure every-texel partials | ⌈w/64⌉·⌈h/64⌉ workgroups × 256 | 16×16 | scalar reduction (internal) | 2 + 7 | 1 (read_write) | 1 (group 1) | 0 (`texel`) / 1 (`quad`) |
| exposure every-texel combine | 1 workgroup × 256 | 256×1 | internal | 2 + 7 | 1 | 1 | same |
| shading change | ⌈rw/2⌉·⌈rh/2⌉ | 8×8 | **1280 B** | 11 | 0 | 3 | 0 |
| accumulate | dw·dh | 8×8 | 0 | 13 | 0 | 2 | 1 |
| RCAS | dw·dh | 8×8 | 0 | 5 | 0 | 1 | 0 |
| blit | dw·dh | 8×8 | 0 | 6 | 0 | 1 | 1 |
| debug | dw·dh | 8×8 | 0 | 10 | 0 | 1 | 0 |
| moments | w·h | 8×8 | 512 B | 4 | 0 | 2 | 0 |

Per-shader-stage maxima actually used: sampled textures **9**, samplers **1**, storage
textures **3**, storage buffers **2**, uniform buffers **1** (2 with the every-texel
group pair), bindings per group **13**. The core's `UpscalerCore.isSupported` codifies
the required device limits (`UpscalerCore.ts:457-470`): `maxBindGroups ≥ 1`,
`maxBindingsPerBindGroup ≥ 13`, `maxSampledTexturesPerShaderStage ≥ 9`,
`maxStorageTexturesPerShaderStage ≥ 3`, `maxStorageBuffersPerShaderStage ≥ 2`,
`maxUniformBuffersPerShaderStage ≥ 1`, `maxSamplersPerShaderStage ≥ 1`,
`maxUniformBufferBindingSize ≥ 256`, `maxComputeWorkgroupSizeX/Y ≥ 8`,
`maxComputeInvocationsPerWorkgroup ≥ 64`, `maxComputeWorkgroupStorageSize ≥ 1280`.
The every-texel exposure additionally requires `maxComputeInvocationsPerWorkgroup ≥ 256`,
`maxComputeWorkgroupSizeX ≥ 256`, `maxBindGroups ≥ 2` (`UpscalerCore.ts:141-153`).

---

## 8. Exposure semantics (`EveryTexelExposure`) — deep dive

### 8.1 Why exposure exists

Accumulation happens in conditioned invertible-tonemap space (`c/(1+max(c))`) after
multiplying the input by the **conditioning exposure**. This keeps very bright/dark HDR
scenes in the same numeric working range (steady variance clip + firefly guard) and is
**divided back out before display**, so it never changes final brightness. Three
sources, in priority order (`_writeConstants` + `exposure_taps.wgsl` epilogue):

1. fixed `settings.exposure` — used when `autoExposure: false`;
2. auto-exposure (default) — log-average scene luminance, eased with eye adaptation;
3. `inputs.exposureTexture` — app-supplied, overrides both (and no metering runs).

Host pre-exposure (`inputs.preExposureTexture`) is a *different* concept: it declares an
exposure factor already baked into the input color; it is published in `.b` and
**preserved** at output, with history ratio-corrected across changes in accumulate.

### 8.2 Production gate, forms and fallback

```
wanted = path == 'temporal'
      && !failed_before
      && !(_exposureMetering == 'taps')            // bench override only
      && device supports 256-invocation workgroups && maxBindGroups >= 2
      && (bench override || renderW*renderH <= 2^19)   // 524288 ≈ 960×540
```

(`UpscalerCore.ts:122-153, 533-540`.) If wanted, `EveryTexelExposure.create()` compiles
both entry points asynchronously; until ready (and on any failure, for the life of the
instance) frames use the taps form. This is an *optional* stage: it never gates
`isReady` and toggling it does not restart history; the exposure eases with the
1/16-stop dead-band (`optionalPassTransition`, `UpscalerCore.ts:210-230`).

Forms: `'texel'` — one scalar per render texel (`log2(max(luma(texel),1e-4))`), and
`'quad'` — one bilinear tap at each 2×2 quad's corner (the quad's box average, then the
log). Production uses `'texel'`; `'quad'` is a bench candidate. Both are vendored.

### 8.3 Generated module structure (`exposure_every_texel.wgsl`, 10741 bytes)

Assembled by `createScalarKernel()` from `@pmndrs/downsampler@0.1.1`:

- `@group(0)` — the reduction's own bindings:
  - `0`: `var<uniform> ds_params : DsParams` (32 B, §4.6)
  - `1`: `var<storage, read_write> ds_partials : array<vec4f>` (16 B per workgroup)
- `@group(1)` — the repo hooks (production declarations renumbered from the taps pass,
  minus the sampler for the texel form):
  - `0`: `var<uniform> C : FsrConstants`
  - `1`: `var inputColor : texture_2d<f32>`
  - `2`: `var linearSampler : sampler` — **quad form only**
  - `3`: `var prevExposure : texture_2d<f32>`
  - `4`: `var exposureOut : texture_storage_2d<rgba16float, write>`
  - `5`: `var externalExposure : texture_2d<f32>`
  - `6`: `var hostPreExposure : texture_2d<f32>`
  - `7`: `var resetSignal : texture_2d<f32>`
- entry points: `@compute @workgroup_size(16, 16) fn partials` and
  `@compute @workgroup_size(256) fn combine` (`fn load`, `fn finish` helpers, `DsParams`,
  `@group(0) @binding(0) var<uniform> ds_params`, `origin = group.xy * 64u + local.xy`).
- the `finish` hook is the *production epilogue verbatim* with `logSum/1024` replaced by
  `exp2(logAverage)`; `'texel'` and `'quad'` differ only in `load` (+sampler binding 2).

### 8.4 Dispatch, params and resources (both forms)

```
texel form: metered grid (w,h) = (renderW, renderH)
quad  form: metered grid (w,h) = (ceil(renderW/2), ceil(renderH/2))
groupsX = ceil(w / 64);  groupsY = ceil(h / 64)        # TILE = 16 (invocations/axis) × 4 (block)
partials: dispatchWorkgroups(groupsX, groupsY, 1)
combine : dispatchWorkgroups(1, 1, 1)
DsParams = [w, h, 0, groupsX, groupsX*groupsY, w*h, 0, 0]   # u32[8], 32 bytes
partials buffer size = groupsX*groupsY*16 bytes
```

Both dispatches are encoded into the **same compute pass** (`exposure` label), with
`setBindGroup(0, group0); setBindGroup(1, group1);` and a pipeline switch between them
(`EveryTexelExposure.ts:259-280`). Examples: `960×540 → groups (15,9), params
[960,540,0,15,135,518400]`; `1×1 → (1,1)`.

### 8.5 The epilogue (`EXPOSURE_PUBLISH`) — exact behaviour

From `luminancePyramid.ts:53-97` (shared by taps, serial and every-texel `finish`):

```
hostRaw = hostPreExposure.r ; host = (hostRaw > 0) ? hostRaw : 1.0
avgLum  = exp2(logSum / (taps²)) / host        // host-invariant metering
target  = clamp(EXPOSURE_KEY / max(avgLum,1e-4), EXPOSURE_MIN, EXPOSURE_MAX)
reset   = FLAG_RESET || resetSignal.r > 0.5
prev    = prevExposure.r
exposure = target
if (!reset && prev > 0) {
    exposure = prev
    if (abs(log2(target/prev)) > ADAPT_DEADBAND_STOPS) {          // 1/16 stop
        rate = clamp(1 - exp2(-deltaTime * ADAPT_SPEED), 0, 1)    // ADAPT_SPEED 2.5/s
        exposure = prev + (target - prev) * rate
    }
}
exposure = FLAG_AUTO_EXPOSURE  ? exposure : C.exposure            // manual override
exposure = FLAG_EXTERNAL_EXPOSURE ? externalExposure.r : exposure // app override
store exposure[0] = vec4(exposure, avgLum, host, reset ? 1 : 0)
```

Constants: `EXPOSURE_TAPS 32`, `EXPOSURE_KEY 0.18`, `EXPOSURE_MIN 0.02`,
`EXPOSURE_MAX 8.0`, `ADAPT_SPEED 2.5`, `ADAPT_DEADBAND_STOPS 0.0625`.
`.r` = conditioning exposure, `.g` = metered average luma (0 when not metered), `.b` =
host pre-exposure, `.a` = **resolved reset** — the only GPU-side reset propagation to
shading change and accumulate.

### 8.6 No-metering optimisation

When `!FLAG_AUTO_EXPOSURE || FLAG_EXTERNAL_EXPOSURE`, the taps/serial shader returns
early after lane 0 publishes the provided exposure (issue #98; ~10 µs saved). The
every-texel form runs the same epilogue via `finish`. `.g` is 0 in that case ("not
metered") and no shader reads it. There is **no CPU readback** in any form.

### 8.7 Semantics relevant to the port

- No metering when fixed/external exposure is supplied; a dim/bright scene then keeps
  whatever conditioning the caller chose.
- Adaptive exposure is deliberately **not** history-corrected in accumulate (only fixed
  and external are); it tracks the scene's own brightness so old history moving toward
  the new lighting is desirable (`accumulate.ts:283-298`).
- A reset snaps the exposure (no easing) so a cut doesn't fade in a stale value.
- `resetHistory()` does not reset the stored exposure value; the snap path handles it.

---

## 9. History and reset semantics

### 9.1 What "history" is

The temporal accumulator keeps, per view:

| state | texture | encoding |
| --- | --- | --- |
| color history | `history[2]` (display, rgba16float) | `.rgb` = accumulated color in **conditioned invertible-tonemap space** (`tonemapInvertible(c·exposure)`), `.a` = age / `maxAccumulation` (0..1) |
| lock state | `locks[2]` (display, rgba16float) | `.r` = lock lifetime, `.g` = locked luma (conditioned tonemap space), `.b` = last shading-change factor (debug), `.a` = resolved caller alpha |
| exposure | `exposure[2]` (1×1, rgba16float) | `.r` conditioning, `.g` metered luma, `.b` host pre-exposure, `.a` resolved reset |
| depth ping-pong | `dilatedDepth[2]` (render, r32float) | dilated linear view depth; `previousDepth` guide = the other half |
| scatter ping-pong | `scatter[2]` (storage buffers) | reconstructed previous depth, +inf = empty |
| shading memory | `shadingLuma[2]` (render r32float), `shadingBlockMemory[2]` (rgba32uint) | detector priors |

Nothing in the graph is tone-mapped or display-encoded: the output is the caller's
linear/HDR domain; presentation transforms belong to the host.

### 9.2 Reset triggers (all collapse to `FLAG_RESET` and/or exposure `.a = 1`)

| trigger | mechanism | notes |
| --- | --- | --- |
| first frame after `configure()` | `resetHistory()` → `_pendingReset = true` | history/locks are freshly zeroed but never trusted |
| `configure()` on resize/path change | full reallocation + `resetHistory()` | CPU-side; also `jitter.reset()` |
| `resetHistory()` | `_pendingReset = true`, jitter sequence restarts | next dispatch is a reset frame |
| `DispatchInputs.reset` | flags `FLAG_RESET` this frame and `_pendingReset=true` (so a split frame's late stage also resets) | CPU verdict |
| `resetTexture` | read on the GPU: `.r > 0.5` at texel (0,0); depth clip writes disocclusion 1, exposure publishes `.a=1` | no readback; must be written from **this** frame's inputs to be same-frame (`docs/inputs-and-contracts.md` §Resets) |
| off-screen reprojection | accumulate treats `prevUV` outside `[0,1]` like a reset for that pixel | per-pixel, not full-frame |

Split frames: pass `resetTexture` to `dispatchGuides()`; the core keeps
`_frameResetSignal` and the late stage reuses it (`UpscalerCore.ts:1270-1273, 1437-1440`).

### 9.3 Per-pass reset behaviour (the whole reset contract)

- **depth clip**: `reset = FLAG_RESET || resetSignal.r > 0.5` → writes `disocclusion = 1`
  for every pixel (published guide agrees with the dropped history); the next-scatter
  clear still runs.
- **exposure**: on reset, `exposure = target` (snap; no easing), publishes `.a = 1`.
- **shading change**: `frameReset` → every texel compares neutrally (previous = current,
  response 0) and every block restarts its memory from the current mean. Also true on a
  `FLAG_SHADING_PRIME` frame (detector's first frame after turning on).
- **accumulate**: `FLAG_RESET || exposureTex.a > 0.5 || offscreen` → store current
  color with age `1/maxAccumulation` and zeroed locks (`alpha = currentAlpha`), no
  history read. Because the *age* restarts at `1/maxAccum`, convergence restarts from
  scratch (the first blended frame weights the current frame≈`maxAccumulation×` history
  of one sample).
- **shading block memory** is additionally zeroed by the CPU (queue write) when a
  temporal frame ran without the detector (`_shadingMemoryStale`), because the memory
  describes content that may have changed; an all-zero memory is read as "empty" and
  restarts.

### 9.4 No explicit clears

WebGPU zero-initializes new resources and the first frame is always a reset, so the
graph has **no clear dispatches**. The only resources with a non-zero initial state are
the two scatter buffers (+inf bits, §7.1).

### 9.5 Device loss / re-creation

The upstream core tracks device loss, invalidates readiness and clears cached pipelines;
`configure()`/`prepare()` recreate. A C++ port: recreate all shaders/pipelines and
working textures on `RD` device loss/context recreation, and reset history.

---

## 10. Cross-cutting subsystems

### 10.1 Reactive mask

- **Inputs**: `reactive` (render-res, `.r ∈ [0,1]`, authored by the app or written into a
  published `reactive` target between split dispatches) and/or `reactiveOpaqueColor`
  (render-res opaque-only color). The mask suppresses locks, keeps near-zero
  accumulation and snaps the pixel toward the current frame in accumulate
  (`REACTIVE_STRENGTH 0.9`).
- **Generation** (only with `reactiveOpaqueColor`): pass 3 writes
  `reactiveGenerated = max(clamp((diff−0.04)·2.0,0,0.9), clamp(incoming,0,1))` where
  `diff = max_channel |final − opaque|`. Merge, never overwrite
  (`generate_reactive.wgsl:41-56`). If the caller also supplies `reactive` it must not
  alias the generated target (upstream throws).
- **Absent input**: 1×1 zero `r8unorm` dummy bound, `FLAG_REACTIVE` clear, accumulate
  reads 0. The pass is not dispatched and `reactiveGenerated`/dummy is still bound.
- **Moment/pyramid note**: no separate T&C mask exists; the reactive channel is the only
  transparency/composition mechanism.

### 10.2 Shading-change detector

- One fused half-resolution dispatch (§7.5) replacing FSR's SPD pyramid + resolve.
- Compares **block means** (4×4 and 8×8 render texels), not per-texel ratios, with a
  jitter-delta-compensated previous sample clamped into the bilinear footprint's tap
  range; a noise floor per scale plus a block-contrast (coefficient of variation) term;
  an 8-slot per-block mean memory with a jump gate prevents aliasing recurrence from
  reading as a change; blocks that moved/were disoccluded/reset restart their memory.
- Response 0..1 feeds accumulate's `SHADING_AGE = 0.75` aging (locks protect thin
  features from it).
- Lazy optional pipeline: until compiled, `detectShadingChanges` is effectively off;
  the first frame after it turns on is a **prime** frame (`FLAG_SHADING_PRIME`), which
  never restarts the color history (`optionalPassTransition`).
- The `shadingChange` guide is only written while the detector runs; it is N−1 for
  consumers before the late stage.

### 10.3 Luminance "pyramid"

There is no mip chain. The name survives from FSR; the local pass is a single
log-average reduction to a 1×1 exposure texel (§8). Intermediate luminance mips would
have no consumer (the shading detector does its own fused workgroup-local reduction).
Any C++ implementation should not add a pyramid.

### 10.4 Moments (`MomentsCore`)

Standalone statistics primitive (per-texel `(E[x],E[x²])` plus one 4×4 coarse level),
used by SVGF-class consumers, **not** part of the upscaler pipeline (§7.10). Vendored
for completeness; do not wire it into the temporal effect.

### 10.5 Alpha

Every path upscales RGBA. Temporal: the input `.a` (coverage) is resolved with the same
jitter-aware Lanczos taps and blend weight in accumulate and stored in **locks `.a`**,
because the history's own `.a` is the accumulation age; RCAS/blit pass locks `.a` to the
output's `.a`. An opaque input (alpha 1 everywhere) resolves to exactly 1. The C++ port
must not replace alpha with 1.

---

## 11. CPU-side replication plan (C++ / `RendererRD`)

### 11.1 Where it plugs in (engine slot)

The temporal path is **not** a drop-in for the milestone-1 `SpatialUpscaler` slot. That
slot runs after tonemapping on display-referred color (`docs/PMNDRS_UPSCALER_SPATIAL.md`
§9.1). The temporal path needs linear HDR scene color and produces linear HDR display
color, exactly like the engine's FSR2 integration:

| item | engine location (`/tmp/godot48`) |
| --- | --- |
| 3D scaling mode selection | `render_forward_clustered.cpp:1855-1890` (`SCALE_FSR2` branch) |
| FSR2 upscale call (the slot to mirror) | `render_forward_clustered.cpp:2573-2615` — `params.color = rb->get_internal_texture(v)`, `params.output = rb->get_upscaled_texture(v)`, `params.depth = rb->get_depth_texture(v)`, `params.velocity = rb->get_velocity_buffer(false, v)`, jitter, `z_near/z_far` |
| upscaled-texture ensure | `render_forward_clustered.cpp:2458-2465` (`rb->ensure_upscaled()`) |
| tonemap runs after | `render_forward_clustered.cpp:2659` |
| motion-vector rendering (already exists) | `using_motion_pass = rb_data.is_valid() && using_upscaling` (`:1928`), `COLOR_PASS_FLAG_MOTION_VECTORS` |
| depth texture (sampling usage already set) | `render_scene_buffers_rd.cpp:811-825, 848-864` |
| velocity texture (`rg16float`) | `render_scene_buffers_rd.cpp:731-740, 866-867` |
| jitter producer | `renderer_scene_cull.cpp:54-63, 2709-2731`; `renderer_viewport.cpp:302-313`; applied in `render_scene_data_rd.cpp:43-47,63-67` |
| raw-WGSL capability gate | `RD::SUPPORTS_RAW_WGSL_COMPUTE` (`rendering_device_commons.h`, `docs/WGSL_COMPUTE_PATH.md` §3.3) |
| reference C++ effect pattern (constants ring, uniform sets, compute list, barrier) | `renderer_rd/effects/pmndrs_upscaler.cpp:85-368` |
| FSR2's own UBO ring pattern | `renderer_rd/effects/fsr2.cpp:179-181, 435-436` |

Recommendation: implement `PmndrsTemporal` as a new effect class selected by a new
`ViewportScaling3DMode` (or a project setting) that mirrors the FSR2 block, **not** as a
`SpatialUpscaler`. Keep FSR1/spatial untouched for fallback. Gate availability on
`SUPPORTS_RAW_WGSL_COMPUTE` + the `UpscalerCore.isSupported` limit set (§7.11) +
storage/sampling format support for `R32F`, `RGBA16F`, `RGBA8`, `RGBA32UINT` and the
depth format + (optionally) the every-texel gate.

The FSR2 slot is effectively free on WebGPU today: the engine's FSR2 path needs
`B10G11R11` storage images, which WebGPU does not have, so
`render_forward_clustered.cpp:1862-1877` falls back to no 3D upscaling (and with it
`using_upscaling = false`, hence no velocity). A pmndrs-temporal scaling mode must set
`using_upscaling`/the motion-vector pass flag for itself so velocity and depth are
rendered before the effect runs; mirror the `SCALE_FSR2` block, not the FSR1 block.

### 11.2 Per-frame sequence (`process()`-equivalent, per view)

```
frame start (once per viewport, per frame):
  jitter_px = pmndrs_jitter[current_phase]                 // §5.1; phase advanced once per dispatched frame
  scene_data.taa_jitter = to_engine_jitter(jitter_px)      // §5.6 step 3
  render scene -> internal HDR color + depth + velocity     // velocity jitter-free automatically

pmndrs_temporal.process(rb, view):
  1. sizes: render = rb->get_internal_size(); display = rb->get_target_size()
  2. resolve all texture RIDs (create-on-first-use via rb->create_texture; §11.4)
  3. constants ring slot → write early + late FsrConstants (§4.2-4.5)
     (before any compute_list_begin — buffer_update is forbidden inside a list)
  4. build uniform sets for each enabled pass (binding tables §7; cache with
     UniformSetCacheRD::get_cache_vec(shader, set, uniforms))
  5. list = compute_list_begin()
       dispatch reconstruct        → barrier
       dispatch depth_clip         → barrier
       [generate reactive]         → barrier
       dispatch exposure (taps)    → barrier
       [shading change]            → barrier
       dispatch accumulate         → barrier
       dispatch debug | rcas | blit
     compute_list_end()
  6. frame-end: history_index ^= 1; depth_index ^= 1; scatter_index ^= 1 (at next frame start);
     frame_index++; pending_reset = false; frame_reset_signal = null
```

Barrier policy: one `RD::compute_list_add_barrier()` between *every* dependent pair. On
WebGPU the driver's usage guard also splits passes for texture hazards, but the barrier
is required for the storage-buffer write→read (`reconstruct` → `depth clip`) and makes
intent deterministic (`docs/PMNDRS_UPSCALER_SPATIAL.md` §8.3, milestone-1 effect
`pmndrs_upscaler.cpp:337-340`).

### 11.3 State machine to replicate (exact)

| state | type | rules |
| --- | --- | --- |
| `history_index` | 0/1 | flip at end of each **completed** temporal frame |
| `depth_index` | 0/1 | flip at end of each completed temporal frame |
| `scatter_index` | 0/1 | flip at start of the guides stage |
| `latest_history_write` / `latest_depth_write` | 0/1 | set to the just-written half, for guide getters |
| `frame_index` | u32 | ++ per completed frame; written to constants |
| `pending_reset` | bool | init **true**; set by `configure`/`resetHistory`/`reset` input; cleared at frame end |
| `shading_prime` | bool | true iff detector ran this frame and did not run last encoded frame; cleared implicitly next frame |
| `jitter_index` | u32 | 0..phaseCount−1; `beginFrame`: retreat if last frame undispatched, then advance; reset on `configure`/`resetHistory` |
| `frame_undispatched` | bool | held-frame bookkeeping (§5.5) |
| `frame_reset_signal` | RID | this frame's GPU reset texture, kept across split stages |
| `shading_memory_stale` | bool | detector skipped while a temporal frame ran → zero block memory on its next run |
| `split_pending` | bool | a `dispatchGuides` without `dispatchUpscale` yet |
| `view_count` / per-view textures | int | history/locks/etc. are per-view: allocate with `layers = view_count` and slice per view |

### 11.4 Resource allocation through `RenderSceneBuffersRD`

Persistent/working textures should be owned by the render buffers (auto-freed/recreated
on resize) using `create_texture(scope, name, format, usage, samples, size, layers,
mipmaps, unique, transient)` (see `render_scene_buffers_rd.h` and milestone-1 doc §9.4
for the exact call shape) and guarded with `has_texture()` in `ensure_context()`.
Suggested scope/names: `PMNDRS_TEMPORAL/history-0|1`, `locks-0|1`, `exposure-0|1`,
`dilated-depth-0|1`, `dilated-motion`, `masks`, `reactive-gen`, `shading-luma-0|1`,
`shading-signal`, `shading-memory-0|1`; display-res textures at display size and
render-res ones at `get_internal_size()`. The output can be the engine's
`rb->get_upscaled_texture(v)` (display, rgba16float, storage+sampling) exactly as FSR2
uses it.

- The **dummy 1×1** textures (reactive zero, reset zero, external exposure zero, host
  pre-exposure zero) are sampled-only (`TEXTURE_USAGE_SAMPLING_BIT`), format `r8unorm`
  for the reactive/reset dummy (upstream) or `rgba16float` — content zero; they must be
  bound but are inert unless the corresponding flag/input is present. Reuse one dummy
  where the WGSL `texture_2d<f32>` type allows.
- The **scatter buffers** are 4·rw·rh bytes each (up to ~4 MB at 1080p/2×); create them
  as RD storage buffers owned by the effect (`storage_buffer_create(size, init_data)`),
  recreated on render-size change, initial content `0x7f800000` per word, freed in the
  destructor. Do not put them in render buffers (no format).
- The **constants ring**: 2 stage buffers × N slots. N ≥ 2 × viewports × frames in
  flight + slack (milestone-1 used 64 slots of 256 B = 16 KB; temporal needs 2× that).
  Write with `buffer_update` **before** `compute_list_begin()`; a slot is never reused
  while a recorded dispatch could still read it.
- **Debug**: the debug pass needs nothing new; use it to validate depth/velocity/jitter
  integration (view 1-3) before trusting accumulation. `DebugView` mapping:
  1 motion, 2 disocclusion, 3 depth, 4 age, 5 locks, 6 exposure, 7 shading change,
  8 reactivity.

### 11.5 Quality / scale mapping

- Reuse the engine's 3D scaling ratio (`renderer_viewport.cpp` 3D scale + FSR ratio
  plumbing). Upstream ratios (`math/resolution.ts:7-13`):
  `native-aa 1.0`, `quality 1.5`, `balanced 1.7`, `performance 2.0`,
  `ultra-performance 3.0`; render size = `floor(display / ratio)` per axis, min 1
  (`getRenderResolution`); a custom/explicit render size overrides and derives the ratio
  (`renderWidth/renderHeight` config).
- Jitter phase count = `round(8·ratio²)` (§5.1). The engine's existing formula is the
  same but truncates; use round to match.
- `maxAccumulation` default 24 (FSR3 uses ~32); `sharpness` 0.8 default;
  `autoExposure` on; `lockThinFeatures` on; `detectShadingChanges` on.

### 11.6 What can run without a split-frame API

The engine drives one `process()` per view per frame, which is the monolithic path
(`dispatch()` equivalent): no split frame is needed unless other engine effects consume
the geometry guides same-frame. Replicate the split only if that becomes a requirement;
the state machine above already supports it.

---

## 12. Raw-WGSL path: requirements and gaps

### 12.1 What milestone 1 already exercised

Single bind group; one uniform buffer (96 B payload in 256 B allocation); `texture_2d<f32>`
sampled bindings (RGBA16F); write-only `texture_storage_2d<rgba16float, write>`; no
samplers in the shipped passes; no storage buffers; no depth textures; no workgroup
memory; no atomics; no second bind group; entry point `main`; 8×8 workgroups. The
optional `blit.wgsl` (sampler + `textureSampleLevel`) exists in the milestone-1 vendor
tree but was not the shipped path.

### 12.2 What the temporal path needs beyond that

| capability | where | milestone-1? |
| --- | --- | --- |
| storage buffers, incl. `var<storage, read_write>` + `array<atomic<u32>>` | reconstruct b5, depth clip b3/b5 | **no** |
| `texture_depth_2d` sampled binding | reconstruct b1 (hardware mode) | **no** |
| `texture_2d<u32>` sampled (uint sample type) | shading change b9 | **no** |
| two bind groups (`@group(1)`) | every-texel exposure only | **no** |
| two entry points in one module (`partials`, `combine`) | every-texel exposure only | **no** |
| sampler bindings (filtering) | exposure taps, accumulate, blit | optional blit only |
| workgroup memory (`var<workgroup>`) | exposure taps (256 B), shading change (1280 B), moments (512 B) | **no** |
| `texture_storage_2d` write formats `r32float`, `rgba8unorm`, `rgba32uint` | depth clip b4, shading change b6/b7/b10, reconstruct b3 | `rgba16float` only |
| 13 bindings in one group / 9 sampled + 3 storage images per stage | accumulate, shading change | 5 max |
| 16×16 and 256×1 workgroups (256 invocations) | every-texel exposure only | 8×8 only |
| multiple dependent dispatches with buffer write→read | whole graph | one texture dependency |

### 12.3 Mandatory driver extensions (ordered by criticality)

**Gap A — sampled `texture_2d<f32>` must be able to resolve to WebGPU
`unfilterable-float`.** *Blocking.*
The driver derives the BGL sample type only from the WGSL component type
(`is_depth ? Depth : u32→Uint : i32→Sint : (multisampled ? UnfilterableFloat : Float)`;
`rendering_device_driver_webgpu.cpp:5193-5204` and the combined-sampler twin at
`5230-5246`), and `RDD::WGSLUniform` has no field for a sampled texture's sample type
(`rendering_device.h:1072-1092`; the milestone-1 effect comments this at
`pmndrs_upscaler.cpp:58-68`). The temporal path binds `r32float` textures
(`dilatedDepth` in depth clip/debug, `shadingLuma`, `shadingSignal` in accumulate),
whose sample type is `unfilterable-float`; a `float` BGL entry is invalid for them, so
bind-group creation fails and the command buffer goes black. Note the reverse direction
also matters: `texture_2d<f32>` bound to `rgba16float` **with a filtering sampler**
(accumulate history/locks, blit, exposure color) must stay `float`; and `rgba16float`
loaded only with `textureLoad` is fine either way (WebGPU allows `unfilterable-float`
for any f32 format — the filterable formats list both sample types in the spec's texture
format capabilities table).
Recommended fix — implement the WebGPU **auto-layout** rule in the driver's WGSL scan
(spec §"createRenderPipeline … auto layout"): for a sampled `f32` texture, use `float`
iff it has a static use with a texture builtin call that also uses a sampler; else
`unfilterable-float`. The driver already does a call-site scan for
`wgsl_sampler_needs_nonfiltering` (`rendering_device_driver_webgpu.cpp:4807-4862`), so
the hook exists; it must also record per-texture "used with a sampler". Alternative
(deterministic, smaller): add `texture_sample_type`/`filterable` to `WGSLUniform` for
`UNIFORM_TYPE_TEXTURE` and honour it in BGL construction; the effect then declares
`r32float → unfilterable-float`, `rgba16float → float` (or unfilterable where load-only).

**Gap B — depth-only view for `texture_depth_2d` bindings of the engine's scene depth.**
*Blocking for `depthMode: 'hardware'` (the default).*
The engine's scene depth is a combined depth-stencil format
(`DATA_FORMAT_D32_SFLOAT_S8_UINT` / `DATA_FORMAT_D24_UNORM_S8_UINT`,
`render_scene_buffers_rd.cpp:811-825`), which maps to WebGPU `depth24plus-stencil8`
(`depth32float-stencil8` only with the optional feature; the WebGPU driver reports
D32S8 unsupported without it, `rendering_device_driver_webgpu.cpp:2805-2820`). Textures
created through the generic path get a default view with `WGPUTextureAspect_All`
(`:1977-1985`); WebGPU requires a **single-aspect** view to bind a depth-stencil texture
as `texture_depth_2d`. The driver already does this for external textures (`:2049`),
MSAA depth in combined bindings (`:5983-6001`) and depth-as-float copies
(`_depth_float_copy_view`, `:2269-2320`), but the standalone `UNIFORM_TYPE_TEXTURE` path
(the one the raw effect uses) has no depth case (`:5841-5880` falls through to
`default_view`). Fix: when `expected_sample == Depth` (and the texture has a stencil
aspect / is combined), create (and cache) a depth-only view from `tex->view_source`
with `baseMipLevel = tex->base_mipmap`, one mip, one layer, `aspect = DepthOnly`, push it
into `us->temp_views`. Workaround if the driver cannot be changed first: use
`depthMode: 'linear'` (needs a linear view-Z color target; the engine does not produce
one today) or add an engine-side helper that creates a depth-only RID.

**Gap C — multiple entry points per raw-WGSL shader.** *Optional (every-texel exposure
only; defer to a later milestone).* `RD::WGSLShaderData` documents "entry point MUST be
named main" and the driver hard-codes `"main"` when creating the compute pipeline
(`rendering_device.h:1088-1093`, `rendering_device_driver_webgpu.cpp:10049`). The
generated exposure reduction needs `partials` + `combine`. Fix options: (1) add
`String entry_point = "main"` to `WGSLShaderData`, plumb it into the driver shader object
and use it in `compute_pipeline_create`; (2) without driver changes, create two RD
shaders from two text variants where the wanted entry is renamed `main` and the other is
renamed `*_unused` (reflection/binding checks unchanged; both modules compile). Use the
taps exposure form until then.

**Gap D — storage buffers incl. atomics on the raw path.** *Supported by the existing
code, but never exercised — verify.* Covered by the driver's SSBO scan
(`:4869-4915`, read/write vs read-only), BGL construction (`:5346-5370`) and bind-group
creation (`:6075-6105`). Requirements on the effect: reflection
`UNIFORM_TYPE_STORAGE_BUFFER`, `writable = true` for `read_write`,
`length = 0` (RD skips the exact-size check, `rendering_device.cpp:5217-5219`),
buffers created with storage usage. Both scatter buffers are distinct resources, so the
driver's writable-storage aliasing stub is not triggered; the depth clip binds one
read-only and one read-write buffer in the same set (allowed). `atomicMin` on
`array<atomic<u32>>` is core WGSL and needs no driver work.

**Gap E — two bind groups on the raw path.** *Supported structurally (reflection → N
BGLs → pipeline layout, `rendering_device_driver_webgpu.cpp:5098-5145, 5488-5628`; RD
`MAX_UNIFORM_SETS=16`), never exercised for raw WGSL — verify with the every-texel
smoke test.* Only needed for the every-texel form.

**Gap F — sampler bindings.** *Supported; verify.* BGL `resolve_sampler_type` defaults
to `Filtering` (`:5120-5135`), which is right for the linear clamp sampler. Needed by
exposure taps, accumulate and blit.

**Gap G — storage-texture formats.** *Supported.* `r32float`, `rgba8unorm`,
`rgba16float`, `rgba32uint` are all in the driver's format parse table
(`:4920-5010`) and are core WebGPU storage formats. **Do not author `r8*`/`r16*`
storage formats in raw WGSL**: the driver promotes those texture formats at creation
(`_promote_storage_format`, `:3007-3056`) but the Tint-output WGSL rewrite that keeps the
text match is skipped for raw stages (`:4101-4145`), so a raw `r16float` storage
declaration would mismatch the promoted `r32float` texture.

**Gap H — workgroup sizes / limits.** *No change.* 8×8 and 16×16/256 are within WebGPU
defaults; `WGSLShaderData.workgroup_size` must equal the WGSL and is used by RD for
`compute_list_dispatch_threads` and validation. The core's limit set (§7.11) should be
checked at effect creation.

**Gap I — constants buffer.** *No change.* `UNIFORM_TYPE_UNIFORM_BUFFER` with
`length = 96` and a 256-byte buffer is already proven on the spatial path.

**Gap J — pass-splitting / usage tracking.** The driver tracks texture usages per
compute pass from the pipeline layout and splits the WebGPU pass automatically
(`_note_compute_set_usage` / `_compute_pass_usage_guard`, `:9771-9880`); RD's
`compute_list_add_barrier()` ends/restarts the RD compute list (`rendering_device.cpp:
7604-7620`). Still emit a barrier between every dependent dispatch — especially for the
buffer write→read pair, which is not a texture hazard.

### 12.4 Confirmed non-needs

- **No storage-texture read or `read_write`** anywhere in the graph: all storage images
  are `write` only. This keeps the raw path's v1 rule ("no read/read_write storage
  textures", `docs/WGSL_COMPUTE_PATH.md` §11.2.9) satisfied.
- No `f16`, no subgroups, no `enable` directives, no overrides/spec constants, no push
  constants, no combined image-samplers, no immutable samplers, no binding arrays.
- No vertex/fragment stages: every module is a compute-only module with entry `main`.
- No indirect dispatch.

### 12.5 Reflection check-list per shader (summary; full table Appendix A)

For every pass: set 0 must contain exactly the bindings of its table, with
`stages = COMPUTE`, `length = 1` for TEXTURE/IMAGE, `length = 0` for STORAGE_BUFFER,
`length = 96` for UNIFORM_BUFFER, `texture_type = TEXTURE_TYPE_2D` for all textures,
`texture_format` for IMAGE = the WGSL storage format, `writable = true` for IMAGE writes
and `read_write` storage buffers. The container verifies that each reflected binding has
a matching `@group(G) @binding(2B)` declaration in the WGSL and fails loudly otherwise
(`rendering_shader_container_webgpu.cpp:56-90, 147-215`). Authored binding numbers are
kept in the reflection; the WGSL text is doubled internally — do not author doubled
numbers.

---

## 13. Risks and open questions (ranked)

1. **Gap A (unfilterable-float sample type).** Blocks almost every pass that touches
   `r32float`. Symptom: WebGPU validation error at first bind-group/pipeline creation,
   black frame. Must land before any temporal work is testable. The driver scan is the
   right place; a per-binding explicit type is the lower-risk stopgap.
2. **Gap B (depth-only view).** Blocks `depthMode: 'hardware'`, i.e. the default and only
   mode that needs no extra engine pass. Symptom: reconstruct binding validation
   failure. Workaround `depthMode: 'linear'` needs an engine-produced linear depth target
   (not available), so treat Gap B as mandatory too.
3. **Velocity/jitter convention mismatch (§5.6).** Silent failure: doubled edges, smear,
   ghosting, "boiling" instead of a hard error. The engine's velocity is used by TAA as a
   UV delta; the pmndrs core expects NDC `cur−prev`. One bad sign in `motionScale` or in
   the jitter conversion invalidates every temporal result. Mitigation: implement the
   debug pass first (motion/depth views), validate before tuning anything else, and add
   a small CPU-side unit test for the phase sequence and conversion math (the upstream
   has `halton.test.ts` / `jitter.test.ts` / `resolution.test.ts` to mirror — port the
   test vectors, they are device-free).
4. **Unknown raw-path support for storage buffers/atomics and two bind groups (Gaps D/E).**
   The code paths exist but no shader has used them; expect at least one round of
   validation debugging (reflection type/length/writable, BGL storage vs read-only,
   buffer size, initial +inf data, aliasing stub triggers). Verify early with a
   throwaway two-dispatch atomics smoke test before building the whole graph.
5. **Memory/bandwidth (§6.4, ~100 MB at 1080p/2×).** Two display-res RGBA16F history
   pairs plus the lock pair dominate. On mobile WebGPU / 4K this may be prohibitive; if
   so, the fix is port-side (pack locks into the history `.a` or a smaller format) and
   would diverge from the vendored WGSL — a deliberate decision, not an implementation
   detail.
6. **Exposure quality form selection.** Taps (1024 samples) are steady on large renders
   but wobble with the jitter phase at small render sizes (upstream gates to
   every-texel ≤ 2^19 texels; NEXT-STEPS §17). Starting with taps is fine for a first
   version; expect exposure pumping at low render resolutions (≤640×360) until the
   every-texel form (Gap C) lands. Also note the 1/16-stop dead-band exists precisely to
   suppress this — do not tune it away.
7. **Shading-change ghosting without the detector.** If v1 skips the detector (allowed,
   §14), animated lighting/materials ghost for as long as the history lives. This is a
   quality risk, not a correctness bug; document it and enable the detector as soon as
   the core stabilises.
8. **Reactive transparency.** Without `reactiveOpaqueColor` or an authored mask,
   transparents/particles ghost (no reliable depth/velocity). Render an opaque-only pass
   or author coverage; the generator pass is cheap once the graph works.
9. **Split-frame / held-frame edge cases.** Ping-pong flips, jitter retreat, and the
   retained reset signal must match §9/§5.5 exactly; a mismatch gives one-frame pops or
   stale-half guides. Mirror the upstream tests' intent with device-free unit tests.
10. **Engine integration slot change (§11.1).** Moving the upscaler before tonemapping
    changes which textures the effect sees and where post-process runs; this is a larger
    renderer change than milestone 1 and needs its own careful test pass (HDR values
    above 1.0, alpha, MSAA-off, view count, screen copies). Also confirm that the
    velocity buffer is actually rendered for the new scaling mode
    (`using_upscaling` gate) and that the jitter sequence/phase count is switched for
    the new mode only.
11. **`textureSampleLevel` on `rgba16float`** (history/locks/color) requires the texture
    to be filterable and the sampler `filtering` — fine on WebGPU core, but the BGL
    inference must not mark those unfilterable (see Gap A's reverse direction).
12. **Depth precision on the WebGPU path.** The engine can use `D32_SFLOAT_S8_UINT` only
    with the optional `depth32float-stencil8` feature; otherwise `D24S8`. The upstream
    disocclusion tolerance is viewport/depth-scaled and documented to give fewer flags
    at higher render resolution; 24-bit depth is acceptable but the guide tolerance was
    tuned on 32-bit three.js depth — expect slightly different disocclusion behaviour
    and validate with the debug view (this is a quality note, not a blocker).
13. **Non-filterable `r32float` published guides.** Any engine consumer of
    `dilatedDepth` must sample nearest / `textureLoad` (upstream ships `NearestFilter`).
    Documented for consumers in `docs/temporal-guides.md`.

---

## 14. Recommended minimal implementation slice

### 14.1 Mandatory for a ghosting-correct temporal core

`reconstruct` (hardware depth) → `depth clip` → `exposure` (taps form) →
`accumulate` → `RCAS` (or `blit` at sharpness 0), with the full persistent set:
history[2], locks[2], exposure[2], dilatedDepth[2], dilatedMotion, masks, scatter[2],
plus the constants ring. That is 5 dispatches + state and satisfies "ghosting-correct":
depth dilation + scatter/gather disocclusion + variance-clip/lock accumulation +
conditioned tonemap resolve.

### 14.2 Skippable for the first version (documented, flag-cleared, dummy-bound)

| skip | mechanism | consequence |
| --- | --- | --- |
| `generate reactive` | don't dispatch; bind 1×1 zero dummy; `FLAG_REACTIVE` clear | transparents/particles ghost (§13.8) |
| `shading change` | don't dispatch; bind 1×1 zero dummy; `FLAG_SHADING_CHANGE` clear; don't allocate luma history / block memory | animated shading ghosts (§13.7) |
| `exposure every-texel` | use `exposure_taps` only | slight exposure wobble at small renders (§13.6) |
| `debug` | skip initially — but it is the **first thing to add** for validating depth/velocity/jitter | none |
| `depthMode: 'linear'` | skip; hardware only | none |
| `reconstructVariants`, `moments`, `easu`, `RCAS_*` experiment variants | never | none |
| split frame (`dispatchGuides`/`dispatchUpscale`) | monolithic `process()` | no same-frame geometry guides for other effects; add later if needed |
| alpha passthrough | actually free: accumulate already writes locks `.a`; keep it | dropping it would be a regression |

Also defer: `rcasDenoise` (keep `FLAG_RCAS_DENOISE` clear), multi-view layers beyond the
basic loop, GPU timings (optional).

### 14.3 Driver extensions that must land first

1. **Gap A** — unfilterable-float sampled textures (driver scan inference, or explicit
   per-binding type). 2. **Gap B** — depth-only view for `texture_depth_2d`. 3. Verify
   **Gaps D/E/F** with a smoke test (storage buffers/atomics, sampler, two bind groups);
   no code change expected for D/F, and E is only needed later. 4. Defer **Gap C**
   (multi entry point) until the every-texel exposure is worth it.

### 14.4 Suggested milestones

- **M2a — drivers + guides smoke**: Gap A/B fixes; atomics smoke test; effect class that
  runs `reconstruct` + `depth clip` and publishes the guides; validate with the debug
  pass (motion/depth/disocclusion views) and a static-camera test (`jitter`-only, no
  motion should be visible in motion view).
- **M2b — core accumulation**: add exposure taps + accumulate + RCAS/blit; validate
  static convergence (supersampling should reduce edge aliasing), camera pan (no smear),
  fast pan (disocclusion clear), reset (no ghost pop).
- **M2c — quality/robustness**: shading change, reactive generation, every-texel
  exposure (Gap C), alpha/composition, colour-domain checks with HDR values > 1.
- **M2d — engine integration polish**: FSR2-style slot selection, quality presets/jitter
  phase count, memory footprint, mobile gating, docs.

---

## 15. Evidence index

Upstream (pinned commit `f6e5209`), under `/tmp/pmndrs-upscaler/packages/core/src/`:

- `UpscalerCore.ts`: `:122-153` exposure gate/support; `:225-230`
  `optionalPassTransition`; `:347-422` state fields; `:457-470` device limits;
  `:499-531` resources/mandatory passes; `:687-726` configure; `:892-925`
  resetHistory/beginFrame; `:932-1001` dispatch; `:1003-1011` endFrame;
  `:1026-1146` split frame; `:1176-1229` blit/spatial; `:1231-1325` temporal/guides
  encode; `:1367-1594` late encode; `:1597-1617` RCAS encode; `:1643-1698`
  flags/constants; `:1729-1846` texture allocation; `:1850-1884` guides getters.
- `internal/ConstantsBuffer.ts` (layout writer, 256 B); `internal/EveryTexelExposure.ts`
  (`:38-92` tile/params, `:259-280` encode); `internal/ComputePass.ts` (`:126`
  WORKGROUP_SIZE 8, `:173-183` dispatch math).
- `shaders/common.ts` (`:9-27` flags, `:30-64` `WGSL_CONSTANTS`, `:113-126`
  `linearizeDepth`); `shaders/reconstruct.ts` (`:7-80` body, `:115-184` hardware/linear,
  `:211-299` depth clip); `shaders/generateReactive.ts`; `shaders/luminancePyramid.ts`
  (`:6-7` exposureTaps, `:10-97` head/epilogue, `:139-206` taps, `:218-239` serial,
  `:261-313` every-texel hooks); `shaders/shadingChange.ts`; `shaders/accumulate.ts`;
  `shaders/rcas.ts` (`:28-221` generator, `:314` production form); `shaders/blit.ts`;
  `shaders/debug.ts`; `shaders/moments.ts`; `shaders/README.md` (pass graph + audit).
- `shaders/shaders.test.ts`: `:45-62` shader registry, `:63-87` binding counts,
  `:89-160` `BASELINE_FINGERPRINTS`, `:560-645` every-texel expectations.
- `math/halton.ts`, `math/jitter.ts`, `math/resolution.ts`, `textures.ts`, `types.ts`.
- Docs: `docs/architecture.md`, `docs/inputs-and-contracts.md`,
  `docs/temporal-guides.md`, `docs/engine-contract.md`.

Vendor tree: `/tmp/pmndrs-vendor-temporal/` (`wgsl/*.wgsl` sha256 + upstream FNV in
`manifest.json`; `upstream/*` verbatim TS/docs; `tools/extract-shaders.mjs`).

Engine (`/tmp/godot48`):

- `servers/rendering/rendering_device.h:1072-1110` — `WGSLUniform`/`WGSLShaderData`/
  `shader_create_from_wgsl`; `servers/rendering/rendering_device.cpp:4711-4775` — raw
  WGSL validation; `:4871-5240` — `uniform_set_create`; `:7604-7620` —
  `compute_list_add_barrier`.
- `drivers/webgpu/rendering_shader_container_webgpu.cpp:56-90` binding doubling,
  `:147-215` raw container creation + reflection/WGSL pair check.
- `drivers/webgpu/rendering_device_driver_webgpu.cpp:161` `parse_group_binding`;
  `:1955-1990` texture create default view; `:2269-2320` `_depth_float_copy_view`;
  `:3007-3056` `_promote_storage_format`; `:4660-4790` texture scan (dimension,
  depth, component); `:4807-4862` sampler non-filtering scan; `:4869-4915` SSBO scan;
  `:4920-5010` storage-texture scan; `:5098-5145, 5180-5488` BGL construction;
  `:5841-5880` standalone TEXTURE bind (no depth view); `:5983-6010` MSAA depth view;
  `:6075-6105` storage-buffer bind + alias stub; `:9771-9880` compute usage guard;
  `:10049` hard-coded `"main"` entry point; `:10448-10470` device limits.
- `servers/rendering/renderer_rd/effects/pmndrs_upscaler.cpp:43-83, 85-368` —
  `WGSLUniform` helpers, constants ring, uniform sets, compute list, barrier.
- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp:1855-1890`
  scaling mode selection, `:1928` motion pass gate, `:2458-2465` `ensure_upscaled`,
  `:2573-2615` FSR2 upscale slot (color/depth/velocity/jitter), `:2659` tonemap.
- `servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.cpp:731-740`
  velocity allocation, `:811-825` depth format/usage, `:848-867` depth/velocity
  formats, `:325-327, 742-798` velocity getters.
- `servers/rendering/renderer_scene_cull.cpp:54-63` Halton, `:2709-2731` jitter array;
  `servers/rendering/renderer_viewport.cpp:302-313` jitter phase count;
  `servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.cpp:36-67`
  `taa_jitter` applied; `core/math/projection.cpp:1044-1047` `add_jitter_offset`.
  `servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl:3216-3225`
  velocity write; `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl:316-325`
  velocity-UV consumption; `servers/rendering/renderer_rd/shaders/effects/
  motion_vector_inc.glsl` depth-derived velocity.

---

## Appendix A — consolidated `RD::Uniform` / `WGSLUniform` reflection table

Set 0 unless noted. `U` = `UNIFORM_TYPE_UNIFORM_BUFFER` (length 96), `T` =
`UNIFORM_TYPE_TEXTURE` (`TEXTURE_TYPE_2D`, length 1), `I` = `UNIFORM_TYPE_IMAGE`
(`TEXTURE_TYPE_2D`, length 1, writable), `S` = `UNIFORM_TYPE_STORAGE_BUFFER` (length 0),
`SM` = `UNIFORM_TYPE_SAMPLER` (length 1). Formats are only validated by RD for `I`
(`texture_format != DATA_FORMAT_MAX`); always set them.

| pass | b | type | format | writable | sample type consequence |
| --- | --- | --- | --- | --- | --- |
| reconstruct (hw) | 0/1/2/3/4/5 | U/T(depth)/T/T/I/I/S | —/depth/rg16/r32f/rgba16f/rw buffer | 0/0/0/1/1/1 | depth / (unfilterable-float §12.3A) |
| reconstruct (linear) | 1 | T | r32f | 0 | unfilterable-float |
| depth clip | 0..6 | U/T/T/S/I/S/T | 1:r32f,2:rgba16f,3:buf,4:rgba8,5:buf(rw),6:any float | 4,5 yes | b1/2/6 unfilterable |
| generate reactive | 0..4 | U/T/T/I/T | 1,2 rgba16f,3 rgba8,4 any | 3 yes | unfilterable |
| exposure taps | 0..7 | U/T/SM/T/I/T/T/T | 1 rgba16f,3 rgba16f,4 rgba16f, rest any | 4 yes | b1 float; b3/5/6/7 unfilterable |
| exposure every-texel g0 | 0/1 | U(uniform 32 B)/S(rw vec4f) | — / rw buffer | 1 yes | — |
| exposure every-texel g1 | 0..7 (no 2) | U/T/[SM]/T/I/T/T/T | same as taps | 4 yes | same as taps |
| shading change | 0..10 | U/T/T/T/T/T/I/I/T/T/I | 1 rgba16f,2 r32f,3 rgba16f,4/5 rgba16f,6/7 r32f,8 rgba8,9 rgba32uint,10 rgba32uint | 6,7,10 yes | b9 uint; rest unfilterable |
| accumulate | 0..12 | U/T/T/T/T/SM/I/T/I/T/T/T/T | 4/6/7/8 rgba16f, 10 any, 12 any, 1 rgba16f,2 rgba16f,3 rgba8 | 6,8 yes | b4/7 float; rest unfilterable |
| RCAS | 0..4 | U/T/T/I/T | 1/2/4 rgba16f, 3 rgba16f | 3 yes | unfilterable |
| blit | 0..5 | U/T/SM/T/I/T | 1/3/5 rgba16f, 4 rgba16f | 4 yes | b1/5 float; b3 unfilterable |
| debug | 0..9 | U/T*7/I | 9 rgba16f | 9 yes | unfilterable |
| moments | 0..3 | U/T/I/I | 2/3 rgba16f | 2,3 yes | unfilterable |

## Appendix B — dispatch formula sheet

Notation: `rw`,`rh` render size; `dw`,`dh` display size; `ceil8(x) = (x+7)/8`.

| pass | groups (x,y,z) |
| --- | --- |
| reconstruct | `(ceil8(rw), ceil8(rh), 1)` |
| depth clip | `(ceil8(rw), ceil8(rh), 1)` |
| generate reactive | `(ceil8(rw), ceil8(rh), 1)` |
| exposure taps / serial | `(1, 1, 1)` — dispatched as `dispatch(pass, …, 8, 8)` |
| exposure every-texel `partials` | `(ceil(w/64), ceil(h/64), 1)`, `w,h` = render (texel) or `ceil(render/2)` (quad) |
| exposure every-texel `combine` | `(1, 1, 1)` |
| shading change | `(ceil8(max(1,ceil(rw/2))), ceil8(max(1,ceil(rh/2))), 1)` |
| accumulate | `(ceil8(dw), ceil8(dh), 1)` |
| RCAS / blit / debug | `(ceil8(dw), ceil8(dh), 1)` |
| moments | `(ceil8(w), ceil8(h), 1)` for the source size |

## Appendix C — every WGSL `@group/@binding` declaration (authored numbers)

`b0` is the shared `FsrConstants` uniform (`@group(0) @binding(0)`) for every pass
unless a line says otherwise.

```
reconstruct_hardware / reconstruct_linear : g0 b0 C | b1 sceneDepth (texture_depth_2d /
    texture_2d<f32>) | b2 sceneVelocity texture_2d<f32> | b3 dilatedDepth storage r32float write
    | b4 dilatedMotion storage rgba16float write | b5 reconstructedDepth storage read_write
    array<atomic<u32>>
depth_clip        : b1 dilatedDepth | b2 dilatedMotion | b3 reconstructedDepth storage read
                    array<u32> | b4 maskOutput storage rgba8unorm write | b5 nextReconstructedDepth
                    storage read_write array<u32> | b6 resetSignal texture_2d<f32>
generate_reactive : b1 opaqueColor | b2 finalColor | b3 reactiveOut storage rgba8unorm write
                    | b4 incomingReactive
exposure_taps     : b1 inputColor | b2 linearSampler sampler | b3 prevExposure | b4 exposureOut
                    storage rgba16float write | b5 externalExposure | b6 hostPreExposure
                    | b7 resetSignal
exposure_every_*  : g0 b0 ds_params uniform, b1 ds_partials storage read_write array<vec4f>;
                    g1 b0 C (=g1), b1 inputColor, [b2 sampler quad only], b3 prevExposure,
                    b4 exposureOut storage rgba16float write, b5 externalExposure,
                    b6 hostPreExposure, b7 resetSignal
shading_change    : b1 inputColor | b2 lumaHistoryIn | b3 dilatedMotion | b4 exposureTex
                    | b5 exposurePrevTex | b6 lumaHistoryOut storage r32float write
                    | b7 shadingChangeOut storage r32float write | b8 masks
                    | b9 blockMemoryIn texture_2d<u32> | b10 blockMemoryOut storage rgba32uint write
accumulate        : b1 inputColor | b2 dilatedMotion | b3 masks | b4 historyIn | b5 linearSampler
                    | b6 historyOut storage rgba16float write | b7 locksIn | b8 locksOut storage
                    rgba16float write | b9 exposureTex | b10 reactiveMask | b11 exposurePrevTex
                    | b12 shadingChangeTex
rcas_temporal     : b1 inputColor | b2 exposureTex | b3 outputColor storage rgba16float write
                    | b4 alphaSource
blit_temporal     : b1 inputColor | b2 linearSampler | b3 exposureTex | b4 outputColor storage
                    rgba16float write | b5 alphaSource
debug             : b1 dilatedMotion | b2 masks | b3 dilatedDepth | b4 historyIn | b5 locksIn
                    | b6 exposureTex | b7 inputColor | b8 reactiveMask | b9 outputColor storage
                    rgba16float write
moments           : b1 sourceTexture | b2 momentsOut storage rgba16float write
                    | b3 coarseOut storage rgba16float write
```

## Appendix D — vendored-file SHA-256 (full)

See `/tmp/pmndrs-vendor-temporal/manifest.json` and `PROVENANCE.md` for the complete
table (14 WGSL modules + verbatim upstream sources + tool). The extractor re-verifies all
upstream FNV-1a baselines on every run; a mismatch aborts with a non-zero exit code.


