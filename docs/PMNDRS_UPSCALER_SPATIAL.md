# pmndrs/upscaler spatial path (EASU + RCAS) — raw-WGSL port spec

Research + vendoring record for **milestone 1** of the pmndrs/upscaler WebGPU port:
run the **spatial** upscaler (`upscaleSpatial`: EASU → RCAS) from Godot C++ on
Dawn/WebGPU as a hand-written-WGSL compute effect, selected as a first-class
Viewport scaling mode (`Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL`, mode 6)
instead of FSR1. Implementation-ready: everything the C++ effect needs is in
this file; the upstream TypeScript is reference-only.

| field | value |
| --- | --- |
| upstream repo | <https://github.com/pmndrs/upscaler> |
| **pinned commit** | **`f6e52098edffb66e16298a30c53b4da37b4b7d7f`** (`v0.6.0`, 2026-11-27) |
| license | MIT (Dennis Smolek) + AMD FidelityFX EASU/RCAS notice (both in `LICENSE`; vendored) |
| vendored tree | `/tmp/pmndrs-vendor/` (WGSL in `wgsl/`, verbatim sources in `upstream/`, provenance + hashes in `PROVENANCE.md`) |
| scope | spatial path only (`easu` + `spatialRcas`); no temporal history, no jitter, no depth/velocity |
| engine tree | implementation done in `/tmp/godot48`; see "As built" below and `docs/WEBGPU_HANDOFF.md` |

Source of truth: upstream commit above, read on 2026-11-27. This doc was
verified against that tree (fingerprints, line numbers). Engine-side facts were
verified against `/tmp/godot48` (HEAD `582d619026` + working-tree changes).

**Status (2026-11-27): milestone 1 implemented, browser-verified, and now a
first-class Viewport scaling mode.** The effect lives in
`servers/rendering/renderer_rd/effects/pmndrs_upscaler.{h,cpp}` +
`pmndrs_upscaler_wgsl.h` (both WGSL modules embedded verbatim), running through
the raw-WGSL RenderingDevice path (`docs/WGSL_COMPUTE_PATH.md` §11). Selection
is `Viewport.scaling_3d_mode == Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` (value
6; `RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL` is the RSE
counterpart) — a first-class mode appended after `SCALING_3D_MODE_NEAREST` so
pre-existing values are unchanged (`*_MAX` = 7). There is **no bool project
setting**: the earlier `rendering/scaling_3d/pmndrs_spatial` setting was removed,
mode 1 is real AMD FSR1 again, and mode 6 is exposed as
`rendering/scaling_3d/mode = "pmndrs (Spatial - Fast)"`. The TPS demo (external
test project) has a dedicated **"pmndrs Spatial"** button in Settings →
Resolution Scale Filter (WebGPU only, restores from the saved `user://` config,
switches the mode live on Apply; the resolution scale must be below Native
because spatial modes at scale 1.0 are internally switched OFF). Verified on the
TPS Forward+ WebGPU export 2026-11-27 21:54 via that button:
`[PMNDRS] spatial upscaler running (EASU+RCAS) 640x360 -> 960x540, 120x68
groups`, 59-60 fps, 0 GPU validation errors; the same build with the default
FSR1 selection shows only the availability line and runs FSR1 with 0 errors.
The spec below is kept as the port contract; as-built deviations: selection is
a first-class scaling mode rather than the `fsr1_implementation` setting
proposed in §9.1/§12 Q3 (the mode-6 enum is the answer to Q3), the constants UBO
is a 64-slot ring (not a single buffer), the `sharpness == 0` blit fallback was
not implemented (RCAS runs at s=0 instead), the temporal bindings are stubbed as
described, and the vendored WGSL is embedded as C++ strings rather than shipped
as `.wgsl` assets.

---

## 0. Headline findings (read this first)

1. **Upstream has no `.wgsl` files.** WGSL is embedded in TypeScript template
   strings under `packages/core/src/shaders/` and concatenated at runtime by a
   tiny TS assembler (`assembleShader`, `wgsl.ts`). WGSL has no `#include`; the
   assembled modules are self-contained single translation units. The two
   modules needed here (plus the optional `sharpness == 0` blit fallback) were
   extracted mechanically and validated against the upstream repo's own
   reviewed FSR fingerprints: `easu` = `48248d62`, `rcas` = `b0405308`,
   `blit` = `ef2eec52` (FNV-1a, `shaders.test.ts`). All three match, so the
   vendored WGSL is exactly the production assembly.
2. **There is no extra setup pass.** The CPU writes one 96-byte constants
   struct (in a 256-byte uniform buffer) and dispatches two compute passes.
3. **No samplers, no filtered samples.** Both passes use `textureLoad` level 0
   only. Sampler requirement: **none**. Both shaders are 8×8 workgroups.
4. **EASU writes an intermediate; RCAS reads it and writes the output.** Both
   storage textures are `rgba16float`, write-only. The intermediate needs
   `STORAGE_BINDING | TEXTURE_BINDING` (RCAS samples it); the destination needs
   `STORAGE_BINDING` and must be an `rgba16float` texture.
5. **Two hard integration constraints** discovered against the current WebGPU
   driver (details in §8): (a) the driver doubles non-combined binding indices
   (`@binding(N)` is laid out at `N*2`), so a raw-WGSL container must match that
   convention; (b) the driver's compute-pass splitter decides read/write
   conflicts from the *pipeline layout* entries, so the storage-image bindings
   must be declared write-only in the layout or WebGPU pass validation can fail.
6. **Milestone-1 placement is the existing FSR1 spatial-upscaler slot** in
   `renderer_scene_render_rd.cpp` (post-tonemap, per view), selected by the new
   first-class `Viewport.SCALING_3D_MODE_PMNDRS_SPATIAL` mode (6) instead of
   FSR1. That slot already routes WebGPU's non-storage-capable render
   targets through a storage-capable `rgba16f` intermediate + blit, so the
   effect can write straight into a storage image.

---

## 1. What the upstream spatial path does (CPU side)

`UpscalerCore` spatial graph (`packages/core/src/UpscalerCore.ts`):

```
configure({displayWidth, displayHeight, path:'spatial' [, qualityMode|customUpscaleRatio|custom renderWidth/Height]})
  └─ allocate: output (display, rgba16float), easu-output (display, rgba16float),
               exposure[2] (1×1 rgba16float, allocated on every path, unused by spatial)
  └─ compile: easu, spatialRcas, blit

per frame: dispatch(color, camera)
  ├─ write constants (256-byte buffer @ group0 binding0)    [CPU, once per frame]
  ├─ EASU  : color(render) -> easu-output(display)          [ceil(display/8) groups]
  └─ RCAS  : easu-output   -> output(display)               [same dispatch shape]
              (if sharpness == 0 the core uses a blit pass instead; milestone 1
               runs RCAS unconditionally — see §12 Q4)
```

Evidence: pass list `UpscalerCore.ts:526`; spatial encode `:1198-1237`;
per-pass bind-group order `:1200-1211` (EASU: constants, input, output) and
`:1603-1610` (RCAS: constants, input, exposure, output, alpha); `easu-output`
allocation `:1762-1764`; jitter is disabled on the spatial path
(`_jittering` = temporal only, `:795-797`); `beginFrame()` returns `null`.

The spatial path needs **no depth, no velocity, no camera values, no history**.
The upstream three adapter even feeds a stand-in camera (`SPATIAL_CAMERA` in
`packages/three/src/UpscalerNode.ts`) because only `near/far` are staged and the
spatial shaders ignore them.

---

## 2. Vendored files and provenance

Full details, hashes and the re-extraction command: `/tmp/pmndrs-vendor/PROVENANCE.md`.
License text (MIT + AMD FSR notice) is reproduced there and in
`/tmp/pmndrs-vendor/upstream/LICENSE`; **copy the AMD notice into the engine
tree when the shaders land there.**

| vendored file | content | use |
| --- | --- | --- |
| `wgsl/easu.wgsl` | assembled `EASU_SHADER`, sha256 `1758c181…87b58`, upstream fingerprint `48248d62` | pass 1 source (compile as-is) |
| `wgsl/rcas_spatial.wgsl` | assembled `RCAS_SHADER` (= `createRcasShader(true, true)`), sha256 `e68ac0f9…cae7f`, fingerprint `b0405308` | pass 2 source (compile as-is) |
| `wgsl/blit.wgsl` | assembled `BLIT_SHADER`, sha256 `0b13bfa5…1d347`, fingerprint `ef2eec52` | **optional** upstream `sharpness == 0` fallback (EASU→blit; needs a sampler — §4/§12 Q4). Milestone 1 runs RCAS instead |
| `upstream/easu.ts`, `rcas.ts`, `blit.ts`, `common.ts`, `wgsl.ts` | verbatim upstream shader sources | audit only |
| `upstream/ConstantsBuffer.ts`, `ComputePass.ts`, `UpscalerCore.ts`, `types.ts`, `textures.ts`, `resolution.ts` | verbatim CPU-side contract | audit only |
| `upstream/LICENSE`, `upstream/REPO_README.md`, `upstream/SHADERS_README.md` | license + upstream docs | attribution/reference |
| `tools/extract-shaders.mjs` | deterministic extraction + fingerprint check | reproduce the WGSL |

`wgsl/easu.wgsl` and `wgsl/rcas_spatial.wgsl` are **complete modules** — copy
verbatim into the engine (source strings for the raw-WGSL shader-container
path), do not edit, do not pass them through glslang/Tint SPIR-V translation. If the driver's binding-doubling
convention forces a mechanical rewrite (§8.2), keep the canonical files and
generate a derived variant, and record the transform.

**#include-free confirmation:** none of the three modules contains a
preprocessor include (`grep -c '#include' wgsl/*.wgsl` = 0 for all) or any
`enable` directive; each is one self-contained WGSL translation unit (struct +
helpers + one `@compute fn main`). The *upstream .ts sources* import shared chunks (`WGSL_CONSTANTS`,
`WGSL_TONEMAP`) with TypeScript `import`s and concatenate them — a build-time
TS composition step, **not** a WGSL `#include`. That is why the assembled
modules, not the `.ts` files, are the vendored shader artifacts.

---

## 3. Pass 1 — EASU (edge-adaptive spatial upscale)

`wgsl/easu.wgsl`, entry point `main`, `@compute @workgroup_size(8, 8)`.

### 3.1 Bindings (`@group(0)`, one bind group)

| binding | WGSL declaration | WebGPU layout entry | RD `UniformType` | resource | format / sample type | usage on the texture | required |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `var<uniform> C : FsrConstants` | uniform buffer | `UNIFORM_TYPE_UNIFORM_BUFFER` | constants buffer | n/a | buffer size ≥ struct size (96); use 256 | yes |
| 1 | `var inputColor : texture_2d<f32>` | sampled texture, no sampler | `UNIFORM_TYPE_TEXTURE` | scene color, **render** resolution | `rgba16float` (any float/unorm format is layout-valid; see §6.1) | `TEXTURE_BINDING` (`TEXTURE_USAGE_SAMPLING_BIT`), 1 mip, 1 sample, non-MSAA | yes |
| 2 | `var outputColor : texture_storage_2d<rgba16float, write>` | storage texture, access **write**, format `rgba16float`, dimension 2D | `UNIFORM_TYPE_IMAGE` | EASU intermediate, **display** resolution | `rgba16float` only (format is in the WGSL text) | `STORAGE_BINDING`; also `TEXTURE_BINDING` because RCAS samples it | yes |

Layout requirements from `ShaderUniform` (`rendering_device_commons.h:1119-1126`):
binding 0 `UNIFORM_TYPE_UNIFORM_BUFFER`, `length` 96 (or 256, must be ≤ buffer
size), `stages = COMPUTE`, `writable=false`; binding 1 `UNIFORM_TYPE_TEXTURE`,
`TEXTURE_TYPE_2D`; binding 2 `UNIFORM_TYPE_IMAGE`, `TEXTURE_TYPE_2D`,
`texture_format = DATA_FORMAT_R16G16B16A16_SFLOAT`, `writable = true`. The
runtime table and the exact reflection contract are in §9.3.

### 3.2 Dispatch

| item | value |
| --- | --- |
| workgroup | `(8, 8, 1)` → 64 invocations |
| covered pixels | one invocation = one **output** (display-res) pixel |
| dispatch | `dispatchWorkgroups(ceil(displayWidth / 8), ceil(displayHeight / 8), 1)` |
| edge handling | shader guard: `if (any(vec2f(gid.xy) >= C.displaySize)) { return; }` (easu.wgsl:106) — no partial-tile math on the CPU; extra invocations just return |
| examples | 960×540 → `120 × 68`; 1280×720 → `160 × 90`; 1920×1080 → `240 × 135`; 640×360 → `80 × 45` |

### 3.3 Algorithm contract (implementers don't need to change it; this is what
the shader expects from the CPU)

* **Source mapping** (`easu.wgsl:111-114`):
  `pp = (vec2f(gid.xy) + 0.5) * C.renderSize * C.displaySizeInv - 0.5`;
  `fp = floor(pp)`, fractional `p = pp - fp`, integer base `ip = vec2i(fp)`.
  So `C.renderSize` **must be the exact input texture size** and
  `C.displaySizeInv` **must be `1/displaySize`** (written per axis). No jitter is
  applied. Ratio can be any ≥ 1 (1.5 used in milestone 1), not just integer.
* **12-tap footprint** (`easu.wgsl:116-139`): B(0,-1) C(1,-1) E(-1,0) F(0,0)
  G(1,0) H(2,0) I(-1,1) J(0,1) K(1,1) L(2,1) N(0,2) O(1,2), each loaded with
  `clamp(p, 0, renderSize-1)` (`easuLoad`, `:39-42`) — border clamping is
  inside the shader; no addressing mode/sampler involved.
* Edge analysis over luma `L = 0.5R + G + 0.5B`; kernel shaping; 12-tap rotated
  anisotropic Lanczos2; dering clamp to min/max of the 4 nearest texels
  (`:141-191`).
* **Alpha is filtered too** — EASU is a 4-channel kernel; an opaque input stays
  opaque, a coverage mask is upscaled consistently (`easu.wgsl:45-48,191-192`;
  upstream `shaders/README.md` "FSR1 spatial fallback").
* Output write: `textureStore(outputColor, gid.xy, pix)` — write-only storage
  texture, no readback.

---

## 4. Pass 2 — RCAS (robust contrast adaptive sharpen, spatial form)

`wgsl/rcas_spatial.wgsl`, entry point `main`, `@compute @workgroup_size(8, 8)`.
This is the upstream **production** RCAS (`RCAS_SHADER = createRcasShader(true,
true)`, `rcas.ts:314`), i.e. FSR 3.1.5 lower limiter + denoise parity,
conditioned-input form. The spatial path selects it by default
(`UpscalerCore.ts:444,596`).

### 4.1 Bindings (`@group(0)`)

| binding | WGSL declaration | WebGPU layout entry | RD `UniformType` | resource | format / sample type | usage on the texture | required |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `var<uniform> C : FsrConstants` | uniform buffer | `UNIFORM_TYPE_UNIFORM_BUFFER` | same 256-byte constants buffer (EASU and RCAS can share one buffer if values match) | n/a | size ≥ 96 | yes |
| 1 | `var inputColor : texture_2d<f32>` | sampled texture | `UNIFORM_TYPE_TEXTURE` | EASU output, **display** resolution | `rgba16float` | `TEXTURE_BINDING` (already set on the intermediate), 1 mip, 1 sample | yes |
| 2 | `var exposureTex : texture_2d<f32>` | sampled texture | `UNIFORM_TYPE_TEXTURE` | **placeholder in spatial mode** — only loaded when `FLAG_INPUT_REINHARD` is set (`rcas_spatial.wgsl:150-160`); milestone 1 keeps that flag clear, but the binding is statically present and must be bound | any float format, ≥ 1×1 | `TEXTURE_BINDING` | yes (bind the scene color or the EASU output) |
| 3 | `var outputColor : texture_storage_2d<rgba16float, write>` | storage texture, write, `rgba16float` | `UNIFORM_TYPE_IMAGE` | final output, **display** resolution | `rgba16float` only | `STORAGE_BINDING` | yes |
| 4 | `var alphaSource : texture_2d<f32>` | sampled texture | `UNIFORM_TYPE_TEXTURE` | the EASU output again (upstream binds `_easuOutput` as both input and alpha source, `UpscalerCore.ts:1216-1223`) | `rgba16float` | `TEXTURE_BINDING` | yes |

Binding 1 and 4 may be the **same texture** (two sampled bindings to one view
are legal). RCAS never sharpens alpha: it writes
`textureLoad(alphaSource, sp, 0).a` (`rcas_spatial.wgsl:178`).

Access modes: all sampled bindings (EASU binding 1; RCAS bindings 1, 2, 4) are
read-only sampling access; both storage bindings are write-only
(`texture_storage_2d<…, write>`). Nothing is read-write, which is why no
WebGPU read-write-storage feature or shadow-copy path is involved.

### 4.2 Dispatch

Identical shape to EASU (one invocation = one display pixel):
`dispatchWorkgroups(ceil(displayWidth/8), ceil(displayHeight/8), 1)`, guard
`gid.xy >= C.displaySize` (`rcas_spatial.wgsl:63`), cross taps clamped to
`[0, displaySize-1]` (`rcasLoad`, `:56-59`).

### 4.3 Behaviour contract for the CPU

* **`C.sharpness` is the only tuning value** (`rcas_spatial.wgsl:116-117`):
  `peak = exp2(-2·(1 − C.sharpness))`, lobe scaled by `peak`. `1` = sharpest,
  `0` = -2 stops. It is the same convention AMD FSR2's RCAS consumes, and
  exactly equals AMD FSR1's `con = exp2(-stops)` when
  `C.sharpness = 1 − stops/2` (see §6.4 for the engine mapping).
* **The spatial form conditions internally.** RCAS's limiter math assumes
  `[0,1]`; on the spatial path the five taps are first compressed with
  `tonemapInvertible(c) = c/(1 + max(c))`, sharpened in that bounded space, then
  inverted once, anchored on the exact linear centre, and capped by the lobe
  applied in linear space against the darkest ring tap (`rcas_spatial.wgsl:75-90,
  133-176`). Consequence: **`FLAG_INPUT_REINHARD` (bit 8) must be 0** for the
  spatial path. If it is set the shader takes the temporal branch, reads
  `exposureTex`, and divides the result by the pre-exposure — wrong for this path.
  For input values in `[0,1]` the conditioning round-trips exactly; for linear
  HDR > 1 it is firefly-capped. No presentation transform is applied.
* **`FLAG_RCAS_DENOISE` (bit 512)** toggles FSR1's lone-outlier denoise
  (`rcas_spatial.wgsl:119-131`). Upstream default is off; milestone 1 keeps it
  off unless a test needs it.
* `RCAS_LIMIT = 0.25 − 1/16` is a module constant; not a uniform.
* Alpha is passthrough (binding 4), not sharpened.

---

## 5. Constants buffer — `FsrConstants`

### 5.1 Buffer contract

| item | value |
| --- | --- |
| WGSL struct | `FsrConstants`, declared in both assembled modules (`easu.wgsl:1-18`, `rcas_spatial.wgsl:1-18`) |
| struct size | **96 bytes** (last member at offset 92; struct align 8, WGSL uniform address space rounds struct align to 16 → 96 stays 96) |
| GPU buffer | **256 bytes**, bound at `@group(0) @binding(0)`; one buffer per frame stage upstream (`ConstantsBuffer.SIZE = 256`, `ConstantsBuffer.ts:11-12`). Only the first 96 bytes are read; 256 is the round size and matches the WebGPU 256-byte uniform-offset alignment |
| std140-like rules | WGSL uniform layout, not std140: `vec2f` align 8/size 8, `f32`/`u32` align 4/size 4; every member below already satisfies it. Do **not** re-pack |
| endianness | little-endian (WebGPU host); write floats/uint32s directly |
| update path | `RD::buffer_update(buffer, 0, 96, &data)` **before** `compute_list_begin()` (RD forbids buffer updates while a compute list is open: `rendering_device.cpp:1244-1247`). WebGPU queue writes are ordered before submit; no explicit barrier needed |
| cpu values | For the spatial path write the full 96 bytes every frame (cheap, avoids stale fields). Values not listed as used may be zero. |

### 5.2 Byte layout (offset, size, WGSL type, meaning, spatial value)

| offset | size | type | field | upstream meaning | **milestone-1 spatial value** |
| --- | --- | --- | --- | --- | --- |
| 0 | 8 | `vec2f` | `renderSize` | render (input) resolution in px | **input texture size** (`rb->get_internal_size()`) as floats |
| 8 | 8 | `vec2f` | `displaySize` | output resolution in px | **output size** (`rb->get_target_size()`) as floats |
| 16 | 8 | `vec2f` | `renderSizeInv` | `1/renderSize` | `1/renderSize` (unused by both spatial shaders, but write it) |
| 24 | 8 | `vec2f` | `displaySizeInv` | `1/displaySize` | **`1/displaySize`, required by EASU's source mapping** |
| 32 | 8 | `vec2f` | `jitter` | current sub-pixel jitter (render px) | 0,0 (spatial has no jitter) |
| 40 | 8 | `vec2f` | `jitterPrev` | previous jitter | 0,0 |
| 48 | 8 | `vec2f` | `motionScale` | NDC→UV delta (default 0.5,−0.5) | 0,0 (unused; upstream default irrelevant here) |
| 56 | 8 | `vec2f` | `depthNearFar` | camera near/far | 0,0 (unused) |
| 64 | 4 | `f32` | `sharpness` | RCAS attenuation, 0..1, 1 = sharpest | **`clamp(1 − fsr_sharpness/2, 0, 1)`** (default 0.2 → 0.9; §6.4) |
| 68 | 4 | `f32` | `maxAccumulation` | temporal history cap | 0 (unused) |
| 72 | 4 | `f32` | `exposure` | conditioning pre-exposure | 0 (unused when `FLAG_INPUT_REINHARD` clear) |
| 76 | 4 | `f32` | `deltaTime` | seconds | 0 (unused) |
| 80 | 4 | `u32` | `flags` | `FLAG_*` bits | **0** (optionally `512` = `FLAG_RCAS_DENOISE`); must keep bit 8 clear |
| 84 | 4 | `u32` | `frameIndex` | frame counter | 0 (unused) |
| 88 | 4 | `u32` | `debugMode` | DebugView | 0 (unused) |
| 92 | 4 | `u32` | `_pad` | padding in `FsrConstants`. The standalone luminance/exposure-pyramid pass replaces this member with `exposureTaps` and the upstream CPU writer sets it to 32 (`luminancePyramid.ts:6-7`, `ConstantsBuffer.ts:27-28`); EASU/RCAS never read it | 0 |
| **96** | | | **total** | | |

### 5.3 Flag bits (decoded by `hasFlag`, constants chunk)

| bit | value | name | spatial usage |
| --- | --- | --- | --- |
| 0 | 1 | `FLAG_RESET` | ignored by EASU/RCAS |
| 1 | 2 | `FLAG_REVERSED_DEPTH` | ignored |
| 2 | 4 | `FLAG_PERSPECTIVE` | ignored |
| **3** | **8** | **`FLAG_INPUT_REINHARD`** | **must be 0** — selects the temporal RCAS branch otherwise (§4.3) |
| 4 | 16 | `FLAG_INPUT_DISPLAY` | ignored by these shaders |
| 5 | 32 | `FLAG_LOCKS` | ignored |
| 6 | 64 | `FLAG_AUTO_EXPOSURE` | ignored |
| 7 | 128 | `FLAG_SHADING_CHANGE` | ignored |
| 8 | 256 | `FLAG_REACTIVE` | ignored |
| **9** | **512** | **`FLAG_RCAS_DENOISE`** | optional RCAS denoise |
| 10 | 1024 | `FLAG_EXTERNAL_EXPOSURE` | ignored |
| 11/12 | 2048/4096 | moments / shading-prime | not in this constants chunk, irrelevant |

Only bits 3 and 9 change EASU/RCAS behaviour on the spatial path. Everything
else can be zero for milestone 1.

### 5.4 Why these values (spatial path vs upstream default config)

Upstream `UpscalerCore` stages the same struct on every path
(`_writeConstants`, `UpscalerCore.ts:1653-1700`) but its default runtime
settings (`sharpness 0.8`, `rcasDenoise false`, `maxAccumulation 24`,
`exposure 1.0`, `autoExposure true`, `lockThinFeatures true`,
`detectShadingChanges true`, `UpscalerCore.ts:430-437`) only matter to temporal
passes. For the spatial path the shaders read exactly:
`renderSize`, `displaySize`, `displaySizeInv`, `sharpness`, `flags` (plus the
never-set `FLAG_INPUT_REINHARD`). Write the full struct anyway so a future
temporal milestone needs no layout change.

---

## 6. Input / output requirements

### 6.1 Textures

| resource | size | format | usage bits | other |
| --- | --- | --- | --- | --- |
| scene color (EASU in) | render (`internal_size`) | `rgba16float` (the renderer's tonemapper destination in Forward+/HDR; `renderer_scene_render_rd.cpp:790` uses `rb->get_base_data_format()`; preferred format is always RGBA16F, `:1197-1199`) | `TEXTURE_USAGE_SAMPLING_BIT` (already present: the tonemapper destination is created with SAMPLING\|STORAGE\|COLOR_ATTACHMENT) | 1 mip, 1 sample, no MSAA; alpha carried; exact size — no sub-rect |
| EASU intermediate | display (`target_size`) | `rgba16float` (hard-coded in WGSL) | `TEXTURE_USAGE_SAMPLING_BIT \| TEXTURE_USAGE_STORAGE_BIT` | 1 mip, 1 sample, one layer (reused per view) |
| output (RCAS out) | display | `rgba16float` (hard-coded in WGSL) | `TEXTURE_USAGE_STORAGE_BIT` (+ `SAMPLING_BIT` because the renderer blits it; the existing WebGPU intermediate has both, `renderer_scene_render_rd.cpp:889-899`) | 1 mip, 1 sample; **must be storage-capable** — WebGPU render-target/swapchain textures are not, the renderer already substitutes a `rgba16float` intermediate and blits (`:889-907`) |
| exposure placeholder (RCAS binding 2) | any (≥1×1) | any float format | sampled usage | not read at runtime in spatial mode; bind the scene color |

**No sampler is created or bound.** Both shaders only use `textureLoad(t, coords,
level 0)` / `textureStore`. There is no `textureSample*`, no derivative, so:
* filterability is irrelevant (RGBA16F is filterable anyway);
* no sampler, no address mode: border clamping is in the shader (`easuLoad`,
  `rcasLoad`) — binding a sub-rectangle or relying on a viewport offset would
  break the mapping;
* coordinates are i32 texel indices; level 0 only, so the bound views must be
  the exact mip 0 slice (one mip, one layer).

### 6.2 Color domain / placement

Milestone 1 puts the effect in the **existing FSR1 spatial-upscaler slot**, i.e.
after the tonemapper (and optional SMAA), per view, operating on the
display-referred tonemapped color that FSR1 consumes today
(`renderer_scene_render_rd.cpp:880-907`). Consequences:

* EASU/RCAS are filter/contrast operators; they make no presentation-transform
  assumption (upstream: "EASU expects perceptual input… callers are responsible
  for supplying the intended color domain"). The slot hands them exactly what
  FSR1 consumes today: tonemapped, bounded, perceptual/display-referred values —
  linear-encoded when HDR 2D output is on (`tonemap.convert_to_srgb =
  !using_hdr`, `renderer_scene_render_rd.cpp:767`), sRGB-encoded otherwise. The
  shaders treat either identically (no transfer-function assumption); no change
  to output transfer/gamma happens here.
* RCAS's spatial conditioning is well-defined on `[0,1]` values and exactly
  round-trips them; HDR-safety is not needed in this slot.
* The output stays in the same domain and is written as `rgba16float`; the
  renderer's existing blit to the render target does the presentation copy, so
  no double tonemap/encode is introduced (identical to the current FSR1 path).
* The upstream library's "canonical" domain is pre-tonemap linear HDR (its
  `upscaleSpatial` is a post-processing node). Feeding pre-tonemap HDR would
  require moving the upscale before tonemapping (FSR2's slot) and is explicitly
  **out of scope for milestone 1** (§11).

### 6.3 Constraints the shaders impose on the CPU

1. `renderSize`/`displaySize` in the UBO must be the **exact** texture dimensions
   of the bound mip/layer (EASU derives source coordinates from them; both
   shaders clamp with them).
2. `displaySize` is also the dispatch guard, so it must match the dispatch
   extent logically; the CPU may only dispatch `ceil(size/8)`.
3. Input must not be multisampled (compute bindings cannot be MSAA; the
   renderer's tonemapper destination is single-sample).
4. Storage format must be `rgba16float`; the WGSL hard-codes it. No sRGB view.
5. `FLAG_INPUT_REINHARD` clear (see §4.3).
6. Both passes must run in order per view: EASU then RCAS, with the
   intermediate written before it is sampled (Godot's compute list + the WebGPU
   driver's pass splitter handle this — §8.3).

### 6.4 Sharpness mapping (engine setting → `C.sharpness`)

Godot's FSR setting is `rendering/scaling_3d/fsr_sharpness`, default **0.2**
(`servers/rendering/rendering_server.cpp:3800`,
`render_scene_buffers_rd.h:86`), passed to FSR1 as AMD RCAS "stops"
(`fsr.cpp:73,98`; AMD: `con = exp2(−stops)`, `ffx_fsr1.h:659-667`).
pmndrs' spatial RCAS uses `peak = exp2(−2·(1−s))` (`rcas.ts:202`). These are the
same curve:

```
exp2(-2·(1 - s))  ==  exp2(-stops)      ⇔      s = 1 - stops/2
```

so use **`C.sharpness = clamp(1 - rb->get_fsr_sharpness()/2, 0, 1)`**: the
default 0.2 becomes 0.9, which yields exactly FSR1's default RCAS strength.
This also matches what the engine already computes for FSR2's RCAS
(`render_forward_clustered.cpp:2592`: `1 - fsr_sharpness/2`; AMD FSR2 remaps it
back to stops with `2 - 2·sharpness`, `thirdparty/amd-fsr2/ffx_fsr2.cpp:1015`,
and FSR2 documents `sharpness` as "0 = no additional sharpness, 1 = maximum",
`ffx_fsr2.h:132`). Reusing `fsr_sharpness` keeps one user-facing knob and gives
a directly comparable A/B against FSR1.

---

## 7. Per-frame CPU sequence (engine-agnostic)

No setup pass, no history, no jitter.

```
configure (viewport / render-buffer configure):
  renderSize  = internal (reduced) resolution
  displaySize = target  (display)  resolution
  create EASU intermediate  display×1, rgba16float, SAMPLING|STORAGE, 1 mip, 1 sample
  create constants ring buffers (256 B each, UNIFORM usage), pipelines

per frame, in the spatial-upscaler slot, per view:
  1. pick a fresh constants ring slot; write the 96-byte struct:
       renderSize, displaySize, renderSizeInv, displaySizeInv,
       sharpness = 1 - fsr_sharpness/2, flags = 0 (or 512 for denoise)
     (before any compute list is opened)
  2. begin compute list
  3. bind EASU pipeline + group0 {constants, scene_color_view, easu_out_view}
     dispatch ceil(displaySize.x/8) × ceil(displaySize.y/8) × 1
  4. barrier (Godot: compute_list_add_barrier; WebGPU: the driver splits the
     compute pass because EASU wrote easu_out and RCAS will sample it)
  5. bind RCAS pipeline + group0 {constants, easu_out_view,
       exposure_placeholder_view (= scene color), output_view, easu_out_view}
     dispatch the same group counts
  6. end compute list
  7. (renderer-owned) blit the rgba16float output to the non-storage render
     target — the existing WebGPU intermediate path already does this
```

The constants buffer content for EASU and RCAS is identical in a spatial frame
(same sizes/sharpness/flags), so one slot per view suffices for both
dispatches. Use a **ring** of slots (see §9.6) because several viewports and
frames can be encoded before one submit.

---

## 8. Dawn / WebGPU 1.0 compatibility

### 8.1 Feature audit of the vendored modules

Audited mechanically over `wgsl/easu.wgsl` and `wgsl/rcas_spatial.wgsl`
(static audit + upstream fingerprint match; the modules were **not** re-compiled
with Tint in this environment — the upstream browser/WebGPU test suite compiles
this exact assembly, and the on-engine smoke test in §9.9 is the first local
compile). The optional `wgsl/blit.wgsl` is the one exception to the sampler rule
below: it declares `var linearSampler : sampler` and calls
`textureSampleLevel` (bilinear, `blit.ts:31-50`), so adopting it adds a sampler +
filterable-input requirement; the two milestone-1 passes have none.

| feature | used? | note |
| --- | --- | --- |
| `enable` directives / language extensions | **no** | core WGSL 1.0 |
| f16 / `shader-f16` | **no** | all `f32`; runs on devices without the f16 feature |
| subgroups | **no** | |
| atomics | **no** | |
| workgroup memory (`var<workgroup>`) | **no** | workgroup storage size 0 |
| storage buffers | **no** | the constants block is a uniform buffer |
| samplers / `textureSample*` / derivatives | **no** | `textureLoad` only |
| `textureSampleLevel` | **no** | level 0 `textureLoad`; no filtering constraint |
| storage texture read | **no** | write-only (`write`) storage textures only; RCAS reads the EASU result as a *sampled* texture in the next dispatch |
| pointer params (`ptr<function, …>`) | yes | `easuSet`/`easuTap` — core WGSL, Dawn implements |
| builtins | `textureLoad`, `textureStore`, `inverseSqrt`, `exp2`, `select`, `floor`, `clamp`, `min/max`, `dot` | all core |
| entry point | `@compute @workgroup_size(8, 8) fn main` | 64 invocations; one `@compute` per module |
| bind groups | 1 (`@group(0)`) | EASU 3 bindings, RCAS 5 |

Limits: 8×8×1 = 64 invocations ≤ default 256;
`maxComputeWorkgroupStorageSize` 0; ≤3 sampled textures and 1 storage texture
per stage (defaults 16 / 4); 1 uniform buffer; 5 bindings in one group. The
engine also requests adapter maxima for compute and binding limits
(`platform/web/js/engine/engine.js:381-424`), so none of these is near a limit.

### 8.2 Binding-number convention (must be resolved with the raw-WGSL path)

The current WebGPU driver preprocesses every shader's SPIR-V so that
**non-combined bindings are doubled** (`spirv_preprocess.cpp:847-848,1341`), and
the bind-group-layout/bind-group entries it builds from reflection use
`binding * 2` (`rendering_device_driver_webgpu.cpp:5149,5164,5192`). The WGSL
that reaches Dawn therefore declares `@binding(0,2,4,6,8)`, and the driver's
WGSL scanner keys (`((set<<16) | binding)`) match the doubled numbers.

A hand-written WGSL module with `@binding(0,1,2,3,4)` will therefore **not**
match the BGL that a synthesized container's reflection produces (BGL would
declare 0,2,4,6,8) — WebGPU requires an exact match between the shader's
declared binding indices and the pipeline layout. Two clean resolutions:

* **(A) rewrite on load** — the raw-WGSL path text-substitutes each
  `@binding(N)` → `@binding(2N)` (groups unchanged) before module creation; keep
  the vendored files canonical and record the transform; or
* **(B) add a no-doubling mode** — the raw path marks the shader "bindings are
  literal" and the driver skips `*2` for it.

This must be settled in the driver design (task-3; delivered as
`docs/WGSL_COMPUTE_PATH.md` once available)
before the effect is coded; §9.3's reflection table uses the **WGSL-visible**
numbers (0..4), and the translation is the raw path's job. Do not silently edit
the vendored WGSL without documenting it (the fingerprints are the audit trail).

### 8.3 Read/write hazards and pass splitting

WebGPU does not allow one texture subresource to be both writable-storage and
sampled inside one compute-pass usage scope. The effect's pattern is exactly
that, one dispatch apart: EASU **writes** the intermediate, RCAS **samples** it
and **writes** the output. The driver already handles this: it records the
upcoming dispatch's texture usages from the *pipeline layout entries*
(`_note_compute_set_usage`, `rendering_device_driver_webgpu.cpp:9803-9826`;
writable storage ⇔ `storageTexture.access ∈ {WriteOnly, ReadWrite}`) and ends /
restarts the compute pass when a dispatch would read something the pass already
wrote (`_compute_pass_usage_guard`, `:9829-9880`). Requirements on the raw path:

* the storage-image bindings' BGL entries must have
  `storageTexture.access = WriteOnly` and `format = rgba16float` (derived from
  the WGSL text or the reflection `writable` flag — the WGSL scan reads
  `texture_storage_2d<rgba16float, write>`, `rendering_device_driver_webgpu.cpp:4181-4189,4891+`);
* the intermediate must be registered through a normal RD uniform set (so the
  driver's `bound_textures` bookkeeping sees it). Do **not** bypass
  `uniform_set_create` with driver-private bind groups: the pass splitter would
  not know about the write and Dawn would reject the pass.

Keeping `compute_list_add_barrier()` between the two dispatches is still
recommended (matches the existing FSR1 effect, `fsr.cpp:116`); on WebGPU it is
cheap and makes the intent explicit even though the driver decides the actual
pass boundary.

### 8.4 Other validation rules that can bite

| rule | why it can bite | mitigation |
| --- | --- | --- |
| storage texture format is in the WGSL text | destination/intermediate must be exactly `rgba16float`; an `rgba8unorm` LDR target or Mobile renderer cannot be bound | gate the effect on RGBA16F storage support + the FSR slot (both already true on WebGPU Forward+/HDR); fall back to FSR1 otherwise |
| storage textures must have `STORAGE_BINDING` | render-target textures are not storage-capable on WebGPU | the existing intermediate+blit path (`renderer_scene_render_rd.cpp:889-907`) already gives the effect a storage `rgba16float` destination |
| storage view = one mip level | a view with a mip range is invalid for storage | allocate 1-mip textures; pin `baseMipLevel:0, mipLevelCount:1` |
| sampled binding needs `TEXTURE_BINDING` | RD `uniform_set_create` rejects missing `SAMPLING` usage (`rendering_device.cpp:4928-4955` for sampled, `:4980-5035` for images) | create intermediate with both bits; scene color already has sampling |
| layout sampleType vs input format | `texture_2d<f32>` bound to `rgba32float` requires layout sampleType `unfilterable-float`; a `float` layout rejects unfilterable formats | milestone 1 input is RGBA16F (filterable); if a raw/unfilterable input is ever needed, declare the sampled bindings `unfilterable-float` |
| uniform buffer size | RD checks `buffer->size >= reflection.length` (`rendering_device.cpp:5110-5111`) | 256-byte buffer, reflection length 96 or 256 |
| uniform buffer update during compute list | RD rejects `buffer_update` while a compute list is active (`:1244-1247`) | write the constants before `compute_list_begin()` (§7 step 1) |
| MSAA input | compute can't bind multisampled textures | single-sample tonemapper destination is what the slot provides |
| NaN in RCAS limiter | `hitMax` divides by `4·mn4 − 4`; for a flat `1.0` ring that is 0/0. The spatial conditioning keeps taps < 1 so the denominator is negative (no 0/0) — does not occur for finite, conditioned taps | keep `FLAG_INPUT_REINHARD` clear; don't feed NaN/Inf into the slot |
| `textureLoad` out-of-range | out-of-bounds loads return 0 in WGSL but here every load is clamped in-shader | none needed; keep the view exactly texel-for-texel |
| dispatch overrun | extra groups would store out of bounds | shader guard + `ceil()` dispatch math (§3.2) |

---

## 9. C++ integration plan against Godot's `RenderingDevice`

Everything below is at the `RendererRD` / `RD::` level. The only new
infrastructure is the raw-WGSL shader/pipeline creation designed in parallel
(task-3; `docs/WGSL_COMPUTE_PATH.md` once delivered); the effect code otherwise uses the normal
RD compute API.

### 9.1 Where it plugs in

| item | file:line (verified in `/tmp/godot48`) |
| --- | --- |
| spatial-upscaler selection (FSR mode) | `servers/rendering/renderer_rd/renderer_scene_render_rd.cpp:485-499` |
| invocation point after scene render/tonemap, per view | `:880-907` |
| storage-capable intermediate + blit fallback (already generic for any `SpatialUpscaler`) | `:887-907` |
| interface to implement | `servers/rendering/renderer_rd/effects/spatial_upscaler.h:37-44` (`get_label`, `ensure_context`, `process`) |
| reference implementation / resource patterns | `servers/rendering/renderer_rd/effects/fsr.cpp:65-130` (UBO/ring, compute list, per-pass uniform sets, barrier, dispatch) |
| project settings registration | `servers/rendering/rendering_server.cpp:3795-3800` (`rendering/scaling_3d/mode`, `scale`, `fsr_sharpness`) |
| RGBA16F-storage probe pattern to copy for gating | `servers/rendering/renderer_viewport.cpp:171-189` (FSR2→FSR probe: `texture_is_format_supported_for_usage(DATA_FORMAT_R16G16B16A16_SFLOAT, TEXTURE_USAGE_STORAGE_BIT)`); `fsr_enabled` itself is only `!low_end && !disable_3d` (`:1048`) |
| engine feature enum (add a capability bit) | `servers/rendering/rendering_device_commons.h:1023-1041` (`RD::Features`) |

Proposed shape (milestone 1, minimal diff):

1. New class `PmndrsSpatialUpscaler : public SpatialUpscaler` (new files under
   `servers/rendering/renderer_rd/effects/`, e.g. `pmndrs_spatial.{h,cpp}`),
   built like `FSR`: create shaders + compute pipelines + constants ring in the
   constructor, `ensure_context()` allocates/reuses the intermediate,
   `process()` writes constants and dispatches.
2. Add project setting `rendering/scaling_3d/fsr1_implementation` (enum
   `amd,pmndrs`, default `amd`) next to `fsr_sharpness`
   (`rendering_server.cpp:3799-3800`) or a boolean; read it with `GLOBAL_GET`
   when the renderer creates the effect.
3. In `renderer_scene_render_rd.cpp`, when `scale_mode == FSR`, select the new
   effect instead of `fsr` if the setting says so **and** the device advertises
   the raw-WGSL capability (`RD::has_feature(RD::SUPPORTS_RAW_WGSL_COMPUTE)`,
   new bit, set by the WebGPU driver) **and** RGBA16F is
   storage+sampling capable (`texture_is_format_supported_for_usage`). Otherwise
   keep the existing `fsr` path unchanged (zero regression risk).
4. Construct the object only when `can_use_storage` (same guard that creates
   `RendererRD::FSR`, `renderer_scene_render_rd.cpp:1922`).

What the slot gives the effect for free (requires **no** renderer changes):

* source = `rb->get_texture_slice("Tonemapper"/"SMAA", "destination", v, 0)`
  (RGBA16F, display-referred, single-sample, sampling usage);
* destination = either the render target if it is storage-capable, or the
  renderer-created `FSR/upscaled_output` intermediate (RGBA16F,
  sampling|storage|color-attachment) followed by
  `copy_effects->copy_to_fb_rect(...)`;
* per-view loop and `ensure_context()` before it.

Per-view loop caveat: `process()` is called once per view with per-view source
and destination slice RIDs; the EASU intermediate can be a single-layer display
texture reused sequentially per view (as FSR1 reuses its `upscale_texture`), or
`view_count` layers sliced per view if that ever limits perf. Either way the
shader sees one 2D texture per dispatch — fine, because both shaders only use
`texture_2d`.

### 9.2 Raw-WGSL shader creation — the one dependency

The current RD path is SPIR-V → Tint → WGSL; the point of this port is to bypass
Tint for these hand-written modules. The forked driver must expose (per the
task-3 design; keep the effect code decoupled from its internals):

1. a way to create a **compute shader RID from raw WGSL + an explicit layout**
   (the existing shader container already carries per-stage WGSL —
   `drivers/webgpu/rendering_shader_container_webgpu.h:45,57,79`,
   consumed by `rendering_device_driver_webgpu.cpp:3974,4103` — so the cheapest
   route is a synthesized container with WGSL + reflection; task-3 owns this);
2. `RD::compute_pipeline_create(shader)` — unchanged;
3. `RD::uniform_set_create(uniforms, shader, 0)` — unchanged (RD validates
   against the shader's reflected uniform sets, `rendering_device.cpp:4799-4812`);
4. the normal compute-list API (`compute_list_begin`/`bind_compute_pipeline`/
   `bind_uniform_set`/`dispatch`/`add_barrier`/`end`,
   `rendering_device.h:1704-1713`);
5. a capability probe: new `RD::Features::SUPPORTS_RAW_WGSL_COMPUTE` set by the
   WebGPU driver (and left unset by Vulkan/Metal/D3D12) so the renderer can gate
   the setting and never call the path on other drivers.

If the design instead exposes a driver-private effect API, the effect class must
still be able to (2)-(4) with the normal RD objects — otherwise the renderer's
resource tracking (RD's draw graph, uniform-set validation, compute-pass
usage splitting) is bypassed and we lose the guarantees in §8.3.

### 9.3 Required reflection for the two shaders

Bindings as declared in WGSL (§3.1, §4.1). `stages = COMPUTE` everywhere;
array length 1 everywhere.

**EASU**

| binding | `ShaderUniform.type` | `writable` | `texture_type` | `texture_format` | `length` |
| --- | --- | --- | --- | --- | --- |
| 0 | `UNIFORM_TYPE_UNIFORM_BUFFER` | false | `TEXTURE_TYPE_MAX` | `DATA_FORMAT_MAX` | 96 (or 256; ≤256) |
| 1 | `UNIFORM_TYPE_TEXTURE` | false | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |
| 2 | `UNIFORM_TYPE_IMAGE` | **true** | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |

**RCAS**

| binding | `ShaderUniform.type` | `writable` | `texture_type` | `texture_format` | `length` |
| --- | --- | --- | --- | --- | --- |
| 0 | `UNIFORM_TYPE_UNIFORM_BUFFER` | false | `TEXTURE_TYPE_MAX` | `DATA_FORMAT_MAX` | 96 (or 256) |
| 1 | `UNIFORM_TYPE_TEXTURE` | false | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |
| 2 | `UNIFORM_TYPE_TEXTURE` | false | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |
| 3 | `UNIFORM_TYPE_IMAGE` | **true** | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |
| 4 | `UNIFORM_TYPE_TEXTURE` | false | `TEXTURE_TYPE_2D` | `DATA_FORMAT_R16G16B16A16_SFLOAT` | 1 |

`writable = true` on the storage images matters twice: RD validates the
`UNIFORM_TYPE_IMAGE` usage and the WebGPU driver's compute-pass splitter uses the
layout entry's write access (§8.3). The sampled bindings' sampleType must be
`float`/`unfilterable-float`, not `uint`/`sint`; with RGBA16F the driver resolves
`float`. Do not declare samplers (`SAMPLER_WITH_TEXTURE` would be wrong; the WGSL
has no sampler).

### 9.4 RD resources to create

| RID | RD call | parameters | lifetime |
| --- | --- | --- | --- |
| `shader_easu`, `shader_rcas` | raw-WGSL container creation (§9.2) + `shader_create_from_container`/new API | `wgsl/easu.wgsl`, `wgsl/rcas_spatial.wgsl`, entry `main`, reflection §9.3 | once, in the effect constructor; freed in dtor |
| `pipeline_easu`, `pipeline_rcas` | `RD::compute_pipeline_create(shader)` | no specialization constants | once; `RD::free_rid` in dtor |
| `constants[i]` ring | `RD::uniform_buffer_create(256)` (`rendering_device.h:1074`) | 256 bytes, UNIFORM usage (created by RD); zero-filled at create, updated per frame | once; freed in dtor. Ring of N slots (§9.6) |
| `easu_output` | `rb->create_texture(SNAME("PMNDRS"), SNAME("easu_output"), RD::DATA_FORMAT_R16G16B16A16_SFLOAT, RD::TEXTURE_USAGE_SAMPLING_BIT \| RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, rb->get_target_size(), 1, 1, true, true)` — mirror of FSR1's pattern (`fsr.cpp:75-81`; `RenderSceneBuffersRD::create_texture` at `render_scene_buffers_rd.h:219`) | display size, 1 layer, 1 mip, unique | owned by the render buffers; recreated automatically when the viewport resizes/reconfigures; use `has_texture()` guard in `ensure_context()` |
| uniform sets | `UniformSetCacheRD::get_cache(...)` (`uniform_set_cache_rd.h:149`) or `RD::uniform_set_create` (`rendering_device.h:1215`) with `RD::Uniform` (`UNIFORM_TYPE_UNIFORM_BUFFER`, `UNIFORM_TYPE_TEXTURE`, `UNIFORM_TYPE_IMAGE`; `rendering_device_commons.h:650-665`) | bindings in §3.1/§4.1 order | cache-managed per frame; no manual free with `UniformSetCacheRD` |
| destination | passed in by the renderer (`process(..., p_destination_texture)`); assert `texture_get_format(dest).usage_bits & STORAGE` and `format == DATA_FORMAT_R16G16B16A16_SFLOAT` | — | renderer-owned |

`TextureFormat`/usage constants: `rendering_device_commons.h:433-446,401-417`.

No sampler, no storage buffer, no push constant. (The constants must be a
uniform buffer because the WGSL declares `var<uniform>` and WebGPU has no push
constants; Godot's push-constant emulation is a separate hidden bind group and
not part of raw WGSL.)

### 9.5 Per-frame encoding (pseudo-code, per `process()` call = per view)

```cpp
void PmndrsSpatialUpscaler::process(Ref<RenderSceneBuffersRD> rb, RID p_source, RID p_dest) {
    // 0. sizes (cached; a resize recreates the render buffers, and has_texture()
    //    already handles the intermediate).
    Size2i render  = rb->get_internal_size();
    Size2i display = rb->get_target_size();
    RID easu_out   = rb->get_texture(SNAME("PMNDRS"), SNAME("easu_output"));

    // 1. constants: ring slot, written BEFORE the compute list opens
    //    (RD::buffer_update is forbidden while a compute list is active).
    FsrConstants c{};                       // 96 bytes, §5.2
    c.render_size[0] = render.x; c.render_size[1] = render.y;
    c.display_size[0] = display.x; c.display_size[1] = display.y;
    c.render_size_inv[0] = 1.0f / render.x;  c.render_size_inv[1] = 1.0f / render.y;
    c.display_size_inv[0] = 1.0f / display.x; c.display_size_inv[1] = 1.0f / display.y;
    c.sharpness = CLAMP(1.0f - rb->get_fsr_sharpness() / 2.0f, 0.0f, 1.0f);
    c.flags = 0;                            // optionally 512 = FLAG_RCAS_DENOISE
    RID ubo = _constants_ring[_ring_index++ % RING_SIZE];
    RD::get_singleton()->buffer_update(ubo, 0, sizeof(c), &c);

    RID src_view = p_source;                // renderer already hands per-view slices
    RID dst_view = p_dest;                  // rgba16f, STORAGE usage (renderer ensures)

    // 2. uniform sets (group 0); get_cache_vec takes a LocalVector<RD::Uniform>
    LocalVector<RD::Uniform> easu_set = {
        RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
        RD::Uniform(RD::UNIFORM_TYPE_TEXTURE,        1, src_view),
        RD::Uniform(RD::UNIFORM_TYPE_IMAGE,          2, easu_out),
    };
    LocalVector<RD::Uniform> rcas_set = {
        RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo),
        RD::Uniform(RD::UNIFORM_TYPE_TEXTURE,        1, easu_out),
        RD::Uniform(RD::UNIFORM_TYPE_TEXTURE,        2, src_view),   // unused placeholder
        RD::Uniform(RD::UNIFORM_TYPE_IMAGE,          3, dst_view),
        RD::Uniform(RD::UNIFORM_TYPE_TEXTURE,        4, easu_out),   // alpha source
    };
    RID easu_us = _uniform_cache->get_cache_vec(_shader_easu, 0, easu_set);
    RID rcas_us = _uniform_cache->get_cache_vec(_shader_rcas, 0, rcas_set);

    // 3. dispatch both passes
    uint32_t gx = (display.x + 7) / 8;
    uint32_t gy = (display.y + 7) / 8;
    RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
    RD::get_singleton()->compute_list_bind_compute_pipeline(list, _pipeline_easu);
    RD::get_singleton()->compute_list_bind_uniform_set(list, easu_us, 0);
    RD::get_singleton()->compute_list_dispatch(list, gx, gy, 1);
    RD::get_singleton()->compute_list_add_barrier(list);
    RD::get_singleton()->compute_list_bind_compute_pipeline(list, _pipeline_rcas);
    RD::get_singleton()->compute_list_bind_uniform_set(list, rcas_us, 0);
    RD::get_singleton()->compute_list_dispatch(list, gx, gy, 1);
    RD::get_singleton()->compute_list_end();
}
```

Define `FsrConstants` as a POD with the exact §5.2 offsets (e.g.
`float render_size[2]; float display_size[2]; …; uint32_t pad;`) and
`static_assert(sizeof(FsrConstants) == 96)`. No `alignas(16)`, no `bool`, no
`size_t` — the shader layout is the contract.

Notes:

* If the raw path needs the WGSL binding numbers doubled (§8.2), only the
  `RD::Uniform` binding numbers vs. shader text change as a matched pair; the
  *semantics* stay as in §3.1/§4.1/§9.3.
* `get_cache_vec` returns a cached uniform set keyed by shader+uniform IDs; when
  the render buffers are recreated on resize, the old textures are freed and
  the cache invalidates those sets automatically
  (`UniformSetCacheRD`, `uniform_set_cache_rd.h:149-190`).
* If the raw path is not ready to take the uniform-set machinery, use
  `RD::uniform_set_create` + `RD::free_rid` per frame (RD defers frees safely);
  either way the shader must be an RD `Shader` RID so the renderer's resource
  tracking works (§8.3).

### 9.6 Constants ring (multi-viewport / multi-frame safety)

`RD::buffer_update` is a CPU→staging→command-buffer copy tracked by RD's draw
graph (`rendering_device.cpp:1165-1240`). If two viewports (or two frames
encoded before one submit) both used the *same* constants buffer, both dispatches
would observe the last write. This is the same hazard FSR2 solves with a UBO
ring (`fsr2.cpp:179-181,435-436`). For milestone 1:

* allocate a ring of `N` uniform buffers (256 B each); `N` ≥ 2 × max
  (viewports × frames-in-flight) + slack — e.g. **16** is plenty (4 KB);
* pop a fresh slot per `process()` call; both passes of that call share it;
* wrap with `% N`. At that point a reused slot is at least N dispatches old, i.e.
  in a submitted-and-completed frame; if paranoid, size the ring per frame
  (`maxDispatchesPerFrame`, exactly what FSR2 does).

Alternative (not recommended for milestone 1): one ring buffer with dynamic
offsets requires `UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC` plus a raw-path
dynamic-offset story; keep it simple.

### 9.7 `ensure_context()` / lifecycle

* `ensure_context(rb)` is called once per viewport per frame, before the view
  loop (`renderer_scene_render_rd.cpp:883`). Do the allocation check there:
  `if (!rb->has_texture("PMNDRS","easu_output")) create ... display = rb->get_target_size()`.
  (The render buffers are re-created on viewport resize/configure; the
  `has_texture` guard is exactly FSR1's pattern, `fsr.cpp:75-83`.)
* Constructor: create shaders/pipelines/ring; nothing depends on the viewport.
* Destructor: free pipelines, shaders, ring buffers. Never free the
  `rb->create_texture` products (render buffers own them).
* Threading: RenderingDevice calls must be on the rendering thread
  (`ERR_RENDER_THREAD_GUARD` in RD), same as every other `RendererRD` effect.

### 9.8 Milestone-1 quality ratio / sizing

Use the existing FSR ratio plumbing, no new math:

* project settings for the A/B: `rendering/scaling_3d/mode = fsr`,
  `rendering/scaling_3d/scale = 1.0/1.5` (0.666…). FSR scales internal =
  `MAX(target * scale, 1)` and floors on assignment
  (`renderer_viewport.cpp:266-282`); at 960×540 this gives 640×360, matching the
  FSR tests in `docs/WEBGPU_HANDOFF.md` ("960x540, Quality 1/1.5").
* `renderSize = rb->get_internal_size()`, `displaySize = rb->get_target_size()` —
  the effect never computes a ratio itself; EASU handles arbitrary non-integer
  ratios.
* The upstream enum ratios (`QUALITY_MODE_RATIOS`, `resolution.ts:7-13`:
  NativeAA 1.0, Quality 1.5, Balanced 1.7, Performance 2.0,
  UltraPerformance 3.0) are only needed if we later surface FSR3-quality
  presets; milestone 1 inherits the engine's `scale`.

### 9.9 Acceptance checks for milestone 1

1. 0 `GPUValidationError` / uncaptured errors with the setting on
   (Forward+, TPS level, FSR mode, scale 1/1.5, MSAA off and 4× both).
2. Visual A/B against FSR1 at the same scale/sharpness: EASU edge quality, RCAS
   strength (s=1−fsr/2 mapping), alpha/transparent canvas unchanged.
3. Frame time at 960×540→640×360 and 1920×1080→1280×720 vs FSR1 in the same
   slot (fixed camera/rotation harness — the TPS level rotates, see handoff
   §"Quality/perf A/B").
4. Setting off ⇒ byte-identical to today's FSR1/bilinear path (no resource
   allocation, no extra passes).

---

## 10. Temporal path hooks (future work — not milestone 1)

The constants layout, the EASU shader and the RCAS production shader are already
temporal-capable; milestone 2 reuses them unchanged. What will be added:

| hook | where it comes from | milestone-1 state |
| --- | --- | --- |
| jitter (`jitter`, `jitterPrev`) | `UpscalerCore.beginFrame()`/`JitterSequence`; renderer applies to projection | not used; fields zero |
| `motionScale`, depth convention (`depthNearFar`, `FLAG_REVERSED_DEPTH`, `FLAG_PERSPECTIVE`) | `FrameCamera` + `_writeConstants` | zero / unused |
| history ping-pong, accumulate, reconstruct/depth-clip, exposure meter, shading-change, reactive, locks, guides | upstream temporal shaders + `UpscalerCore._encodeTemporal` | not vendored |
| `FLAG_INPUT_REINHARD`, `C.exposure`, real `exposureTex` | temporal exposure conditioning | flag clear; exposureTex bound as placeholder |
| blit pass with linear-`clamp-to-edge` sampler | `blit.ts` (vendored as `wgsl/blit.wgsl`), used when `sharpness == 0` | not needed yet; adds a sampler + filterable-input requirement |
| split frame (`dispatchGuides`/`dispatchUpscale`) | upstream guides API | not needed |

Design point: the 96-byte constants struct already contains every field the
temporal path needs, so no layout migration. The intermediate/ring/effect class
stay the same; the temporal milestone adds more passes and a second constants
stage (upstream keeps two buffers so an early guides stage and a late upscale
stage can share one encoder; see `ConstantsBuffer.ts:5-9`).

---

## 11. Risks (ranked) and mitigations

1. **The raw-WGSL driver path is a hard dependency and its binding conventions
   are subtle.** Without it there is no way to run these modules (translating
   WGSL→GLSL→SPIR-V→Tint would defeat the port and likely change numerics).
   Risks: binding-number doubling (§8.2), reflection fields that RD
   `uniform_set_create` validates against, and the storage-texture
   format/access declaration that the compute-pass splitter reads (§8.3).
   *Mitigation:* land the driver seam first (task-3, `docs/WGSL_COMPUTE_PATH.md`), with
   a smoke test that creates the two pipelines and does one dispatch each;
   validate 0 errors before wiring the renderer. Keep the AMD FSR1 path
   untouched as the fallback behind the setting.
2. **Constants staleness across viewports / encoded frames.** One shared 256-byte
   uniform buffer silently gives every viewport the last-written sizes (wrong
   EASU ratio, wrong dispatch guard). *Mitigation:* ring of per-`process()`
   buffers (§9.6), written before `compute_list_begin()`; add a 2-viewport test
   to the acceptance list.
3. **Color-domain/placement and format gating.** The upstream library's spatial
   path is documented for pre-tonemap linear HDR; milestone 1 runs it
   post-tonemap on display-referred RGBA16F. EASU is defined on perceptual input
   and RCAS conditions internally, but the A/B against FSR1 must confirm the
   sharpening look, and the effect must be gated off where the FSR slot doesn't
   provide RGBA16F storage capability (LDR target, Mobile renderer, non-WebGPU
   drivers, or a missing raw-WGSL feature) with a clean fallback to FSR1.
   *Mitigation:* gate + fallback in §9.1; A/B captures; keep `fsr_sharpness`
   mapping (§6.4) so RCAS strength is directly comparable.

Secondary: multi-view (single-layer intermediate reused sequentially — works,
but if a future backend requires per-view intermediates, allocate layers);
`sharpness == 0` semantics (upstream uses the blit, we run RCAS at s=0 —
§12 Q4); performance (upstream's own benchmarks say the WGSL port defers
`textureGather`/f16/bind-group-cache optimizations — do not add them for
milestone 1).

---

## 12. Questions the implementers must answer (or get answered)

1. **Raw-WGSL path contract (owner: driver-design / task-3).** Exact RD API for
   "compute shader from raw WGSL + reflection"; and does the raw path expect the
   WGSL to declare the driver's doubled binding numbers (`0,2,4,…`) or will it
   rewrite them? The effect code can't be finalized until this is fixed. If the
   answer is "rewrite", we need one generated copy of the two vendored modules
   and a documented transform (fingerprints preserved for the canonical files).
2. **Placement/domain sign-off.** Confirm milestone 1 stays in the FSR1
   (post-tonemap) slot rather than moving the upscale before tonemapping
   (FSR2 slot / linear HDR). The former is minimal and matches "instead of
   FSR1"; the latter is the upstream library's intended domain and would change
   where tone mapping happens.
3. **Setting surface + sharpness mapping.** Do we expose
   `rendering/scaling_3d/fsr1_implementation = pmndrs` (and reuse
   `fsr_sharpness` with `s = 1 − x/2`), or a separate boolean / separate
   sharpness value? What is the default (recommend: off until A/B passes)?
4. **`sharpness == 0` semantics.** Run RCAS with `C.sharpness = 0` (≈ −2 stops
   of lobe, not a passthrough) or implement upstream's EASU→blit path for exact
   "no sharpening"? The blit WGSL is vendored (`wgsl/blit.wgsl`, needs a linear
   clamp-to-edge sampler + filterable input) if exact parity is required.
   Milestone-1 recommendation: always RCAS, document the small non-zero lobe;
   revisit if a "no sharpen" UI toggle exists.
5. **Fallback matrix.** On non-WebGPU drivers, no raw-WGSL feature, non-RGBA16F
   base format, Mobile renderer, or unsupported multi-view assumptions: fall
   back silently to AMD FSR1 (recommended) and warn once, or disable scaling?
6. **Perf target.** What frame-time/PSNR deltas count as "parity with FSR1" for
   the milestone? (Handoff has a fixed-camera harness and an FSR2 baseline;
   FSR1 baseline + pmndrs numbers need to be recorded in the same run.)

---

## 13. Evidence index (file:line, pinned upstream commit unless noted)

| claim | evidence |
| --- | --- |
| pinned commit / version | `/tmp/pmndrs-upscaler` `git log -1` → `f6e5209…` "release: v0.6.0" |
| no `.wgsl` files upstream; TS-assembled WGSL | `find /tmp/pmndrs-upscaler -name '*.wgsl'` empty; `packages/core/src/shaders/wgsl.ts:1-27`; shaders README line 3 |
| assembled-module fingerprints | `packages/core/src/shaders/shaders.test.ts:89-99` (easu `48248d62`, rcas `b0405308`); recomputed in `/tmp/pmndrs-vendor/tools/extract-shaders.mjs` |
| EASU bindings / workgroup / guard | `packages/core/src/shaders/easu.ts:39-43,111-113` |
| RCAS bindings / workgroup / guard / conditioning | `packages/core/src/shaders/rcas.ts:166-182,200-217,296-314`; assembled `rcas_spatial.wgsl:44-63,116-178` |
| spatial pass graph | `UpscalerCore.ts:526,994-996,1198-1237` |
| RCAS spatial uniform-set order | `UpscalerCore.ts:1603-1610` |
| EASU intermediate allocation (display, rgba16float, sampling and storage) | `UpscalerCore.ts:1762-1764,1702-1712` |
| no jitter on spatial path | `UpscalerCore.ts:795-797,920` |
| constants buffer 256 B / 96 B payload / binding 0 | `ConstantsBuffer.ts:11-13,25-34`; `common.ts:30-49`; shaders README line 3 |
| constants staging (fields, flags) | `UpscalerCore.ts:1653-1700` |
| quality ratios / render-size floor | `packages/core/src/math/resolution.ts:7-35` |
| default settings (sharpness 0.8 etc.) | `UpscalerCore.ts:430-437` |
| renderer spatial slot (selection, ordering, intermediate+blit) | `renderer_scene_render_rd.cpp:485-499,790,880-907` (engine tree `582d619026`) |
| RGBA16F preferred color format | `renderer_scene_render_rd.cpp:1197-1199` |
| FSR1 reference effect (UBO, uniform sets, dispatch, barrier) | `effects/fsr.cpp:65-130` |
| spatial upscaler interface | `effects/spatial_upscaler.h:37-44` |
| `fsr_sharpness` setting + default 0.2 | `servers/rendering/rendering_server.cpp:3799-3800`; `render_scene_buffers_rd.h:86` |
| FSR1 raw sharpness (stops) | `effects/fsr.cpp:73,98`; AMD `ffx_fsr1.h:659-667` |
| FSR2 sharpness mapping `1−x/2` and AMD remap | `render_forward_clustered.cpp:2592`; `thirdparty/amd-fsr2/ffx_fsr2.cpp:1015`, `ffx_fsr2.h:132` |
| pmndrs `peak = exp2(−2(1−s))` | `rcas.ts:201-202` |
| RD texture format/usage/uniform enums | `rendering_device_commons.h:401-446,650-665,1023-1041,1119-1126` |
| RD compute API | `rendering_device.h:481,1074,1215,1302,1704-1713` |
| RD buffer-update restrictions | `rendering_device.cpp:1241-1256` (update forbidden inside an open list); staging/graph copy `:1165-1240` |
| RD uniform-set validation (sampled/image/UBO) | `rendering_device.cpp:4799-4812` (entry), `:4928-4955` (`UNIFORM_TYPE_TEXTURE`: type + `SAMPLING` usage), `:4980-5035` (`UNIFORM_TYPE_IMAGE`: type + `STORAGE` usage), `:5110-5111` (UBO size) |
| RGBA16F storage probe (to copy for gating) | `renderer_viewport.cpp:171-189`; `fsr_enabled` meaning `:1048` |
| web device limit requests | `platform/web/js/engine/engine.js:381-424` |
| per-stage WGSL container already exists | `drivers/webgpu/rendering_shader_container_webgpu.h:45,57,79`; `rendering_device_driver_webgpu.cpp:3974,4103` |
| binding doubling (driver convention) | `drivers/webgpu/spirv_preprocess.cpp:847-848,1341`; `rendering_device_driver_webgpu.cpp:5149,5164,5192` |
| storage format/access scan from WGSL | `rendering_device_driver_webgpu.cpp:4181-4189,4891+`, BGL use `:5249-5260` |
| compute-pass usage guard / splitter | `rendering_device_driver_webgpu.cpp:9803-9826,9829-9880` |
| UBO ring precedent | `effects/fsr2.cpp:179-181,435-436`; `effects/fsr2.h:53,115-116` |
| renderer_viewport FSR scale→internal size | `renderer_viewport.cpp:266-282` |

Engine-tree paths are relative to `/tmp/godot48` (HEAD `582d619026`, not
committed); verify line numbers there before editing — the tree is volatile.
