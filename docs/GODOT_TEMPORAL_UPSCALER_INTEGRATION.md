# Temporal 3D upscaler integration guide (pmndrs temporal, Viewport scaling mode 7)

> **STATUS (2026-11-28): IMPLEMENTED, THEN REVERTED — NOT SHIPPED.** Mode 7 and
> the temporal effect were built, exported and browser-verified in automation,
> then **fully removed from the engine** after the owner's real-browser test
> showed severe ghosting. This document is **archived research** for a future
> retry (milestone 2b); no mode-7/temporal code exists in the current stack.
> The required retry follow-ups (reactive mask, shading-change detector,
> camera-cut reset, deterministic jitter/velocity validation, ground-truth A/B
> harness — FSR 2 is not usable on WebGPU — human Subjective review) are listed
> in `PMNDRS_TEMPORAL_PORT.md` and in the `WEBGPU_HANDOFF.md` milestone-2
> history section. The "As built" section below is **historical**.

Engine-side integration contract for **milestone 2** of the pmndrs/upscaler port: a
temporal upscaler that plugs in **where FSR2 does** in `RenderForwardClustered`, as a
first-class `Viewport` scaling mode (`VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL`, value 7),
running on the raw-WGSL compute path (WebGPU only).

This document is the **engine seam only**: where the effect is called, what it receives,
how the jitter/velocity/depth/exposure contracts are wired, and every renderer/viewport/
driver site that must learn about mode 7. The effect's internal pass graph, vendored WGSL,
constants layouts and dispatch math are the sibling deliverable
`docs/PMNDRS_TEMPORAL_PORT.md` (assumed below as the "port contract"; every place this doc
depends on a detail from it is marked **[port contract]**).

| field | value |
| --- | --- |
| engine tree | `/tmp/godot48` (Godot 4.8.0-beta + Fast Godot patch stack), HEAD `582d619026` + uncommitted worktree changes; all `file:line` refs verified there |
| upstream | pmndrs/upscaler `f6e52098edffb66e16298a30c53b4da37b4b7d7f` (`v0.6.0`) |
| existing milestone 1 | `docs/PMNDRS_UPSCALER_SPATIAL.md` (spatial EASU/RCAS, mode 6), `docs/WGSL_COMPUTE_PATH.md` (raw-WGSL RD path) |
| FSR2 reference implementation | `thirdparty/amd-fsr2` wired in `servers/rendering/renderer_rd/effects/fsr2.{h,cpp}` + `render_forward_clustered.cpp` |
| status | **implemented then REVERTED — NOT SHIPPED (2026-11-28)**; archived research for a future retry. The "As built" section below is historical |

`file:line` refs are relative to the engine tree `/tmp/godot48` unless they name a
workspace doc (`docs/...`). **The worktree is live**: parallel tasks are already landing
the effect/driver work this guide describes (at the time of writing:
`servers/rendering/renderer_rd/effects/pmndrs_temporal.h` exists as an in-progress draft,
and `drivers/webgpu/rendering_device_driver_webgpu.cpp` carries the Gap A/Gap B fixes —
see §5.2). Line numbers in those files were re-verified against the worktree at
2026-11-28 00:05; treat the quoted symbols as the anchor if a file drifts by a few lines.

---

## Historical as-built (2026-11-28) — REVERTED, NOT SHIPPED

> The code described in this section was removed on 2026-11-28 after the
> owner's browser test showed severe ghosting; it is kept only as a record for a
> retry. Do not treat it as the current engine seam.

Milestone 2 was **implemented, exported and browser-verified in automation** at the time. The implemented seam
matches this guide's FSR2-slot proposal: mode 7
(`Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL` /
`RenderingServer.VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL` = 7, appended after
mode 6 so old values are unchanged, `*_MAX` = 8, classified
`VIEWPORT_SCALING_3D_TYPE_TEMPORAL`, hint `pmndrs (Temporal - Slow):7`) is
bound/registered/documented, guarded to Forward+, and the viewport layer probes the
device **before** the render buffers are configured (fallback FSR2 → FSR1 → 3D
scaling off), with a renderer-side last-line fallback in
`render_forward_clustered.cpp`. The effect runs pre-tonemap inside
`RenderForwardClustered` in the FSR2 slot with a per-render-buffers
`PmndrsTemporalContext` (history/locks/exposure ping-pong, dilated depth/motion/
masks, atomic depth-scatter buffers), writing `rb->get_upscaled_texture(v)`
before tonemapping. See `WEBGPU_HANDOFF.md` (milestone-2 section) and
`PMNDRS_TEMPORAL_PORT.md` (status section + contract) for the full record.

**Where the implementation diverged from this proposal:**

- **Jitter ownership**: not moved into the effect (no `begin_frame()` ownership
  refactor). The engine's Halton jitter is reused: the renderer passes
  `scene_data->taa_jitter` / `prev_taa_jitter` × internal size × 0.5 (FSR2-style
  render pixels) and the effect maps them to its x-right/y-down convention as
  `(x, -y)`. `motionScale = (-1, -1)` for Godot's UV-delta prev−cur velocity.
- **Depth**: `reversed_depth = true` — Godot 4.8 Forward+ is reversed-Z. The
  original `PMNDRS_TEMPORAL_PORT.md` §5.6 standard-depth assumption was an
  erratum, now corrected inline.
- **Velocity clear**: mode 7 clears the motion-vector buffer to `(0,0,0,0)` per
  frame instead of FSR2's `(-1,-1,0,0)`.
- **MSAA**: 3D MSAA is forced off for mode 7 with a one-time warning (the
  reconstruct/depth-clip graph has no MSAA path); TAA is disabled by the
  temporal-mode handling.
- **Context API**: as proposed — `create_context` / `free_context` / `upscale`
  with a `Parameters` struct. `exposure` and `reactive` are optional RIDs and are
  null in the TPS v1 path (the effect self-meters exposure; no reactive mask on
  the WebGPU v1 path).
- **No host exposure / reactive / guides inputs in v1**, and multiview/XR is
  unsupported (`view_count == 1`).

**Known limitations — and the reason for the revert** (also in the handoff):
no reactive mask (transparent/particle ghosting), no shading-change detector
(animated-lighting ghosting), sky/background ghosting under camera rotation
(zero motion), no camera-cut reset signal (history resets only on render-buffer
reconfigure/resize/mode switch), single-view only, and the deferred passes
listed in `PMNDRS_TEMPORAL_PORT.md`. The owner's real-browser test showed
severe ghosting despite the clean automated captures, and the whole temporal
path (mode 7 + effect + driver extensions) was removed rather than tuned. A
retry must cover these limitations and add a human-A/B gate. Verification evidence:
`/tmp/temporal_run2_console.log`, `/tmp/temporal_move_console.log`,
`/tmp/fsr1_move_console.log`, `/tmp/ab_console.log`,
`/tmp/temporal_run2_settings.png`, `/tmp/temporal_move_*.png`, harnesses
`/tmp/node-test/tps_pmndrs_temporal.js`, `tps_temporal_motion.js`,
`tps_temporal_ab.js`.

---

## 0. TL;DR

1. **The FSR2 seam is exactly the right hole.** `RenderForwardClustered::_render_scene()`
   runs the temporal upscaler **after** the transparent pass and MSAA resolves, **before**
   tonemapping (`render_forward_clustered.cpp:2572-2653`), writing into
   `rb->get_upscaled_texture(v)`. `RendererSceneRenderRD::_render_buffers_post_process_and_tonemap()`
   then tonemaps from that texture when `use_upscaled_texture` is true
   (`renderer_scene_render_rd.cpp:484,514-515,838`). Mode 7 must be added to that
   `use_upscaled_texture` expression.
2. **Jitter**: the renderer computes a Halton jitter per viewport, applies it to the camera
   projection in `RenderSceneDataRD::update_ubo()` (`render_scene_data_rd.cpp:85`
   `correction.add_jitter_offset(taa_jitter)`), and the motion-vector shader **subtracts**
   current/previous jitter from the clip positions
   (`scene_forward_clustered.glsl:3216-3223`), i.e. motion vectors are already
   jitter-free. To let the pmndrs temporal effect own jitter, **override
   `scene_data.taa_jitter` / `scene_data.prev_taa_jitter` in
   `RendererSceneRenderRD::render_scene()` (before the virtual `_render_scene()` call at
   `renderer_scene_render_rd.cpp:1559`)** with values derived from
   `pmndrs->begin_frame()`; convert the returned pixel jitter (x right, y down) to
   Godot's clip-space offset as `taa_jitter = (-2*jx/W, -2*jy/H)` (§2.3 derives the signs
   from the `flip_y` correction plus the WebGPU driver's output-Y negation). Motion vectors
   need **no shader change**.
3. **Resources are all already there at render resolution**: internal colour (RGBA16F,
   linear pre-tonemap), depth (D32_SFLOAT_S8_UINT / D24_S8, or R32F on the MSAA-resolve
   path), velocity (RG16F), 1×1 R32F luminance for exposure, and a target-size RGBA16F
   upscaled texture (`render_scene_buffers_rd.cpp:721-728`). The reactive mask is **not**
   usable on WebGPU (alpha-swizzled view is a no-op — no texture swizzles in WebGPU).
   History (ping-pong colour at render res, previous depth, locks/guides) must be allocated
   persistently per `RenderSceneBuffersRD`, not per frame.
4. **Scaling mode 7** needs ~12 small edits, all enumerated with snippets in §4: enum +
   type helper, bindings and property/project-setting hints, viewport guards/fallbacks/
   size switch/MSAA policy, the `RenderForwardClustered` `scale_type` switch, the
   `use_upscaled_texture` expression, and the new effect member.
5. **WebGPU driver gaps** (§5): the raw-WGSL path already does uniform buffers, sampled
   `texture_2d<f32>`, and write-only `rgba16float` storage images (that is what milestone 1
   exercised). The temporal passes additionally needed (a) **standalone sampled depth
   textures** (`texture_depth_2d`) — the driver used to bind the default `aspect=All` view
   of a depth-stencil texture, which WebGPU rejects; (b) **unfilterable-float sample types**
   for `r32float` guide textures sampled via `texture_2d<f32>`. Both (a)/(b) are being
   fixed in the worktree as **Gap B / Gap A** of `docs/PMNDRS_TEMPORAL_PORT.md` §12.3
   (auto-layout sample types; `_depth_only_view()`); the guide below documents the failure
   modes and the smoke tests. Still required: (c) **read/write storage buffers with
   `atomic<u32>`** (RD/driver code paths exist but no raw-WGSL shader has exercised them —
   smoke-test required); (d) separate `sampler` bindings (code exists, unexercised);
   read/read_write storage textures are **not** needed (all storage outputs are write-only),
   and B10G11R11 is not used, so the FSR2 blocker does not apply.
6. **Design**: `RendererRD::PmndrsTemporal` owned by `RendererSceneRenderRD` (like
   `pmndrs_spatial`), with a per-rb `ViewState : RenderBufferCustomDataRD` that
   `RenderSceneBuffersRD::configure()`/`cleanup()` re-create/free automatically.
   `begin_frame(rb)` is called from `RendererSceneRenderRD::render_scene()`; `upscale(params)`
   from `RenderForwardClustered::_render_scene()` in the FSR2 branch position. §6 has the
   class surface and the full `Parameters`; §7 has the ordered step list; §8 the risks.

---

## 1. FSR2 end-to-end (the reference seam)

FSR2 is the only temporal upscaler wired in this tree. Everything below is the map a new
temporal effect must copy or deliberately deviate from.

### 1.1 Per-rb context allocation: `ensure_fsr2`

`RenderBufferDataForwardClustered` keeps the FSR2 context (an opaque `FSR2Context*`):

- declaration: `render_forward_clustered.h:98` (`RendererRD::FSR2Context *fsr2_context = nullptr;`),
  accessor `get_fsr2_context()` at `render_forward_clustered.h:147`
- lazy allocation: `render_forward_clustered.cpp:93-97`

```cpp
void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_fsr2(RendererRD::FSR2Effect *p_effect) {
	if (fsr2_context == nullptr) {
		fsr2_context = p_effect->create_context(render_buffers->get_internal_size(), render_buffers->get_target_size());
	}
}
```

The context is **per render-buffer** state; it is destroyed when the custom data is freed
(`render_forward_clustered.cpp:118-149`, `memdelete(fsr2_context)` at 134-137). The custom
data object itself is created in `setup_render_buffer_data()`
(`render_forward_clustered.cpp:287-295`) and re-`configure()`d by
`RenderSceneBuffersRD::configure()` (`render_scene_buffers_rd.cpp:211-213`), which calls
`cleanup()` first (`render_scene_buffers_rd.cpp:155-214`, cleanup at 182). This is the lifetime pattern to
reuse: **resize = free + reconfigure = history reset**.

`create_context()` (`fsr2.cpp:875-907`) receives only `internal_size` and `target_size`;
the ffx backend allocates its own internal textures (via the `fpCreateResource` callback)
and its uniform-buffer ring is `FSR2_UBO_RING_BUFFER_SIZE`
(`fsr2.h:50-53`, `FSR2_MAX_QUEUED_FRAMES × FFX_FSR2_PASS_COUNT × FSR2_MAX_UNIFORM_BUFFERS`).
The FSR2 *Effect* itself (shaders/pipelines/samplers) is owned by
`RenderForwardClustered` alongside `taa` (`render_forward_clustered.cpp:5445`, destroyed at
5464-5467).

### 1.2 `scale_type`: where the temporal mode is selected in the 3D pass

Inside `RenderForwardClustered::_render_scene()` (starts `render_forward_clustered.cpp:1782`),
after `rb_data`, reflection-probe flags and `_update_vrs`:

- `render_forward_clustered.cpp:1859-1866`:
  `using_debug_mvs`, `using_taa = rb->get_use_taa()`, and the local enum
  `SCALE_NONE / SCALE_FSR2 / SCALE_MFX`.
- `render_forward_clustered.cpp:1868-1888`: `switch (rb->get_scaling_3d_mode())`.
  FSR2 case is guarded by `fsr2_effect && fsr2_effect->is_supported()`; otherwise it prints
  one warning and leaves `SCALE_NONE` (no upscaler). This guard is the **last line of
  defence** because the viewport layer already falls back when the device cannot run FSR2
  (see §4.3).
- `render_forward_clustered.cpp:1890`: `bool using_upscaling = scale_type != SCALE_NONE;`
  — everything downstream (motion-vector requirement, motion pass, MSAA resolves,
  `ensure_upscaled`, the upscale dispatch) hangs off this single bool.

### 1.3 Motion-vector requirement logic

`render_forward_clustered.cpp:1892-1907`:

```cpp
bool motion_vectors_required;
if (using_debug_mvs) {
	motion_vectors_required = true;
} else if (ce_needs_motion_vectors) {
	motion_vectors_required = true;
} else if (!is_reflection_probe && using_taa) {
	motion_vectors_required = true;
} else if (!is_reflection_probe && using_upscaling) {
	motion_vectors_required = true;
} else {
	motion_vectors_required = false;
}
p_render_data->scene_data->calculate_motion_vectors = motion_vectors_required;
```

Consequences wired automatically for any new `SCALE_*` value:

- `render_forward_clustered.cpp:1949-1955`: when `calculate_motion_vectors` the colour pass
  flags get `COLOR_PASS_FLAG_MOTION_VECTORS`, the advanced shader group is enabled and
  `global_pipeline_data_required.use_motion_vectors = true`.
- `render_forward_clustered.cpp:1928`: `bool using_motion_pass = rb_data.is_valid() && using_upscaling;`
  → the opaque list is split into a separate colour pass and motion pass
  (`render_forward_clustered.cpp:2320-2350`); `_fill_render_list()` takes
  `p_using_motion_pass` (`render_forward_clustered.cpp:941`, used at 1094 and 1177-1178 to
  tag surfaces with `COLOR_PASS_FLAG_MOTION_VECTORS`).
- The velocity texture is created on demand:
  `RenderBufferDataForwardClustered::get_color_pass_fb()` calls
  `render_buffers->ensure_velocity()` when the motion-vector flag is set
  (`render_forward_clustered.cpp:199-203`), and `RenderSceneBuffersRD::ensure_velocity()`
  allocates `RB_TEX_VELOCITY` (+ MSAA sibling) at internal size
  (`render_scene_buffers_rd.cpp:730-740`).
- Motion vectors are cleared to the FSR2 "invalid" sentinel `(-1,-1,0,0)` before the motion
  pass (`render_forward_clustered.cpp:2333-2337`), because FSR2 defines
  `FFX_FSR2_OPTION_GODOT_DERIVE_INVALID_MOTION_VECTORS` and reconstructs those pixels from
  depth + reprojection (`fsr2.cpp:536-543`;
  `thirdparty/amd-fsr2/shaders/ffx_fsr2_callbacks_glsl.h:369-378`).
  **A new temporal effect that does not implement that sentinel must clear to `(0,0,0,0)`
  instead** — otherwise sky/background pixels report a bogus 1-texel motion (§2.6, §8).

### 1.4 The `upscale(params)` call site — every field and where it comes from

`render_forward_clustered.cpp:2572-2653` (inside `_render_scene`, after MSAA resolve and
post-transparent compositor effects, before `_debug_draw_cluster`/tonemap):

```cpp
if (rb_data.is_valid() && (using_upscaling || using_taa)) {
	if (scale_type == SCALE_FSR2) {
		rb_data->ensure_fsr2(fsr2_effect);                       // 2574

		RID exposure;
		if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
			exposure = luminance->get_current_luminance_buffer(rb);   // 2576-2579
		}

		RD::get_singleton()->draw_command_begin_label("FSR2");
		RENDER_TIMESTAMP("FSR2");

		for (uint32_t v = 0; v < rb->get_view_count(); v++) {         // 2584
			real_t fov = p_render_data->scene_data->cam_projection.get_fov();
			real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
			real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);  // 2585-2587
			Vector2 jitter = p_render_data->scene_data->taa_jitter * Vector2(rb->get_internal_size()) * 0.5f; // 2588
			RendererRD::FSR2Effect::Parameters params;
			params.context = rb_data->get_fsr2_context();          // 2590
			params.internal_size = rb->get_internal_size();        // 2591
			params.sharpness = CLAMP(1.0f - (rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f); // 2592
			params.color = rb->get_internal_texture(v);            // 2593  pre-tonemap RGBA16F
			params.depth = rb->get_depth_texture(v);               // 2594  hardware depth (resolved on MSAA)
			params.velocity = rb->get_velocity_buffer(false, v);   // 2595  RG16F render res
			params.reactive = rb->get_internal_texture_reactive(v);// 2596  alpha-swizzled view (!)
			params.exposure = exposure;                            // 2597  1x1 R32F or invalid RID
			params.output = rb->get_upscaled_texture(v);           // 2598  RGBA16F target res
			params.z_near = p_render_data->scene_data->z_near;     // 2599
			params.z_far = p_render_data->scene_data->z_far;       // 2600
			params.fovy = fovy;                                    // 2601
			params.jitter = jitter;                                // 2602  pixels, render res
			params.delta_time = float(time_step);                  // 2603  seconds
			params.reset_accumulation = false; // FIXME: The engine does not provide a way to reset the accumulation. (2604)

			Projection correction;
			correction.set_depth_correction(true, true, false);    // 2606-2607

			const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;   // 2609
			const Projection &cur_proj = p_render_data->scene_data->cam_projection;         // 2610
			const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;// 2611
			const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;    // 2612
			params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse(); // 2613

			fsr2_effect->upscale(params);                          // 2615
		}
		RD::get_singleton()->draw_command_end_label();
	} else if (scale_type == SCALE_MFX) { ... }                    // 2619-2647
	else if (using_taa) { taa->process(...); }                    // 2648-2653
}
```

Then `_render_buffers_post_process_and_tonemap()` is called at
`render_forward_clustered.cpp:2656-2662`.

Notes that a new temporal effect inherits:

- **Order**: after the transparent pass (2510-2528), after MSAA resolve (2530-2558) and
  post-transparent compositor effects (2567-2570). This is the pre-tonemap, pre-glow,
  pre-DoF scene colour, exactly what a temporal upscaler wants.
- **Per view**: the loop runs over `rb->get_view_count()` layers, but FSR2 uses one
  context; multiview is effectively untested. A new effect should gate `view_count == 1`
  for v1 (§8).
- **Exposure** is only non-null when auto-exposure is enabled
  (`luminance->get_current_luminance_buffer(rb)` returns the current 1×1 R32F *luminance*
  buffer, `luminance.cpp:150-157`); FSR2 passes it as the ffx `exposure` resource.
- **`reset_accumulation` is hard-coded false** — this is an engine gap a new effect should
  fix (see §6.5 reset handling).

`FSR2Effect::Parameters` (the struct a new effect mirrors) is at `fsr2.h:156-173`:

```cpp
struct Parameters {
	FSR2Context *context;      // per-rb state
	Size2i internal_size;      // render res
	RID color;                 // render-res RGBA16F, linear pre-tonemap
	RID depth;                 // depth attachment (or R32F resolve on MSAA)
	RID velocity;              // RG16F render res, UV-space prev-cur (see §2)
	RID reactive;              // optional
	RID exposure;              // optional 1x1 R32F
	RID output;                // target-size RGBA16F
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	Vector2 jitter;            // PIXELS at render res (taa_jitter * internal_size * 0.5)
	float delta_time = 0.0f;   // seconds
	float sharpness = 0.0f;
	bool reset_accumulation = false;
	Projection reprojection;   // unjittered prev/cur projection+transform sandwich
};
```

### 1.5 `FSR2Effect` internals: parameters → ffx dispatch, passes, uniform ring

- Class shape (`fsr2.h:127-201`): `RootSignature`/`Pipeline`/`Pass` proxies map Godot RD
  objects onto ffx callbacks; one `Pass` per `FFX_FSR2_PASS_*` holds the shader version,
  pipeline and manual `FfxResourceBinding` lists (sampled/storage/uniform).
- Constructor `fsr2.cpp:516-861`: first probes
  `RD::texture_is_format_supported_for_usage(DATA_FORMAT_B10G11R11_UFLOAT_PACK32, STORAGE)`
  (`fsr2.cpp:523`). On WebGPU that is **false** → the effect sets `supported=false`,
  prints one warning and returns before compiling any shader (`fsr2.cpp:524-527`). This is
  the current WebGPU fallback: the *renderer* then keeps its previous upscaler (§4.3).
  Additional WebGPU guards: no image atomics / no subgroup defines
  (`fsr2.cpp:545-558`), fp16 shader variants disabled when
  `SUPPORTS_HALF_FLOAT` is false (`fsr2.cpp:579-587`).
- Passes are dispatched through `ffxFsr2ContextDispatch()` in `upscale()`
  (`fsr2.cpp:909-952`): 17 parameter fields are translated to
  `FfxFsr2DispatchDescription` (colour/depth/motion/reactive/exposure/output RIDs at
  912-925, jitter at 927-928, `motionVectorScale = internal_size` at 929-930, render size
  at 932-933, sharpness at 934-935, `frameTimeDelta` seconds at 936, near/far/fovy at
  938-940, reprojection matrix stored at 948, dispatch at 950).
- Uniform ring: ffx calls back into `schedule_gpu_job_rd`/`execute_gpu_jobs_rd`
  (`fsr2.cpp:895-896`); per-frame constants buffers come from
  `FSR2Context::Scratch::ubo_ring_buffer[FSR2_UBO_RING_BUFFER_SIZE]` indexed by
  `ubo_ring_buffer_index` (`fsr2.h:112-118`, macros at `fsr2.h:50-53`). A new effect needs
  the same "never update a buffer an already-recorded dispatch reads" discipline; the
  spatial pmndrs effect solved this with a 64-slot ring (`pmndrs_upscaler.h:74-111`).

### 1.6 Post-upscale presentation

The upscaled texture is presented by `RendererSceneRenderRD::_render_buffers_post_process_and_tonemap()`
(`renderer_scene_render_rd.cpp:466`, called from `render_forward_clustered.cpp:2661`):

```cpp
RSE::ViewportScaling3DMode scale_mode = rb->get_scaling_3d_mode();                  // 483
bool use_upscaled_texture = rb->has_upscaled_texture() && (scale_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR2 || scale_mode == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL); // 484  <-- mode 7 goes here
...
RID color_texture = use_upscaled_texture ? rb->get_upscaled_texture() : rb->get_internal_texture(); // 514
Size2i color_size = use_upscaled_texture ? target_size : rb->get_internal_size();                  // 515
```

Everything from there on works at `color_size`/target res: DoF (`542,564`), glow (glow
still sources the internal texture, 638 — same as FSR2), and finally tonemapping reads
`color_texture` (`renderer_scene_render_rd.cpp:838`). Other places already keyed off
"is this a temporal scaler":

- blur/backbuffer sizing: `renderer_scene_render_rd.cpp:336,369`
  (`reuse_blur_texture = !rb->has_upscaled_texture() || sizes match`)
- SMAA/FXAA: `effects/smaa.cpp:149-151,171-173` (`scaling_type == TEMPORAL` + upscaled
  texture ⇒ AA at target size)
- mip bias: `render_scene_buffers_rd.cpp:109` (TAA or temporal scaling → −0.5 bias)
- DoF at target size: `render_scene_buffers_rd.cpp:521` (`allocation_blur_textures`)

The upscaled texture is created on demand by
`RenderSceneBuffersRD::ensure_upscaled()` (`render_scene_buffers_rd.cpp:721-728`), called
from `render_forward_clustered.cpp:2459-2464` when `using_upscaling`:

```cpp
const uint32_t usage_bits = SAMPLING | (can_be_storage ? STORAGE : 0) | COLOR_ATTACHMENT | INPUT_ATTACHMENT;
create_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_UPSCALED, get_base_data_format(), usage_bits, TEXTURE_SAMPLES_1, target_size);
```

`get_base_data_format()` is `RGBA16F` for Forward+
(`renderer_scene_render_rd.cpp:1209-1211`, `render_scene_buffers_rd.h:198`), i.e. the
output stays linear HDR and the tonemapper handles display encoding. Accessors:
`has_upscaled_texture()`/`get_upscaled_texture()/get_upscaled_texture(layer)`
(`render_scene_buffers_rd.h:312-320`).

### 1.7 MSAA / TAA interplay

- **MSAA**: before the upscale block, `render_forward_clustered.cpp:2530-2558` resolves the
  MSAA attachment only if it exists: colour → `get_internal_texture(v)` (2540-2542),
  depth → `get_depth_texture(v)` (2543-2548, only when
  `using_taa || using_upscaling || using_dof || compositor needs resolved depth` at 2536),
  velocity → `get_velocity_buffer(false,v)` (2549-2554). So a temporal upscaler that
  supports MSAA receives resolved inputs; but note the resolved depth is **R32_SFLOAT**
  (colour-format) while the non-MSAA depth is a **depth-stencil** format
  (`render_scene_buffers_rd.cpp:811-826`) — two different binding stories (§3, §5.2).
  pmndrs core explicitly **requires aliased, single-sample input and warns once if it gets
  an MSAA texture** [port contract; `docs/inputs-and-contracts.md` upstream: "Turn MSAA
  off"]. Recommended v1: force MSAA off for mode 7 in the viewport layer exactly like
  MetalFX temporal does (§4.3).
- **TAA conflict**: the viewport layer disables TAA when the scaling mode is classified
  `VIEWPORT_SCALING_3D_TYPE_TEMPORAL` (`renderer_viewport.cpp:252-257`), and the
  `RenderSceneBuffersConfiguration` carries the resulting `use_taa` to
  `rb->get_use_taa()` (`render_forward_clustered.cpp:1860`). Mode 7 gets this for free
  **iff** `scaling_3d_mode_type()` is taught it is temporal (§4.1). A temporal upscaler and
  TAA must never both run: they would both own the jitter and both jitter the projection.
- **`scale == 1.0`**: the "no scaling needed" shortcut only fires for spatial modes:
  `renderer_viewport.cpp:147-153` (`!upscaler_available || SPATIAL`), so temporal modes
  keep running at 1.0 (intentional: temporal upscaling at 1.0 is still a
  jitter+accumulate Anti-Aliasing pass). Mode 7 inherits this. Downsampling
  (`scale > 1.0 + EPSILON`) falls back to bilinear for all non-bilinear modes
  (`renderer_viewport.cpp:236-242`).

### 1.8 Why FSR2 is not usable on WebGPU (and why the new effect is)

Three separate blockers, all already mitigated by fallbacks in this tree:

1. B10G11R11 storage images are not a WebGPU storage texel format → effect disabled at
   construction (`fsr2.cpp:523-527`) and at the viewport layer
   (`renderer_viewport.cpp:175-190` falls back to FSR1 or OFF).
2. No 32-bit image atomics on WebGPU (`drivers/webgpu/...:10573-10574` returns false for
   `SUPPORTS_IMAGE_ATOMIC_32_BIT`) — FSR2 already ships `FFX_FSR2_NO_IMAGE_ATOMICS` /
   `SPD_NO_IMAGE_ATOMICS` defines (`fsr2.cpp:552-555`).
3. No reliable half float / subgroups (`SUPPORTS_HALF_FLOAT` false,
   `rendering_device_driver_webgpu.cpp:10652-10654`); FSR2 disables fp16 variants.

The pmndrs temporal path is F32 WGSL, uses no image atomics, no subgroups, and uses only
base-WebGPU storage formats (rgba16float, r32float, rgba8unorm, rgba32uint), so none of
these blockers apply — only the new driver gaps in §5.

---

## 2. Jitter: current chain, conventions, and how the new effect owns it

### 2.1 Current production chain (Halton, one source)

1. **Phase count** — `RendererViewport::_configure_3d_render_buffers()`
   (`renderer_viewport.cpp:302-313`): if the mode is temporal,
   `jitter_phase_count = uint32_t(8.0f * pow(target_width / render_width, 2.0f))` (copied
   from `ffxFsr2GetJitterPhaseCount`); else if TAA, `16`; else `0`. Stored in
   `Viewport::jitter_phase_count` (`renderer_viewport.h:64`) and passed to
   `RendererViewport::_draw_3d()` → `RSG::scene->render_camera(...)`
   (`renderer_viewport.cpp:358`).
2. **Jitter value** — `RendererSceneCull::render_camera()`
   (`renderer_scene_cull.cpp:2709-2730`): a Halton(2,3) sequence in [−1,1]
   (`get_halton_value()`, `renderer_scene_cull.cpp:54-64`) is divided by the *render* size
   and picked by frame number:
   `jitter = camera_jitter_array[frame_number % phase_count] / viewport_size` where
   `p_viewport_size` is the internal (render) size (`renderer_viewport.cpp:358`).
   `taa_frame_count = frame_number % phase_count`. Both are stored in
   `RendererSceneRender::CameraData` (`renderer_scene_render.h:318-319`;
   `set_camera()` at `renderer_scene_render.cpp:38-56`, called from
   `renderer_scene_cull.cpp:2795`).
3. **Previous frame** — the viewport caches the previous `CameraData`:
   `viewport->prev_camera_data` (`renderer_viewport.h:77-78`), `_render_scene()` reads it
   (`renderer_scene_cull.cpp:3727-3731`) and stores the current one after rendering
   (`renderer_scene_cull.cpp:3737`).
4. **Into the renderer** — `RendererSceneRenderRD::render_scene()`
   (`renderer_scene_render_rd.cpp:1411`) builds `RenderSceneDataRD scene_data`:
   `scene_data.taa_jitter = p_camera_data->taa_jitter` (1431),
   `scene_data.taa_frame_count = p_camera_data->taa_frame_count` (1432),
   `scene_data.prev_taa_jitter = p_prev_camera_data->taa_jitter` (1444).
5. **Applied to the camera projection** — `RenderSceneDataRD::update_ubo()`
   (`render_scene_data_rd.cpp:74`): `correction.set_depth_correction(flip_y)` then
   `correction.add_jitter_offset(taa_jitter)` (85-86) and `projection = correction * cam_projection`;
   stored into the UBO (88-89). The raw jitter is also written to
   `ubo.taa_jitter[0..1]` (114-115). The previous-frame camera is rebuilt the same way from
   `prev_cam_projection/prev_cam_transform` and **`prev_taa_jitter`**
   (`render_scene_data_rd.cpp:275-305`; `prev_correction.add_jitter_offset(prev_taa_jitter)`
   at 282, `prev_ubo.taa_jitter` at 302-303).
   `Projection::add_jitter_offset()` adds the offset to the 4th (w) column of the
   correction matrix (`core/math/projection.h:134`, `projection.cpp:1044-1047`), and
   because that matrix is multiplied on the **left** of the camera projection
   (`correction * cam_projection`) the offset is a true post-divide NDC offset.
6. Other consumers of `taa_jitter`/`taa_frame_count` that keep working unchanged:
   `RenderSceneDataRD::get_cam_projection()`/`get_view_projection()`
   (`render_scene_data_rd.cpp:42-52,56-66`, used by debug draws and compositor
   effects), sky jitter (`servers/rendering/renderer_rd/environment/sky.cpp:1227-1228`), contact shadows
   (`render_forward_clustered.cpp:1714` → `ss_effects.cpp:1857`), occlusion culling
   (unjittered `camera_data.main_projection` is used, `renderer_scene_cull.cpp:2808`).

### 2.2 Motion vectors are already jitter-free

`scene_forward_clustered.glsl:3215-3222`:

```glsl
#ifdef MOTION_VECTORS
	vec2 position_clip = (screen_position.xy / screen_position.w) - scene_data.taa_jitter;
	vec2 prev_position_clip = (prev_screen_position.xy / prev_screen_position.w) - scene_data_block.prev_data.taa_jitter;

	vec2 position_uv = position_clip * vec2(0.5, 0.5);
	vec2 prev_position_uv = prev_position_clip * vec2(0.5, 0.5);

	motion_vector = prev_position_uv - position_uv;
#endif
```

`screen_position` is `gl_Position` from the vertex shader, which uses the UBO projection
that already contains the jitter (`scene_forward_clustered.glsl:525`,
`gl_Position = projection_matrix * vec4(vertex_interp, 1.0)`); `prev_screen_position` uses
`prev_data` (the previous-frame UBO, i.e. previous jitter). Subtracting the current and
previous jitter makes both endpoints unjittered, so a static pixel reports exactly zero.
**Therefore motion vectors need no change when a new upscaler owns the jitter — only the
`taa_jitter`/`prev_taa_jitter` values must be the ones actually applied.**

### 2.3 Conventions (the part that silently breaks)

Three different spaces meet here. Get them wrong and you get doubled edges (jitter sign)
or history smear (velocity sign), not a validation error.

| quantity | production convention | evidence |
| --- | --- | --- |
| `taa_jitter` (CameraData/SceneData) | **clip-space offset** (added to projection w-column → post-divide NDC offset), range ≈ ±1/(2·render_size) per axis for Halton | `render_scene_data_rd.cpp:85-86,114-115`; `projection.cpp:1044-1047` |
| Godot engine-space clip/NDC y | **image-down** (Vulkan convention): the projection gets `set_depth_correction(flip_y=true)`, and the fullscreen UV mapping in `motion_vectors.glsl` is `uv = ((x+1)/2, (y+1)/2)` from `gl_Position`, so engine-space NDC y=−1 is the top row | `render_scene_data_rd.cpp:85-86`; `effects/motion_vectors.glsl:12`; `motion_vectors.glsl:65` |
| final WebGPU clip y | **image-up**: the WebGPU shader container negates the vertex Position builtin's Y before every return (`negate_position_y`, applied to every Tint-converted vertex shader via `prepare_for_tint`) | `spirv_preprocess.cpp:1503-1560,3346`; `spirv_preprocess.h:88,214`; callers `rendering_shader_container_webgpu.cpp:257`, `spirv_to_wgsl.cpp:79` |
| velocity buffer | RG16F, **render res**, **UV space**, **y-down**, value = **previous − current** (see next row) | `render_scene_buffers_rd.cpp:866` (`DATA_FORMAT_R16G16_SFLOAT`); `scene_forward_clustered.glsl:3223` (`motion_vector = prev - cur`) |
| how consumers use velocity | `uv_reprojected = uv + velocity` (TAA) → matches prev−cur UV | `taa_resolve.glsl:316-319`; `motion_vectors.glsl:65` |
| invalid pixels | cleared to `(-1,-1,0,0)`; FSR2 derives them from depth+reprojection | `render_forward_clustered.cpp:2333-2337`, `fsr2.cpp:539-543` |

**pmndrs contract** (normative sources: upstream `docs/engine-contract.md` §Jitter/§Motion,
the three adapter's `applyJitterViewOffset()` — `packages/three/src/math/viewOffset.ts` —
and `packages/core/src/UpscalerCore.ts:758-767,900-945`; the `jitter` getter doc at
`packages/core/src/UpscalerCore.ts:766-773` describes the *sample* offset, which has the opposite sign of the
projection offset below — do not use it for the projection):

- `beginFrame()` **advances and returns the current jitter in render pixels**, range
  `[-0.5, 0.5]` per axis, **x right, y down**. The same frame's previous jitter is
  available as `jitterPrevious`; `jitterPhase` is `0..jitterPhaseCount-1`.
- The jitter is a *sample* offset: "the sample for render texel `(i, j)` moves to
  `(i + 0.5 + x, j + 0.5 + y)`". The required **projection** translation is therefore the
  negation, applied to the *final* (WebGPU, y-up) clip position:

  ```wgsl
  clip.x += (-2.0 * jx / W) * clip.w;
  clip.y += ( 2.0 * jy / H) * clip.w;   // final clip y is up
  ```
  (identical to three's `camera.setViewOffset(W, H, jx, jy, W, H)`).
- Velocity convention is **NDC delta, current − previous, NDC y up**; `motionScale`
  (default `[0.5,-0.5]`) converts it to a UV delta so that `prevUV = uv − velocity·motionScale`.
  "A UV-space, y-down engine passes `[1,1]`."

**Derived Godot mappings** (this is the exact change to make; validate per §2.7):

- **Jitter → projection.** Work the chain end-to-end (this is the only place the signs can
  go wrong, and it is worth the four lines):
  1. pmndrs requires the **final WebGPU clip** translation `(-2*jx/W, +2*jy/H)`
     (final clip y is up).
  2. Godot stores `taa_jitter` in the *engine-space* projection, which is
     `correction * cam_projection` with `correction.set_depth_correction(flip_y)`:
     engine-space clip y is the **negation** of the camera's y
     (`render_scene_data_rd.cpp:85-86`, `core/math/projection.cpp:899-919`).
  3. The WebGPU shader container then negates the vertex Position builtin's Y again before
     rasterization (`negate_position_y`, `drivers/webgpu/spirv_preprocess.cpp:1503-1560`
     called from `prepare_for_tint` at `:3346`; both the runtime container
     `rendering_shader_container_webgpu.cpp:257` and the offline baker
     `spirv_to_wgsl.cpp:79` use it).
  4. `add_jitter_offset` is applied to the correction matrix *after* the depth correction,
     so in engine space the offset appears with a **+** sign on both axes; the driver's
     output negation flips only Y. Hence a `taa_jitter` `t` produces the final clip
     translation `(+t.x, -t.y)`.
  5. Set `(+t.x, -t.y) = (-2*jx/W, +2*jy/H)` → **`t = (-2*jx/W, -2*jy/H)`**.

  So with `j = begin_frame()`:

  ```cpp
  const Size2i internal = rb->get_internal_size();
  // pmndrs jitter = render-pixel sample offset, x right / y down.
  scene_data.taa_jitter      = Vector2(-2.0f * j.x / internal.x, -2.0f * j.y / internal.y);
  scene_data.prev_taa_jitter = fj.reset ? Vector2()
          : Vector2(-2.0f * jp.x / internal.x, -2.0f * jp.y / internal.y);
  scene_data.taa_frame_count = float(j.phase);
  ```

  Equivalently the **pixel** jitter handed to the effect's `Parameters` is just `(j.x, j.y)`
  — the same way FSR2 receives `taa_jitter * internal_size * 0.5` at
  `render_forward_clustered.cpp:2588`.
  **Note:** the pmndrs-port doc (`docs/PMNDRS_TEMPORAL_PORT.md` §5.6) proposes
  `t = (-2*jx/W, +2*jy/H)`; its X agrees, its Y does not account for step 4. The numeric
  test in §2.7 settles it in minutes — treat the Y sign as a single named constant in code
  until then, and do not "fix" it from visual impression alone.
- **Velocity → effect.** Godot's buffer is UV y-down prev−cur; pmndrs wants NDC y-up
  cur−prev or a configured `motionScale`. With Godot's value substituted directly into
  `prevUV = uv − velocity·motionScale`, the scale that reproduces
  `prevUV = curUV + godot_velocity` is **`motionScale = [-1, -1]`**. (Derivation:
  NDC y-up delta `Δndc = (ndc_cur − ndc_prev)`; UV y-down delta `Δuv = (0.5Δndc_x, −0.5Δndc_y)`;
  Godot velocity `v = prevUV − curUV = −Δuv`; `uv − v·s = uv + Δuv` requires `s = -1` on
  both axes.) The port adapter must therefore set the core's `motionScale` to `[-1,-1]`
  rather than relying on the default `[0.5,-0.5]`.
- **Depth.** Godot Forward+ is **reversed-Z in [0,1]** (near=1, far=0, cleared to 0):
  `Projection::set_depth_correction` defaults are `(flip_y=true, reverse_z=true, remap_z=true)`
  (`core/math/projection.h:76`, `projection.cpp:899-919`) and `RenderSceneDataRD` calls it
  as `set_depth_correction(flip_y)` (`render_scene_data_rd.cpp:44,64,85`), so the UBO
  projection maps near→1, far→0; the depth attachment is cleared to 0.0f
  (`render_forward_clustered.cpp:2323`), and FSR2 is configured with
  `FFX_FSR2_ENABLE_DEPTH_INVERTED` ("input depth is inverted [1..0]", `fsr2.cpp:877`,
  `thirdparty/amd-fsr2/ffx_fsr2.h:91`). Pass `reversedDepth = true`, `perspective =
  !scene_data.cam_orthogonal`, positive finite `near/far` from `scene_data.z_near/z_far`
  (pmndrs linearizes with `near`/`far`; they must match the projection that rendered
  depth). `docs/PMNDRS_TEMPORAL_PORT.md` §5.6 currently says the opposite (standard depth,
  `reversedDepth=false`); the evidence above says reversed-Z — this is the second
  contract point to settle with the port owner before dispatch (a mismatch throws in the
  core's linearization, so it will not pass silently).

### 2.4 Previous-frame jitter availability

- **FSR2/TAA today**: `scene_data.prev_taa_jitter` from the viewport's cached
  `CameraData` (chain in §2.1 items 3-5). This is why the jitter sequence must be strictly
  frame-indexed: any hold/skip breaks the prev/current pairing.
- **New effect**: the effect itself owns the sequence (`beginFrame()` advances it and
  exposes `jitterPrevious`), so the renderer must **not** rely on the viewport prev chain.
  `PmndrsTemporal::ViewState` stores `jitter`/`jitter_prev`; `begin_frame()` returns both.
  The renderer writes both into `scene_data` (above). If a frame begins but the upscale is
  skipped, the effect's own `_frameUndispatched` rollback handles the sequence (upstream
  `UpscalerCore.beginFrame()` retreats before advancing), so the renderer must call
  `begin_frame()` **only on frames where `upscale()` will actually run** (guard on
  reflection probes / unsupported / non-mode-7 — see §6.4).

### 2.5 TAA jitter code as reference

- `TAA::process()` (`effects/taa.cpp:90-129`) loops views, resolves at internal size,
  creates a `taa/prev_velocity` RGBA16F history (`taa.cpp:103-110`; note the WebGPU
  RG16F→RGBA16F comment), then calls `resolve()` and finally copies the current velocity
  into the previous-velocity texture (`taa.cpp:129`).
- `TAA::resolve()` (`taa.cpp:51-88`) shows the pattern of a per-view effect reading
  colour/depth/velocity and writing history through `RD::Uniform` sets created with a
  `sampler + texture` (`taa.cpp:78-84`) — a good template for `PmndrsTemporal`'s
  uniform-set creation.
- Crucially, **TAA never applies jitter itself**. The viewport/SceneData path does (TAA
  phase count = 16, `renderer_viewport.cpp:307-310`), and `taa_resolve.glsl` only
  reprojects with the jitter-free velocity (`taa_resolve.glsl:316-319`). A TAA reference
  therefore tells you nothing about who should own the jitter — the temporal upscaler must
  own it, which is exactly the FSR2 behaviour: FSR2's Halton phase count is chosen at the
  viewport layer but its constants receive the same `taa_jitter`.

### 2.6 The exact change: let `PmndrsTemporal` own the jitter

Chosen seam: **override `scene_data` inside `RendererSceneRenderRD::render_scene()`**, immediately
after the camera data block and before `_render_scene()` is called
(`renderer_scene_render_rd.cpp:1559`). This is strictly better than a public virtual on
`RendererSceneRender` called from `RendererSceneCull::render_camera()` because
current+previous jitter stay together, every consumer (motion pass, sky, contact shadows,
debug draws, effect params) sees one coherent value, and no public interface changes.

```cpp
// renderer_scene_render_rd.cpp, insert after scene_data.prev_taa_jitter assignment (~line 1445)
if (!p_reflection_probe.is_valid() && pmndrs_temporal != nullptr && pmndrs_temporal->is_supported() &&
		rb->get_scaling_3d_mode() == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL &&
		rb->get_view_count() == 1) {
	const RendererRD::PmndrsTemporal::FrameJitter fj = pmndrs_temporal->begin_frame(rb);
	const Size2i internal = rb->get_internal_size();
	// pmndrs jitter is a render-pixel SAMPLE offset (x right, y down); the projection
	// offset is its negation in the final WebGPU clip space. Godot's engine-space clip y
	// is flipped (depth correction) and the WebGPU driver negates output y again, so the
	// net mapping is both-negative. Derivation in §2.3; validate with §2.7 test 1.
	scene_data.taa_jitter = Vector2(-2.0f * fj.jitter.x / internal.x, -2.0f * fj.jitter.y / internal.y);
	scene_data.prev_taa_jitter = fj.reset ? Vector2()
			: Vector2(-2.0f * fj.jitter_prev.x / internal.x, -2.0f * fj.jitter_prev.y / internal.y);
	scene_data.taa_frame_count = float(fj.phase);
}
```

Consequences to double-check in review:

- The Halton jitter computed in `RendererSceneCull::render_camera()` is still stored in the
  `CameraData` and cached as prev camera data; it is **overwritten** for the scene pass.
  Nothing else reads `CameraData::taa_jitter` between `render_camera()` and `render_scene()`
  except the occlusion buffer update, which uses the unjittered projection
  (`renderer_scene_cull.cpp:2808`). This is acceptable but should be documented in the
  code comment.
- `jitter_phase_count` computed at `renderer_viewport.cpp:302-313` remains FSR2-style; the
  effect uses its own phase count internally, and the value is only used to gate the
  Halton computation. If you prefer, skip the Halton computation entirely when mode 7 is
  active (it is wasted work), but then make sure `prev_camera_data` still exists.
- `taa_frame_count` is replaced by the effect's `jitterPhase` so contact shadows
  (`render_forward_clustered.cpp:1714`) keep cycling.
- No shader edits: motion vectors already subtract `scene_data.taa_jitter` and
  `prev_data.taa_jitter` (§2.2), and the projection matrices are built from `taa_jitter`
  in `update_ubo()`.
- `FFX`-style `reprojection` matrix for the effect parameters is built from **unjittered**
  `scene_data.cam_projection/prev_cam_projection` and
  `cam_transform/prev_cam_transform` exactly like the FSR2 call
  (`render_forward_clustered.cpp:2606-2613`).

### 2.7 How to validate the jitter plan

1. **Sign/axis test (numeric, decisive).** Bypass `beginFrame()` and inject a fixed
   jitter `j`, e.g. `(0.25, 0)` then `(0, 0.25)`. Put a single sub-pixel-sized bright
   feature (a tiny unshaded quad / a 1px emitter) in a static scene and read back the
   internal colour texture (`RD::texture_get_data`, editor/native only; or a debug blit to
   screen). The feature's centre of mass must move by **−(j.x·W, j.y·H)** pixels in the
   rendered (texture y-down) image versus the zero-jitter frame: for `j.x = 0.25` it moves
   left by 0.25 px; for `j.y = 0.25` it moves **up** by 0.25 px (content shift = −sample
   offset). If the measured shift is `+j` the mapping is sign-flipped; if Y is wrong while
   X is right, the `negate_position_y` step is the culprit (see §2.3). This test settles the
   one open sign question in the whole port in a few minutes — do it before any quality
   work.
2. **History convergence test.** Static scene, camera still: the temporal output must
   converge to a stable supersampled image; a wrong jitter sign makes the result *worse
   than bilinear* and produces a characteristic doubled/blurred edge pair. This is the same
   failure the pmndrs docs warn about for `camera.setViewOffset`.
3. **Velocity-test.** Camera pan + static geometry: `VIEWPORT_DEBUG_DRAW_MOTION_VECTORS`
   must show a coherent field; with a wrong velocity sign the guide shows vectors pointing
   backwards and moving objects smear in the opposite direction. Then check a rotating
   camera: sky pixels with velocity `(0,0)` will ghost (see §8) — that is expected until a
   camera-motion fallback exists.
4. **Circle/moving-object test.** A moving object over a static background: with the
   correct conventions, disocclusion leaves a short-lived trail that clears in 1-2 frames;
   a wrong `motionScale` leaves a persistent double-image.

---

## 3. Render-buffer resources available to a temporal upscaler

### 3.1 Textures and buffers at the FSR2 seam

All sizes below come from `RenderSceneBuffersRD::configure()`
(`render_scene_buffers_rd.cpp:155-214`); `can_be_storage` is true on Forward+/WebGPU
(`renderer_scene_render_rd.cpp:1213-1217`), `force_hdr` mirrors the render target
(`render_scene_buffers_rd.cpp:159`), and `get_base_data_format()` is RGBA16F for Forward+
(`render_scene_buffers_rd.h:198`).

| resource | accessor(s) | name/format | usage | size | created at |
| --- | --- | --- | --- | --- | --- |
| internal (pre-tonemap) colour | `get_internal_texture()`, `get_internal_texture(layer)` (`render_scene_buffers_rd.h:257-265`) | `render_buffers/color`, RGBA16F linear HDR | `SAMPLING|COLOR_ATTACHMENT|INPUT_ATTACHMENT` (+`STORAGE` when `can_be_storage`) — `get_color_usage_bits()` `render_scene_buffers_rd.cpp:796-810` | internal size, layers = view count | `render_scene_buffers_rd.cpp:186` |
| internal reactive | `get_internal_texture_reactive(layer)` (`render_scene_buffers_rd.h:266-273`) | **alpha-swizzled view of the colour texture** | same texture | internal | n/a — **does not work on WebGPU** (see 3.2) |
| depth (single-sample) | `get_depth_texture()`, `get_depth_texture(layer)` (`render_scene_buffers_rd.h:281-283`) | `render_buffers/depth`; `get_depth_format(resolve_target,false,can_be_storage)` → **D32_SFLOAT_S8_UINT** preferred, **D24_UNORM_S8_UINT** fallback (`render_scene_buffers_rd.cpp:811-826`); `R32_SFLOAT` when `resolve_target` | `SAMPLING|DEPTH_STENCIL_ATTACHMENT` (+`STORAGE` only on the resolve path) — `get_depth_usage_bits()` `render_scene_buffers_rd.cpp:845-863` | internal | `render_scene_buffers_rd.cpp:192` |
| depth (MSAA) | `get_depth_msaa()/get_depth_msaa(layer)` | `render_buffers/depth_msaa` | DEPTH_STENCIL_ATTACHMENT + CAN_COPY_FROM | internal, samples | `render_scene_buffers_rd.cpp:200` |
| colour (MSAA) | `get_color_msaa()/get_color_msaa(layer)` | `render_buffers/color_msaa` | colour attach | internal, samples | `render_scene_buffers_rd.cpp:199` |
| velocity | `ensure_velocity()`, `get_velocity_buffer(false, layer)` (`render_scene_buffers_rd.h:322-327`) | `render_buffers/velocity`, **RG16F** (`render_scene_buffers_rd.cpp:866`) | colour usage bits (`render_scene_buffers_rd.cpp:870-872`) | internal, layers | `render_scene_buffers_rd.cpp:730-740` |
| velocity (MSAA) | `get_velocity_buffer(true, …)`, `has_velocity_buffer(true)` | `render_buffers/velocity_msaa` | colour usage bits | internal, samples | `render_scene_buffers_rd.cpp:737` |
| upscaled output | `ensure_upscaled()`, `get_upscaled_texture()/…(layer)`, `has_upscaled_texture()` (`render_scene_buffers_rd.h:310-320`) | `render_buffers/color_upscaled`, RGBA16F (`get_base_data_format()`) | `SAMPLING|STORAGE|COLOR_ATTACHMENT|INPUT_ATTACHMENT` | **target** size, layers = view count | `render_scene_buffers_rd.cpp:721-728` |
| exposure / luminance | `luminance->get_current_luminance_buffer(rb)` (`luminance.cpp:150-157`) | custom data `RB_LUMINANCE_BUFFERS`, `R32_SFLOAT` **1×1**, holds current average luminance (`luminance.cpp:86-123`, the published 1×1 created at 116) | `STORAGE|SAMPLING|CAN_COPY_TO` on the final 1×1 | 1×1 | `luminance.cpp:86-123` |
| velocity override (rare) | `render_target_get_override_velocity(...)` used by `get_velocity_buffer(false, …)` (`render_scene_buffers_rd.cpp:758-771`) | arbitrary render-target-supplied texture | — | possibly not internal size | — |

Notes:

- `get_velocity_buffer(false, layer)` can return a **render-target override** texture
  (`render_scene_buffers_rd.cpp:762-771`) — for a screen-texture/motion-vector workflow the
  format/size may not match the internal buffers. Guard: check the texture format/size in
  `Parameters` handling (or require `rb->has_texture(RB_SCOPE_BUFFERS, RB_TEX_VELOCITY)`).
- `RB_TEX_BACK_COLOR/RB_TEX_BACK_DEPTH`, blur textures (`RB_TEX_BLUR_0/1`) and the
  `RB_TEX_COLOR_SUBSAMPLED` family are *not* what a temporal upscaler should read; they are
  screen-texture/DoF plumbing.
- Depth is **not** a float colour texture except on the MSAA-resolve path. A temporal
  effect needs either (a) `texture_depth_2d` hardware sampling (`depthMode: 'hardware'`,
  reversed-Z) with MSAA off, or (b) a dedicated linear/depth-only copy. See §5.2 for the
  driver fix required for (a), and §4.3 for the MSAA policy.

### 3.2 Reactive mask is not usable on WebGPU

`get_internal_texture_reactive()` returns an **alpha-swizzled view** (r=g=b=a) of the
colour texture (`render_scene_buffers_rd.h:266-273`). WebGPU has no texture component
swizzle, and the WebGPU driver's shared-view creation never applies the stored swizzle —
`texture_create_shared()`/`texture_create_shared_from_slice()` do not even read
`p_view.swizzle_*` (`rendering_device_driver_webgpu.cpp:2092-2148,2151-2190`) and the
driver has an explicit comment that swizzles are not implemented
(`rendering_device_driver_webgpu.cpp:1929-1934`). So on WebGPU that view returns plain
RGBA and a shader reading `.r` gets colour red, not reactivity.

Options for the pmndrs temporal effect:

1. **v1: pass no reactive input.** The pmndrs core has reactive as an optional input and a
   `generateReactive` pass that derives reactivity from `opaqueColor` vs `finalColor`; Godot
   does not have opaque-only colour available at this seam. Skip it and rely on depth clip +
   shading change.
2. If reactive is needed later: add a 1-dispatch extract pass (read colour `.a`, write
   `rgba8unorm.r`) or introduce a real reactive render-target component.

### 3.3 Exposure / luminance

- Only present when `RSG::camera_attributes->camera_attributes_uses_auto_exposure(...)`;
  the FSR2 call site already guards on that (`render_forward_clustered.cpp:2576-2579`).
- Format is 1×1 `R32_SFLOAT` (`luminance.cpp:96-116`), content is the current average
  luminance — not an exposure multiplier. The pmndrs `exposureTexture` expects the app's
  exposure ("red texel of a 1×1 texture; overrides the built-in auto-exposure"), and the
  core multiplies the input by it before its invertible tonemap and divides it back out on
  output, so it must not change final brightness [port contract; upstream
  `docs/inputs-and-contracts.md` "Exposure"]. Confirm the semantic mapping with the port
  contract: either pass the luminance buffer directly (if the port transforms it like FSR2
  does) or pass `camera_attributes_get_exposure_normalization_factor()` as the core's fixed
  `settings.exposure` instead. Binding the 1×1 R32F as `texture_2d<f32>` hits the same
  unfilterable-float driver gap as the guide textures (§5.2).
- Alternative if the driver gap is not fixed for v1: allocate a 1×1 **RGBA16F** exposure
  texture in the effect, fill it from the CPU with the camera's
  `exposure_normalization_factor` via `RD::texture_update()` once per frame, and do not use
  auto-exposure at all. This is cheap and avoids both the unfilterable binding and the
  GPU→CPU readback.

### 3.4 Sizes and scale math

`RendererViewport::_configure_3d_render_buffers()` (`renderer_viewport.cpp:136-333`):

- `target_size = p_viewport->size` (the viewport's 3D size), unless scaling is off
  (`renderer_viewport.cpp:265-299`).
- `internal_size = MAX(target_size * scaling_3d_scale, 1.0)` for FSR/FSR2/MetalFX/pmndrs
  modes (`renderer_viewport.cpp:274-283`); CLAMPed to 16384 only for bilinear/nearest
  (271-272). `scaling_3d_scale` is the `Viewport.scaling_3d_scale` property with the
  project default `rendering/scaling_3d/scale` (`rendering_server.cpp:3799`).
- The render-buffer configuration is built at `renderer_viewport.cpp:319-331`:
  `set_internal_size`, `set_target_size`, `set_scaling_3d_mode`, `set_msaa_3d`,
  `set_use_taa`, etc.
- `RenderSceneBuffersRD::get_internal_size()/get_target_size()/get_view_count()`
  (`render_scene_buffers_rd.h:240-243`) are what the effect receives.

### 3.5 What must be allocated persistently for history

Everything that survives a frame must live on `RenderSceneBuffersRD`, created with
`create_texture()` (named textures are freed by `cleanup()` on reconfigure/destroy —
`render_scene_buffers_rd.cpp:127-148,292-351`) or as a `RenderBufferCustomDataRD` whose
`configure()`/`free_data()` are called by `RenderSceneBuffersRD::configure()`
(`render_scene_buffers_rd.cpp:211-213`) and `cleanup()` (`render_scene_buffers_rd.cpp:180-181`).
The `Luminance::LuminanceBuffers` class is the canonical example
(`luminance.h:84-100`, `luminance.cpp:86-133`).

Minimum state a temporal upscaler must persist. **Note the two resolutions**: history,
locks and the output are at **display (target) resolution**, everything else at render
resolution. This is the port contract's inventory (`docs/PMNDRS_TEMPORAL_PORT.md` §6);
formats are dictated by the vendored WGSL and must match it exactly.

| resource | resolution | format | usage | persistence |
| --- | --- | --- | --- | --- |
| output | target | RGBA16F | `SAMPLING|STORAGE` | engine-owned: bind `rb->get_upscaled_texture()` as the core allocator's output (do not let the core allocate it) |
| `history[2]` colour ping-pong | **target** | RGBA16F | `SAMPLING|STORAGE` | persists across frames; written by accumulate, sampled by accumulate/RCAS/debug |
| `locks[2]` | **target** | RGBA16F | `SAMPLING|STORAGE` | persists; same ping-pong index as history |
| `exposure[2]` | 1×1 (or reduced) | RGBA16F | `SAMPLING|STORAGE` | persists; only if auto-exposure/conditioning is enabled |
| `dilatedDepth[2]` | render | R32F | `SAMPLING|STORAGE` | persists; `textureLoad` only (G2 auto-layout → unfilterable-float) |
| `dilatedMotion` | render | RGBA16F | `SAMPLING|STORAGE` | frame-local |
| `masks` (disocclusion) | render | RGBA8 | `SAMPLING|STORAGE` | frame-local |
| `reactiveGenerated` (optional) | render | RGBA8 | `SAMPLING|STORAGE` | frame-local |
| `shadingLuma[2]` | render | R32F | `SAMPLING|STORAGE` | persists; load-only (unfilterable-float) |
| `shadingSignal` | ⌈render/2⌉ | R32F | `SAMPLING|STORAGE` | frame-local; load-only |
| `shadingBlockMemory[2]` | ⌈rw/4⌉ × (⌈rh/4⌉+⌈rh/8⌉) | RGBA32Uint | `SAMPLING|STORAGE|CAN_UPDATE` | persists; must be zeroed by the CPU when stale |
| `scatter[2]` reconstructed-depth buffers | rw·rh·4 B each | storage buffers, `array<atomic<u32>>` | `STORAGE` | persists; initialized to `0x7f800000` (+inf bits) at allocation/reset, not zero |
| reactive/reset dummy textures | 1×1 | R8/RGBA8 | `SAMPLING` | constant; bound when a pass has no real input (e.g. no reactive mask, no cut signal) |
| constants uniforms | 1×1 struct | 96 B payload in 256 B UBO | — | per frame/pass from a ring (§3.5/G8 in §5.2) |
| per-rb effect state | — | `ViewState` custom data | — | jitter/prev, phase, reset flag, frame index, history/depth/scatter parity indices |

Sizing rule of thumb (port contract's numbers, `docs/PMNDRS_TEMPORAL_PORT.md` §6.4):
display-res `history 2×8 B + locks 2×8 B + output 8 B ≈ 40 B/display px`; render-res
guides/scatter `≈ 44 B/render px`. 1080p display + 960×540 render ≈ **106 MB**; 720p +
ratio-2 ≈ 47 MB. That is a genuine VRAM risk on mobile WebGPU (§8) and should be part of
the mode's availability decision, not an afterthought.

---

## 4. Scaling mode 7 (`VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL`): complete edit checklist

The mode must be appended **after** `PMNDRS_SPATIAL` everywhere so that existing integer
values (and saved project settings) are unchanged. Value 7, `*_MAX` becomes 8.

### 4.1 Enum + type classification

`servers/rendering/rendering_server_enums.h:470-483`:

```cpp
enum ViewportScaling3DMode {
	VIEWPORT_SCALING_3D_MODE_BILINEAR,
	VIEWPORT_SCALING_3D_MODE_FSR,
	VIEWPORT_SCALING_3D_MODE_FSR2,
	VIEWPORT_SCALING_3D_MODE_METALFX_SPATIAL,
	VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL,
	VIEWPORT_SCALING_3D_MODE_NEAREST,
	VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL,
	// Fast Godot: pmndrs temporal upscaler (FSR2-style) on the raw-WGSL compute
	// path. WebGPU only; elsewhere the viewport layer falls back.
	VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL,
	VIEWPORT_SCALING_3D_MODE_MAX,
	VIEWPORT_SCALING_3D_MODE_OFF = 255, // for internal use only
};
```

`servers/rendering/rendering_server_enums.h:492-499` — **required**, this is what drives
TAA conflict, mip bias, jitter phase count, blur sizing, SMAA, and the `scale==1.0`
shortcut:

```cpp
} else if (p_mode == VIEWPORT_SCALING_3D_MODE_FSR2 || p_mode == VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL ||
		p_mode == VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL) {
	return VIEWPORT_SCALING_3D_TYPE_TEMPORAL;
}
```

`scene/main/viewport.h:104-111`:

```cpp
		SCALING_3D_MODE_PMNDRS_SPATIAL,
		SCALING_3D_MODE_PMNDRS_TEMPORAL,
		SCALING_3D_MODE_MAX
```

`scene/main/viewport.cpp:5489-5490` (BIND_ENUM_CONSTANT before MAX) and
`servers/rendering/rendering_server.cpp:2938-2939` (same for RSE):
add `BIND_ENUM_CONSTANT(SCALING_3D_MODE_PMNDRS_TEMPORAL)` /
`BIND_ENUM_CONSTANT(RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL)`.

### 4.2 Property / project-setting hints

- `scene/main/viewport.cpp:5423` `ADD_PROPERTY(... "scaling_3d_mode", PROPERTY_HINT_ENUM,
  "Nearest (Fastest):5,Bilinear (Fastest):0,FSR 1.0 (Fast):1,FSR 2.2 (Slow):2,MetalFX
  (Spatial - Fast):3,MetalFX (Temporal - Slow):4,pmndrs (Spatial - Fast):6,pmndrs
  (Temporal - Slow):7" ...)`.
- `servers/rendering/storage/render_scene_buffers.cpp:55` — same string (this is
  `RenderSceneBuffersConfiguration`'s inspector hint).
- `servers/rendering/rendering_server.cpp:3786-3794` project setting hints. Current code
  keeps `mode_hints_arr` in ascending-value order and inserts MetalFX before the last
  element. With two pmndrs entries, insert explicitly for the Metal arrays:

```cpp
Vector<String> mode_hints_arr = { "Nearest (Fastest):5", "Bilinear (Fastest):0", "FSR 1.0 (Fast):1", "FSR 2.2 (Slow):2", "pmndrs (Spatial - Fast):6", "pmndrs (Temporal - Slow):7" };
mode_hints = String(",").join(mode_hints_arr);

mode_hints_arr.insert(4, "MetalFX (Spatial - Fast):3");   // before pmndrs entries
mode_hints_arr.insert(5, "MetalFX (Temporal - Slow):4");
mode_hints_metal = String(",").join(mode_hints_arr);
```

(Order in a hint string is cosmetic; ascending numeric order matches the existing style.)
No new `GLOBAL_DEF` is needed for the mode itself — `rendering/scaling_3d/mode` already
stores the raw enum int (`rendering_server.cpp:3795-3797`).

### 4.3 `RendererViewport::_configure_3d_render_buffers()` guards/fallbacks

`servers/rendering/renderer_viewport.cpp:136-333`. Edits:

1. **Mobile renderer rejection** — extend the list at `renderer_viewport.cpp:162`:

```cpp
if ((scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR || scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR2 ||
		scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL ||
		scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL) && OS::get_singleton()->get_current_rendering_method() == "mobile") { ... bilinear ... }
```

2. **Support probe + fallback chain** — after the FSR2 B10G11R11 fallback block
   (`renderer_viewport.cpp:175-190`) or next to the MetalFX temporal probes
   (193-203), add (RD_ENABLED only):

```cpp
// pmndrs temporal needs the raw-WGSL compute path and RGBA16F sampled+storage.
// Provisional gate; tighten to whatever the §5.2 driver decision is.
if (scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL &&
		(!RD::get_singleton()->has_feature(RD::SUPPORTS_RAW_WGSL_COMPUTE) ||
				!RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
						RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT))) {
	if (RD::get_singleton()->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, RD::TEXTURE_USAGE_STORAGE_BIT)) {
		scaling_3d_mode = RSE::VIEWPORT_SCALING_3D_MODE_FSR2;
		WARN_PRINT_ONCE("pmndrs temporal upscaling is not supported by this rendering device. Falling back to FSR 2.");
	} else {
		scaling_3d_mode = RSE::VIEWPORT_SCALING_3D_MODE_OFF;
		scaling_3d_scale = 1.0;
		WARN_PRINT_ONCE("pmndrs temporal upscaling is not supported by this rendering device. Disabling 3D scaling.");
	}
	scaling_type = RSE::scaling_3d_mode_type(scaling_3d_mode);
}
```

3. **MSAA policy** (v1: temporal needs single-sample) — mirror the MetalFX temporal rule
   at `renderer_viewport.cpp:212-223`:

```cpp
if (scaling_3d_mode == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL && msaa_3d != RSE::VIEWPORT_MSAA_DISABLED) {
	WARN_PRINT_ONCE("pmndrs temporal upscaling does not support 3D MSAA. Disabling 3D MSAA internally.");
	msaa_3d = RSE::VIEWPORT_MSAA_DISABLED;
}
```

(If you instead keep MSAA, you must add a "hardware depth as R32F colour texture" shader
variant and rely on §5.2 for the unfilterable binding — not recommended for v1.)

4. **Size switch** — add the mode to the upscaler group at
   `renderer_viewport.cpp:274-279`:

```cpp
case RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_SPATIAL:
case RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL:
case RSE::VIEWPORT_SCALING_3D_MODE_FSR:
case RSE::VIEWPORT_SCALING_3D_MODE_FSR2:
```

5. **No edit needed (verify only)**:
   - TAA conflict `renderer_viewport.cpp:252-257` (automatic once 4.1 type helper is fixed).
   - `scale == 1.0` shortcut `renderer_viewport.cpp:147-153` (automatic: temporal keeps
     running at 1.0).
   - downsampling `renderer_viewport.cpp:236-242` (automatic: falls back to bilinear).
   - `upscaler_available` guard `renderer_viewport.cpp:244-250` — requires
     `p_viewport->fsr_enabled`, which is `!is_low_end() && !disable_3d`
     (`renderer_viewport.cpp:1049`). Forward+/WebGPU is not low-end, so this is true; if
     it were false mode 7 would silently fall back to bilinear. Leave as-is.
   - jitter phase count `renderer_viewport.cpp:302-313` (automatic; value unused by the
     effect but keeps Halton/prev data alive — see §2.6).
   - `_viewport_requires_motion_vectors()` `renderer_viewport.cpp:1175-1179` (automatic via
     temporal classification).

### 4.4 `RendererViewport::viewport_set_scaling_3d_mode()`

`renderer_viewport.cpp:1072-1112`. Add next to the FSR2/PMNDRS_SPATIAL Forward+-only
guards (1081-1089):

```cpp
if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL) {
	WARN_PRINT_ONCE_ED("pmndrs temporal 3D scaling is only available when using the Forward+ renderer.");
	return;
}
```

(inside the `rendering_method != "forward_plus"` block).

### 4.5 `RenderForwardClustered::_render_scene()`

- `scale_type` enum `render_forward_clustered.cpp:1862-1866`: add `SCALE_PMNDRS_TEMPORAL`.
- switch `render_forward_clustered.cpp:1868-1888`: add

```cpp
case RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL:
	// pmndrs temporal needs the raw-WGSL compute path; the viewport layer
	// normally falls back already, this is the last line of defence.
	if (pmndrs_temporal != nullptr && pmndrs_temporal->is_supported()) {
		scale_type = SCALE_PMNDRS_TEMPORAL;
	} else {
		WARN_PRINT_ONCE("pmndrs temporal upscaling is not supported by the current rendering device. Falling back to no 3D upscaling.");
	}
	break;
```

  (`pmndrs_temporal` is the protected member from `RendererSceneRenderRD`, §6; do not
  forget `rb->get_view_count() == 1` — if >1, warn + `SCALE_NONE`.)
- **velocity clear sentinel** `render_forward_clustered.cpp:2333-2337`: the
  `else` branch clears to `(-1,-1,0,0)` for FSR2. pmndrs has no derive-invalid-motion
  option, so for mode 7 clear with `(0,0,0,0)` (or add the sentinel handling in the
  effect's adapter [port contract]). Concretely:

```cpp
Vector<Color> motion_vector_clear_colors;
motion_vector_clear_colors.push_back(scale_type == SCALE_PMNDRS_TEMPORAL ? Color(0, 0, 0, 0) : Color(-1, -1, 0, 0));
```

- **upscale call** in the `if (rb_data.is_valid() && (using_upscaling || using_taa))` block
  (`render_forward_clustered.cpp:2572`): add a branch next to `SCALE_FSR2`, templated on
  §1.4 and §6.2. It must fill the same information FSR2 fills (colour/depth/velocity/
  exposure/output, sizes, near/far, fova, jitter from `scene_data.taa_jitter`,
  delta_time, reprojection) and call `pmndrs_temporal->upscale(params)` per view.
- **lifecycle**: if the effect is owned by `RendererSceneRenderRD` (§6.1), nothing to
  add here beyond access; if you prefer FSR2-style ownership by `RenderForwardClustered`
  instead, mirror `render_forward_clustered.cpp:5445`/`5464-5467` and expose a
  `begin_frame()` call from `RendererSceneRenderRD::render_scene()` through a getter.

### 4.6 `RendererSceneRenderRD` selection / presentation

- `renderer_scene_render_rd.cpp:484` — **the** presentation switch:

```cpp
bool use_upscaled_texture = rb->has_upscaled_texture() &&
		(scale_mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR2 ||
				scale_mode == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL ||
				scale_mode == RSE::VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL);
```

- `renderer_scene_render_rd.cpp:486-508` (spatial upscaler selection) — **no edit**: mode
  7 is not a `SpatialUpscaler` and must never be routed through the post-process slot
  (that slot is post-tonemap, single-pass, no history; `effects/spatial_upscaler.h:35-45`).
  This is the key architectural point of the whole port: mode 6 lives in
  `_render_buffers_post_process_and_tonemap`, mode 7 lives in
  `RenderForwardClustered::_render_scene`.
- `renderer_scene_render_rd.cpp:1411-1559` `render_scene()`: jitter override block (§2.6).
- Header `renderer_scene_render_rd.h:62-80`: new protected member (next to
  `pmndrs_spatial`, line 74) and include (next to `pmndrs_upscaler.h`, line 39).
- Constructor/destructor `renderer_scene_render_rd.cpp:1936-1941`/`1959`: create/delete the
  effect under `can_use_storage && has_feature(SUPPORTS_RAW_WGSL_COMPUTE)` (same condition
  as `pmndrs_spatial`).
- No edit needed in `smaa.cpp`, `render_scene_buffers_rd.cpp:109,521`, blur sizing
  (`renderer_scene_render_rd.cpp:336,369`), DoF/glow — all keyed off
  `has_upscaled_texture()`/`scaling_3d_mode_type()`.

### 4.7 Docs / demo exposure

- `doc/classes/Viewport.xml` — add
  `<constant name="SCALING_3D_MODE_PMNDRS_TEMPORAL" value="7" enum="Scaling3DMode">…`
  next to the mode-6 entry (`doc/classes/Viewport.xml:582-583`).
- `doc/classes/RenderingServer.xml` — same for
  `VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL` (next to line 5438-5439).
- **TPS demo**: it is an external test project (not in this repo; the mode-6 write-up in
  `docs/PMNDRS_UPSCALER_SPATIAL.md` "Status" describes it). Mode 7 is exposed through the
  existing `window.scaling_3d_mode = Viewport.SCALING_3D_MODE_PMNDRS_TEMPORAL` /
  project setting `rendering/scaling_3d/mode = 7`; the demo's Settings → "Resolution Scale
  Filter" list needs one more button gated on the WebGPU driver, same pattern as the
  mode-6 button. Unlike mode 6, scale **must be below 1.0** for a meaningful reference but
  scale 1.0 is legal (AA-only); keep the TPS "Native is disabled for upscalers" UI rule
  configurable for mode 7.
- Also add a browser-verification log line in the effect (`[PMNDRS] temporal upscaler
  running …`) mirroring `pmndrs_upscaler.cpp:337-343` so the run is identifiable in the
  console, and update `docs/PATCH_STACK.md`/`docs/WEBGPU_HANDOFF.md` when the feature lands
  (other tasks own those docs).

---

## 5. WebGPU driver: what works today, what the temporal effect still needs

All references to `drivers/webgpu/rendering_device_driver_webgpu.cpp` = `RDW`.

### 5.1 What the raw-WGSL path supports today (milestone 1 scope)

Per `docs/WGSL_COMPUTE_PATH.md` §5/§7/§11 and the code:

| capability | status | evidence |
| --- | --- | --- |
| `shader_create_from_wgsl(WGSLShaderData)` (no SPIR-V/Tint), 1..N contiguous `@group`s | works; only exercised with **1 group** | `rendering_device.cpp:4711-4797`; feature gate `rendering_device_commons.h:1041-1044`, `RDW:10678-10682` |
| uniform buffers (`var<uniform>` in set 0 binding 0) | works (pmndrs spatial constants) | `pmndrs_upscaler.cpp:133-145`; driver BGL `RDW:5366-5376` |
| sampled `texture_2d<f32>` via `textureLoad` | works (EASU/RCAS inputs) | `pmndrs_upscaler.cpp:141-143`; BGL `RDW:5256-5292` |
| write-only `texture_storage_2d<rgba16float, write>` | works | `pmndrs_upscaler.cpp:140-144,166-171`; scan `RDW:4986-5065`; BGL `RDW:5339-5362` |
| compute pipeline + compute lists + `compute_list_add_barrier` | works | `pmndrs_upscaler.cpp:350-367`; `rendering_device.cpp:7532-7551` |
| `compute_list_dispatch_threads` / workgroup sizes from WGSL | works | `rendering_device.h:1089-1100` (WGSLShaderData); `RDW:10153` (`main` entry point) |
| sampler bindings (`var s: sampler` + `textureSampleLevel`) | **code exists, never exercised by raw WGSL** | BGL sampler case `RDW:5242-5254` + `resolve_sampler_type` `RDW:5193-5206`; sampler scan `RDW:4740-4775` |
| storage buffers incl. `var<storage, read_write>` + atomics | **code exists, never exercised by raw WGSL** | RD accepts `UNIFORM_TYPE_STORAGE_BUFFER` (`rendering_device.cpp:4711-4797`, validation at `:5196-5246`); driver SSBO readonly scan `RDW:4935-4980`; BGL `RDW:5377-5410`; WGSL atomics are native Dawn input since raw WGSL bypasses Tint (`RDW:4713-4723`) |
| multiple bind groups | structurally supported (contiguous sets; pipeline layout loop), untested | `rendering_device.cpp:4727-4750`; `RDW:5584-5700` |
| read/read_write storage textures | **not supported in raw WGSL v1** | doc `docs/WGSL_COMPUTE_PATH.md` §7 gotcha 8; driver only splits Tint output `RDW:4405-4700` |
| f16 / subgroups | not available on WebGPU | `RDW:10652-10666` |

### 5.2 Gaps that must be closed/verified for the temporal passes

**(G1) Standalone sampled depth (`texture_depth_2d`).** REQUIRED — **fix landed in the
worktree as "Gap B" (`docs/PMNDRS_TEMPORAL_PORT.md` §12.3): `_depth_only_view()` +
a `Depth` branch in the standalone sampled-texture bind path; verify it and keep the
smoke test.**

- The temporal reconstruct pass reads scene depth as `@group(0) @binding(1) var sceneDepth : texture_depth_2d;`
  (upstream `packages/core/src/shaders/reconstruct.ts:8,152`), with a `texture_2d<f32>`
  variant only for the *linear view-distance* depth mode. Godot's hardware depth is a
  **combined depth-stencil** format (D32_SFLOAT_S8_UINT / D24_UNORM_S8_UINT,
  `render_scene_buffers_rd.cpp:811-826`).
- The driver's BGL for a raw-WGSL sampled texture correctly sets
  `sampleType = Depth` when the WGSL declares `texture_depth_2d`
  (`RDW:5273-5284`, scan at `RDW:4810`). Historically, bind-group creation for a
  standalone `UNIFORM_TYPE_TEXTURE` then fell through to `tex->default_view`
  (`RDW:6013-6015`), and default/shared views are always created with `aspect = All`
  (`RDW:1973-1985,2127,2174`) — WebGPU rejects an `aspect=All` view of a combined
  depth-stencil format in a texture binding. The in-progress fix (Gap B) adds
  `_depth_only_view()` (`RDW:2375`) and a `expected_sample == Depth` branch in the
  standalone sampled-texture bind path (`RDW:5945-5956`), mirroring the combined
  sampler+texture path's existing `DepthOnly` handling (`RDW:6076-6090`). Keep the
  smoke test anyway: depth-stencil formats vary (D32_SFLOAT_S8_UINT only exists with the
  `depth32float-stencil8` optional feature, otherwise D24_UNORM_S8_UINT is chosen —
  `render_scene_buffers_rd.cpp:811-826`), so test both. If the fix is ever reverted, the
  workaround is a `depthMode: 'linear'` path with a linear view-Z colour target (the
  engine does not produce one today) or an engine-side depth-only copy RID.
- Verification: a 1-line raw-WGSL compute shader that does
  `textureLoad(depthTex, coord, 0)` and writes the value to a storage texture; run it
  against a Forward+ viewport depth texture with 3D MSAA off and on. Before the Gap B
  fix, `wgpuDeviceCreateBindGroup` failed with an aspect validation error for both
  format choices; after the fix the same smoke test must pass on D32_SFLOAT_S8_UINT (when
  `depth32float-stencil8` is available) and on the D24 fallback.

**(G2) Unfilterable-float sample types for `r32float` guides.** Was blocking for the
full temporal graph; **fix landed in the worktree as "Gap A"** — verify + keep the smoke
test.

- The temporal shaders declare several storage textures as `r32float` and later sample
  them as `texture_2d<f32>`: `dilatedDepth`, `lumaHistory`, `shadingChange`
  (upstream `packages/core/src/shaders/reconstruct.ts:10,214`, `packages/core/src/shaders/shadingChange.ts:68-69`), and the engine exposure
  buffer is 1×1 R32F (`luminance.cpp:96-116`).
- Before the Gap A fix the driver set BGL `sampleType = Float` for every
  `texture_2d<f32>` (`RDW:5273-5284` is the post-fix decision point). Binding an
  `R32Float` texture to a `Float` binding requires the `float32-filterable` device
  feature; without it, `CreateBindGroup` fails. The driver's
  raster-path band-aids (depth→float copy `RDW:2274-2370`; blank fallbacks
  `RDW:5953-6020`; RGBA8 fallback for unfilterable 32F `RDW:5964-5975`) are **not
  acceptable for compute guides** (silent blank/quantized data).
- **Fix landed in the worktree as "Gap A"** (`docs/PMNDRS_TEMPORAL_PORT.md` §12.3):
  the driver now applies WebGPU's auto-layout rule to raw-WGSL stages — a `f32` sampled
  texture that is statically used with a sampler keeps `Float`, one that is only
  `textureLoad`/`textureDimensions`'d becomes `UnfilterableFloat`
  (`wgsl_tex_sampled_with_sampler` collected in the call-site scan `RDW:4900-4930`,
  consumed at `RDW:5273-5291`; scoped by `raw_wgsl_shader` so the verified Tint/SPIR-V
  BGL entries are unchanged). The effect must therefore declare `r32float` guide bindings
  exactly as the vendored WGSL does (load-only → unfilterable) and must not pass those
  textures to a filtering sampler. This keeps the vendored WGSL verbatim and needs no
  reflection field.
- Fallback if the auto-layout fix is ever unavailable: gate
  `PmndrsTemporal::is_supported()`/the viewport fallback on `float32-filterable`
  (engine.js requests it when the adapter exposes it,
  `platform/web/js/engine/engine.js:351`; `sampler_is_format_supported_for_filter(R32F,
  LINEAR)`, `RDW:3231-3242`, RD API `rendering_device.h:746`). Consequence: mode 7 would
  be unavailable on devices without the feature (some mobile/older browser GPUs).
- Verification: dummy compute pass that writes `R32F` storage then samples it as
  `texture_2d<f32>` with `textureLoad`, with `float32-filterable` requested/not requested.

**(G3) Read/write storage buffers with atomics.** REQUIRED for reconstruct/depth clip
(core `var<storage, read_write> reconstructedDepth : array<atomic<u32>>` and
`atomicMin/Max`, upstream `packages/core/src/shaders/reconstruct.ts:12,77,216-218`).

- This is *structurally* supported but **completely unexercised** by the raw-WGSL path.
  Verification plan (no engine code change expected):
  - reflect as `WGSLUniform{type=UNIFORM_TYPE_STORAGE_BUFFER, binding=N, writable=true, length=0}`
    (`rendering_device.h:1072-1086`; RD normalizes as a storage buffer at
    `rendering_device.cpp:4711-4797`);
  - allocate with `RD::storage_buffer_create(w*h*4)` (`rendering_device.cpp:1549-1560`);
  - bind as `RD::Uniform(UNIFORM_TYPE_STORAGE_BUFFER, N, buffer)`; RD validation is at
    `rendering_device.cpp:5196-5246` (exact-size check only if `length != 0`; keep 0);
  - the WGSL root scan must detect `var<storage, read_write>` and set
    `WGPUBufferBindingType_Storage` (`RDW:4935-4980`, BGL `RDW:5377-5410`);
  - a raw-WGSL smoke-test shader with `atomicAdd`/`atomicMin` on `array<atomic<u32>>` must
    run without Dawn validation errors on Chrome and at least one non-Chromium WebGPU
    implementation.
  Risks to check: the syntax `var<storage, read_write> name : array<atomic<u32>>;` must be
  seen by the 256-char forward scan from `@group(` (it is, same line); Dawn's WGSL atomics
  are core so no Tint involvement; a read-write storage buffer must not be visible to the
  vertex stage (irrelevant, compute-only).

**(G4) `sampler` bindings in raw WGSL.** REQUIRED (accumulate history sampling, exposure
meter, RCAS/blit).

- Code paths exist (`RDW:5193-5206` default `Filtering`, `RDW:4740-4775` declaration scan,
  BGL `RDW:5242-5254`), but milestone 1 used `textureLoad` only and no sampler at all
  (`pmndrs_upscaler.cpp:141-143,166-170`). Verified risk: the sampler is only "Filtering"
  by default; if the temporal shaders pass the sampler to `textureSampleLevel` together
  with an r32float texture, G2 must mark the *texture* unfilterable and the sampler
  NonFiltering. With RGBA16F/RG16F guide and history textures (core-filterable formats),
  the default Filtering sampler is correct.
- Verification: a dummy compute shader declaring `var s : sampler; var t : texture_2d<f32>;`
  and calling `textureSampleLevel(t, s, uv, 0)`; check BGL has two entries at the doubled
  bindings and Dawn accepts the bind group.

**(G5) Formats: what is legal, what to avoid.**

- Needed storage formats: `rgba16float`, `r32float`, `rgba8unorm`, `rgba32uint` — all are
  **base WebGPU storage texel formats**; the driver's WGSL storage-format scan knows all
  of them (`RDW:4986-5065`). r32float write-only needs no feature; read via textureLoad
  needs G2.
- **Do not use r16float/rg16float storage textures**: the driver promotes 16-bit storage
  formats to 32-bit at texture creation (`_promote_storage_format()`,
  `RDW:3051-3111`; applied at `RDW:1894-1902`), which desynchronizes the runtime texture
  format from the WGSL storage declaration and fails bind-group validation. The vendored
  temporal WGSL does not use them (verify after any port adaptation).
- **B10G11R11 is not used** anywhere in the temporal path — the FSR2 blocker
  (`fsr2.cpp:523-527`) does not apply. Keep it that way; `r32float`/`rgba16float` are the
  guides/history formats.
- The engine must allocate history at exactly the formats the WGSL declares, and with
  `TEXTURE_USAGE_STORAGE_BIT|SAMPLING_BIT` (the driver adds `TextureBinding|CopySrc` for
  storage textures, `RDW:1894-1902`, but the RD-level usage must include what the engine
  will sample with; see `RenderSceneBuffersRD::create_texture()` signature,
  `render_scene_buffers_rd.h:219`).

**(G6) MSAA.** No driver gap, but the effect must not receive MSAA inputs; disable 3D
MSAA for mode 7 in the viewport layer (§4.3 item 3). If MSAA support is ever required, the
resolved depth is `R32_SFLOAT` hardware depth (`render_scene_buffers_rd.cpp:811-814`) and
needs a third shader variant (`texture_2d<f32>` + hardware depth, not the linear variant)
plus G2.

**(G7) Dispatch / workgroup limits and WGSL features.**

- All passes are 8×8 workgroups except optional exposure-meter reducers; the WebGPU shell
  maximizes `maxComputeWorkgroupsPerDimension`, `maxComputeWorkgroupStorageSize`,
  `maxStorageBuffersPerShaderStage`, `maxStorageTexturesPerShaderStage`, `maxBindGroups`,
  etc. when the adapter offers them (`platform/web/js/engine/engine.js:379-428`), so
  limits are not expected to bind. Clamp 0-size dispatches to ≥1 (`rendering_device.cpp`
  rejects 0, and tiny viewports exist: `renderer_viewport.cpp:271-283`).
- No subgroups, no f16, no override constants (`SUPPORTS_HALF_FLOAT` false
  `RDW:10652-10654`; raw WGSL has no `@id`/spec-constant path, `docs/WGSL_COMPUTE_PATH.md`
  §11.2 item 15). `workgroupBarrier()`/`var<workgroup>` are core WGSL and need no driver
  support (they are not bindings; the WD scan only rewrites/scans `@group`-bound
  declarations).
- Dispatch/barrier discipline: write storage then read it as a sampled texture in a later
  dispatch → explicit `compute_list_add_barrier()` between passes (the driver's usage guard
  also splits, `RDW:9935-10010`, but be explicit; the spatial effect's two-pass code at
  `pmndrs_upscaler.cpp:357-365` is the template).

**(G8) Constants ring / uniform-set churn.**

- `buffer_update()` must happen before `compute_list_begin()` (`docs/WGSL_COMPUTE_PATH.md`
  §7 gotcha 4); one temporal frame has multiple passes and (across viewports) multiple
  viewports, so size the constants ring like the spatial effect's 64 slots
  (`pmndrs_upscaler.h:74-78`) or larger, and never reuse a slot whose dispatch is already
  recorded.
- Use `UniformSetCacheRD` (`pmndrs_upscaler.cpp:313,328`) for per-pass sets; the parity of
  history ping-pong changes the bound RIDs every frame, so create sets per frame through
  the cache (it hashes and evicts). Do **not** cache a set across a history swap while the
  RID is freed/recreated.
- **Two bind groups** are only needed by the optional `EveryTexelExposure` metering shader,
  which rewrites its own declarations from `@group(0)` to `@group(1)` (upstream
  `packages/core/src/shaders/luminancePyramid.ts:249-280`). v1 can start with tap-based metering (single group) and
  add multi-group later; when added, verify contiguous groups 0 and 1 with both sets
  non-empty (RD rejects empty sets, `rendering_device.cpp:4720-4724`; driver pipeline
  layout handles gaps `RDW:5584-5680`).

---

## 6. Integration design: `RendererRD::PmndrsTemporal`

### 6.1 Ownership and files

New files (mirroring milestone 1's effect layout):

- `servers/rendering/renderer_rd/effects/pmndrs_temporal.h`
- `servers/rendering/renderer_rd/effects/pmndrs_temporal.cpp`
- `servers/rendering/renderer_rd/effects/pmndrs_temporal_wgsl.h` (WGSL embedded as C++
  strings, generated/vendored by the port task; no runtime asset loading; keep the MIT +
  AMD notices)

**In-progress worktree draft:** `pmndrs_temporal.h` already exists (untracked, written
2026-11-28 00:00) as a context-based skeleton — `create_context(internal, target)`,
`free_context`, `upscale(context, Parameters)`, `is_supported()`, and a `Parameters` struct
with the eight RIDs, sizes, near/far/fovy, jitter/prev, delta_time, sharpness,
`reset_accumulation`, `reversed_depth`. It is compatible with this guide's seam except for
two contract points, which should be resolved before the renderer wiring lands (§6.8).

Owner: `RendererSceneRenderRD` (protected member next to `pmndrs_spatial =
nullptr` at `renderer_scene_render_rd.h:74`; created with
`can_use_storage && RD::has_feature(SUPPORTS_RAW_WGSL_COMPUTE)` in the constructor
`renderer_scene_render_rd.cpp:1936-1941`, deleted at 1959). Rationale: the base class's
`render_scene()` is where `begin_frame()` must run (§2.6), and `RenderForwardClustered` can
still call `upscale()` from `_render_scene()` because the member is protected. The effect
is inert (not created / `is_supported()==false`) on every non-WebGPU driver and on
Forward Mobile.

### 6.2 Class surface

```cpp
namespace RendererRD {

// Milestone 2: pmndrs temporal upscaler (FSR2/3-style). Plugged in where FSR2 is
// (RenderForwardClustered, pre-tonemap), selected by
// VIEWPORT_SCALING_3D_MODE_PMNDRS_TEMPORAL, raw-WGSL compute (WebGPU only).
class PmndrsTemporal {
public:
	PmndrsTemporal();
	~PmndrsTemporal();

	bool is_supported() const { return supported; }

	// Returned by begin_frame(); the renderer converts it into the camera
	// projection jitter (see docs/GODOT_TEMPORAL_UPSCALER_INTEGRATION.md §2.3).
	struct FrameJitter {
		Vector2 jitter;      // render px, [-0.5,0.5], x right, y down (pmndrs convention)
		Vector2 jitter_prev; // previous dispatched frame, same convention
		uint32_t phase = 0;  // 0 .. jitter_phase_count-1
		bool reset = false;  // history was (re)initialised this frame
	};

	// Called once per viewport per frame, before any 3D rendering.
	// Advances the jitter sequence, (re)allocates per-size history if needed and
	// latches this frame's jitter for upscale(). p_camera_cut forces a history
	// reset (teleport/FOV jump) in addition to the configure() reset.
	FrameJitter begin_frame(Ref<RenderSceneBuffersRD> p_render_buffers, bool p_camera_cut = false);

	// Mirrors FSR2Effect::Parameters (+ the pmndrs-specific camera facts).
	struct Parameters {
		// inputs, one view (call once per view; v1: view_count == 1)
		RID color;       // rb->get_internal_texture(v)          RGBA16F render res, linear HDR
		RID depth;       // rb->get_depth_texture(v)             hardware reversed-Z depth
		RID velocity;    // rb->get_velocity_buffer(false, v)    RG16F render res
		RID exposure;    // luminance->get_current_luminance_buffer(rb) 1x1 R32F (or RID())
		RID reactive;    // v1: RID() (alpha-view is a no-op on WebGPU, see §3.2)
		RID output;      // rb->get_upscaled_texture(v)          RGBA16F target res
		Size2i internal_size;
		Size2i target_size;
		float z_near = 0.0f;
		float z_far = 0.0f;
		bool is_orthogonal = false;
		float delta_time = 0.0f;    // seconds
		float sharpness = 0.0f;     // rb->get_fsr_sharpness()
		bool reset_accumulation = false;
		Projection reprojection;    // unjittered prev/cur sandwich (same as FSR2)
	};
	void upscale(const Parameters &p_params);

private:
	// Per-render-buffer persistent state (history ping-pong, guides, storage
	// buffers, jitter sequence state). Registered as RenderSceneBuffersRD custom
	// data so configure()/cleanup() re-create/free it on resize/destroy.
	class ViewState : public RenderBufferCustomDataRD {
		GDCLASS(ViewState, RenderBufferCustomDataRD);
	public:
		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;
		// booked by begin_frame(), consumed by upscale():
		Vector2 jitter;
		Vector2 jitter_prev;
		uint32_t phase = 0;
		uint32_t frame_index = 0;
		bool reset_pending = true;
		bool frame_begun = false;
		// RIDs: history[2], depth_history[2], guides..., constants ring slots...
	};

	ViewState *get_view_state(Ref<RenderSceneBuffersRD> p_render_buffers); // create + configure + register
	// ... shaders/pipelines/samplers/constants ring (private, like PmndrsSpatial) ...
	bool supported = false;
};
}
```

Implementation notes:

- `ViewState` should be registered exactly like `Luminance::LuminanceBuffers`:
  `p_render_buffers->set_custom_data(SNAME("pmndrs_temporal"), state)`
  (`luminance.cpp:135-147` pattern) after a manual `state->configure(rb.ptr())` on first
  use; subsequent resizes are handled by `RenderSceneBuffersRD::configure()`'s loop over
  `data_buffers` (`render_scene_buffers_rd.cpp:211-213`) and by `cleanup()`
  (`render_scene_buffers_rd.cpp:127-148`). `configure()` must therefore be idempotent and
  must recreate all per-size resources with the new size and set `reset_pending = true`.
- `begin_frame()`: `get_view_state(rb)`, call the ported core's `beginFrame()` (or advance
  the store `JitterSequence` itself), store `jitter/jitter_prev/phase`, and set
  `frame_begun = true`. On `reset_pending`, call the core's `resetHistory()` and report
  `reset = true`.
- `upscale()`: dispatch the ported core's `dispatch()`/`encode()` (or the hand-ported
  sequence). The core's allocator callback creates textures through
  `rb->create_texture(RB_SCOPE_BUFFERS, SNAME("pmndrs_temporal/…"), format, usage, …)` /
  `RD::storage_buffer_create()` and registers the resulting RIDs in the `ViewState`.
  All `buffer_update()`/`texture_update()` calls happen **before**
  `compute_list_begin()`. Use one `RD::ComputeListID` for the whole pass chain with
  `compute_list_add_barrier()` between read-after-write dependencies
  (`pmndrs_upscaler.cpp:350-367` is the two-pass template).
- `upscale()` must verify the inputs it actually received: colour is `RGBA16F` and
  render-res, output is `RGBA16F` and target-res, velocity is `RG16F`
  (`rd->texture_get_format(...)`, as the spatial effect checks its destination at
  `pmndrs_upscaler.cpp:266-274`); if not, warn once and return without touching the frame
  (the renderer will present the internal texture; `use_upscaled_texture` is true but the
  texture is untouched — better: fall back by clearing to a copy or keep mode off. Flag as
  a v1 edge case).
- One-time console evidence lines (used by the browser-verification recipe):
  `[PMNDRS] temporal upscaler active (raw WGSL)` in the constructor and
  `[PMNDRS] temporal upscaler running (…) WxH -> WxH, NxM groups` on first actual frame,
  mirroring `pmndrs_upscaler.cpp:118-122,337-343`.

### 6.3 Where `RenderForwardClustered` calls it

The upscale branch in §4.5, sketched:

```cpp
} else if (scale_type == SCALE_PMNDRS_TEMPORAL) {
	RD::get_singleton()->draw_command_begin_label("PMNDRS Temporal");
	RENDER_TIMESTAMP("PMNDRS Temporal");

	RID exposure;
	if (RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
		exposure = luminance->get_current_luminance_buffer(rb);
	}

	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		real_t fov = p_render_data->scene_data->cam_projection.get_fov();
		real_t aspect = p_render_data->scene_data->cam_projection.get_aspect();
		real_t fovy = p_render_data->scene_data->cam_projection.get_fovy(fov, 1.0 / aspect);

		RendererRD::PmndrsTemporal::Parameters params;
		params.color = rb->get_internal_texture(v);
		params.depth = rb->get_depth_texture(v);
		params.velocity = rb->get_velocity_buffer(false, v);
		params.exposure = exposure;
		params.reactive = RID();                       // see §3.2
		params.output = rb->get_upscaled_texture(v);
		params.internal_size = rb->get_internal_size();
		params.target_size = rb->get_target_size();
		params.z_near = p_render_data->scene_data->z_near;
		params.z_far = p_render_data->scene_data->z_far;
		params.is_orthogonal = p_render_data->scene_data->cam_orthogonal;
		params.delta_time = float(time_step);
		params.sharpness = CLAMP(1.0f - (rb->get_fsr_sharpness() / 2.0f), 0.0f, 1.0f);
		params.reset_accumulation = false;              // see §6.5
		Projection correction;
		correction.set_depth_correction(true, true, false);
		const Projection &prev_proj = p_render_data->scene_data->prev_cam_projection;
		const Projection &cur_proj = p_render_data->scene_data->cam_projection;
		const Transform3D &prev_transform = p_render_data->scene_data->prev_cam_transform;
		const Transform3D &cur_transform = p_render_data->scene_data->cam_transform;
		params.reprojection = (correction * prev_proj) * prev_transform.affine_inverse() * cur_transform * (correction * cur_proj).inverse();
		(void)fovy; // pmndrs takes near/far/ortho; fovy only if its constants ask for it
		pmndrs_temporal->upscale(params);
	}
	RD::get_singleton()->draw_command_end_label();
}
```

`begin_frame()` must already have been called (§2.6) — `upscale()` reads `ViewState`'s
latched jitter/prev; it never re-derives jitter itself.

### 6.4 Jitter ownership summary

- Single source: `PmndrsTemporal::ViewState` (advance in `begin_frame`, consume in
  `upscale`).
- Renderer applies it by writing `scene_data.taa_jitter`/`prev_taa_jitter` before
  `_render_scene()`; the projection UBO, motion pass, sky, contact shadows and debug draws
  all follow (§2.1/§2.6).
- Motion vectors stay unjittered with **zero shader edits** because
  `scene_forward_clustered.glsl:3216-3223` subtracts the jitter it finds in the UBO.
- `begin_frame()` is called only when `upscale()` will run in the same call tree:
  `!reflection_probe`, mode == 7, effect supported, `view_count == 1`. If the upscale
  early-returns (format guard), the effect's own `_frameUndispatched`/rollback mechanism
  must be used so the phase is not skipped — the port adapter should expose a
  `cancel_frame()`/`rollback()` if the core's `beginFrame()` bookkeeping is not enough
  [port contract].
- No public `RendererSceneRender`/`RendererSceneCull` API changes are needed. The
  alternative (a virtual `get_viewport_jitter()` on `RendererSceneRender` called from
  `RendererSceneCull::render_camera()`, `renderer_scene_cull.cpp:2709-2730`) is rejected:
  it splits current/previous jitter across the camera-data and effect state, still requires
  overriding `prev_camera_data`, and forces every renderer implementation to carry the
  hook.

### 6.5 Reset / camera cuts / resize

- **Resize / format change / mode switch / new rb**: `RenderSceneBuffersRD::configure()`
  re-runs `ViewState::configure()`, which re-creates history at the new size and sets
  `reset_pending = true`. `begin_frame()` then calls the core's `resetHistory()` and
  reports `reset = true`; the renderer reports `prev_taa_jitter = 0` (no previous jitter).
- **Camera cut / teleport / FOV change**: the engine has no explicit "camera cut" signal
  (FSR2's `reset_accumulation` is hard-coded false with a FIXME,
  `render_forward_clustered.cpp:2604`). v1: rely on the core's disocclusion/depth-clip
  path (it handles most cuts by itself because history is depth-tested). Optional
  follow-up (small, self-contained): in `render_scene()`, set
  `reset = true` when `p_camera_data->main_projection != p_prev_camera_data->main_projection`
  (raw projections, jitter excluded) or when the camera origin jumps more than a threshold;
  feed that into `begin_frame(rb, /*p_camera_cut=*/true)`. Add a `RenderingServer`-level hook only if the
  demo needs explicit cuts.
- **Mode switches** (mode 6 ↔ 7 ↔ FSR2): TAA conflict and buffer reconfiguration are
  handled by the viewport layer; switching to mode 7 must recreate `ViewState` (it is
  created on first `begin_frame`, and the rb is reconfigured on the mode change →
  `configure()` path resets history).
- **`scale == 1.0`**: history is the same size as the render; keep it (AA benefit).

### 6.6 Presentation / post-process ordering (no changes beyond §4.6)

- Tonemap reads the upscaled texture (`renderer_scene_render_rd.cpp:838`), so the effect's
  output must be **linear HDR, pre-tonemap** — pmndrs's output is documented as staying in
  the input's domain, so feed it the linear internal colour and do not tonemap in the
  effect.
- Glow/DoF still source the internal (render-res) colour, exactly like FSR2
  (`renderer_scene_render_rd.cpp:638,514`); this is existing behaviour, not a regression.
- Alpha: the internal colour's alpha is passed through the effect's RCAS alpha source
  [port contract] — required for transparent viewports and for the reactive-mask-alpha
  convention if that is ever restored.

### 6.7 Demo-facing exposure

`Viewport.scaling_3d_mode` / `RenderingServer.viewport_set_scaling_3d_mode` /
project setting `rendering/scaling_3d/mode` all carry the enum int; mode 7 is value 7. The
TPS demo (external project) needs its Settings UI updated as described in §4.7. Nothing
engine-side beyond the enum/hints edits.

### 6.8 Reconciliation with the in-progress `pmndrs_temporal.{h,cpp}` draft

The draft header in the worktree (2026-11-28 00:00) takes the FSR2 shape:
`create_context(internal, target)` → per-render-buffer state, `upscale(context, params)`.
That is fine and equivalent to the `ViewState` design above (implement `ViewState` as a
`RenderBufferCustomDataRD` that *owns* the context); the review points are:

1. **Jitter ownership.** The draft's `Parameters` carries `jitter`/`jitter_prev`
   "in render pixels with the engine/FSR2 convention (x right, y up; the effect converts to
   pmndrs' x right / y down itself)". Two problems with that wording:
   - the engine's `taa_jitter` is a **clip-space offset** (not a pixel offset, not y-up);
     the only well-defined engine-convention pixel value is the one FSR2 receives,
     `taa_jitter * internal_size * 0.5`, and per §2.3 the pmndrs sample offset is its
     **negation** in pixel space: `j_pmndrs = -(taa_jitter * internal_size * 0.5)`;
   - `jitterPrevious` must be the previous *dispatched* frame's jitter, and the effect must
     handle the begin-but-not-dispatched rollback (upstream `beginFrame()` semantics).
   Preferred fix: keep the guide's `begin_frame(rb) -> FrameJitter` (effect owns the
   sequence and returns both offsets; renderer converts to `taa_jitter` and passes the same
   pixel offsets back in `Parameters`), because it removes the conversion ambiguity from
   the adapter and keeps one source of truth. If the context-based API is kept, add
   `FrameJitter begin_frame(context, rb)` to it and have `Parameters.jitter` be the exact
   `begin_frame()` value.
2. **Depth convention.** The draft defaults `reversed_depth = false`. Godot Forward+ is
   reversed-Z in [0,1] (§2.3, evidence: `Projection::set_depth_correction` defaults,
   depth cleared to 0.0f, FSR2 configured with `FFX_FSR2_ENABLE_DEPTH_INVERTED`). The
   effect must pass `reversedDepth = true` / set `FLAG_REVERSED_DEPTH` unless a
   per-viewport probe says otherwise (e.g. a future compatibility renderer). A mismatch
   throws in the pmndrs linearization, so it will be caught, but it should not ship as a
   "surprise".
3. **Velocity conversion.** The draft says "UV delta" for velocity. The engine value is
   `prevUV − curUV` in y-down UV; the effect must configure `motionScale = [-1,-1]` (§2.3)
   rather than relying on the core default `[0.5,-0.5]`. Put the constant in the adapter,
   not in the shaders.
4. **Context lifetime.** `create_context(internal, target)` must be re-run when
   `RenderSceneBuffersRD::configure()` re-creates the buffers (size/format/mode change);
   the simplest correct home is `RenderBufferDataForwardClustered` (like `fsr2_context`,
   `render_forward_clustered.h:98,147` + `render_forward_clustered.cpp:93-97,134-137`) or a
   `RenderBufferCustomDataRD` `configure()` hook (`render_scene_buffers_rd.cpp:211-213`).
   The draft's `free_context()` must be called from `free_data()`/`cleanup()`.
5. **Reset.** The draft's `reset_accumulation` is a good addition over FSR2 (whose flag is
   hard-coded false, `render_forward_clustered.cpp:2604`); the renderer should set it from
   the `begin_frame()` reset state (§6.5) and from the mode-switch/reconfigure path.

---

## 7. Ordered implementation step list

Prereq: `docs/PMNDRS_TEMPORAL_PORT.md` exists (pass graph, bindings, constants, vendored
WGSL) and the vendored WGSL is embedded under `effects/pmndrs_temporal_wgsl.h`.

1. **Driver prerequisites (§5), in this order:**
   a. Add the raw-WGSL storage-buffer/atomics + sampler smoke-test shaders (throwaway
      `RD::Compute` test or a debug effect) and confirm on Chrome + Firefox/Safari at least
      once. Fix anything the smoke tests reveal (expected: nothing for G3/G4).
   b. Verify **G1/G2** in the current worktree: the Gap B depth-only view
      (`_depth_only_view()`) and the Gap A raw-WGSL auto-layout sample types
      (`docs/PMNDRS_TEMPORAL_PORT.md` §12.3) must be present and covered by smoke tests.
      If absent in your tree, implement them per §5.2.
   c. Re-run the existing web template compile check + a Forward+ browser frame to confirm
      no regression in the Tint/raster paths (the Gap A fix is deliberately scoped to
      `raw_wgsl_shader`; confirm SPIR-V/Tint BGLs are untouched).
2. **Effect skeleton** (`pmndrs_temporal.{h,cpp}`, `_wgsl.h`, `SCsub`): constructor probes
   (`SUPPORTS_RAW_WGSL_COMPUTE`, RGBA16F sampling+storage, G2 gate), create shaders and
   compute pipelines, create the linear clamp sampler and the constants ring; add to
   `RendererSceneRenderRD`'s creation/destruction. Compile (web template) at this point.
3. **`ViewState`**: per-rb custom data, `configure()`/`free_data()`, history/guide/storage
   resource creation at the current internal/target size, `reset_pending`, registration
   pattern from `Luminance::LuminanceBuffers`. Add the `begin_frame()` skeleton returning a
   fixed zero jitter to unblock wiring.
4. **Port the pass graph** into `upscale()` per the port contract: constants upload before
   the compute list, per-pass uniform sets through `UniformSetCacheRD`, barriers between
   read-after-write passes, output into `Parameters::output`. Add the "[PMNDRS] temporal
   … running" log.
5. **Renderer seam**: `RendererSceneRenderRD::render_scene()` jitter override (§2.6);
   `RenderForwardClustered` `scale_type` case + velocity clear + `upscale(params)` branch
   (§4.5); add mode 7 to `use_upscaled_texture` (§4.6).
6. **Scaling mode 7 plumbing** (§4.1-4.4, §4.7): enums, type helper, bindings, hints,
   viewport guards/fallbacks/size switch, MSAA policy, docs XML.
7. **Verification pass A — conventions**: jitter sign/axis, velocity sign/scale,
   depth reversed-Z, near/far, reprojection matrix (§2.7 recipes). Fix the conversions
   before judging image quality; these are the failure modes that look like "the upscaler
   is bad".
8. **Verification pass B — lifecycle**: resize window, change `scaling_3d_scale`, switch
   modes live, camera cuts, viewport hidden/shown, project reload, transparent background,
   HDR/tonemap variants, glow/DoF enabled, orthographic camera.
9. **Verification pass C — perf & quality**: fixed-camera A/B screenshots vs FSR1/mode 6,
   draw/GPU time, VRAM, and the "running" log; TPS demo button; update
   `docs/PATCH_STACK.md`/`docs/WEBGPU_HANDOFF.md` (other owners) and regenerate the patch
   stack.
10. **Optional follow-ups**: reactive-mask extract pass, camera-cut reset heuristics,
    multiview/XR support, EveryTexelExposure (second bind group), sky-motion fallback,
    `reset_accumulation` plumbed from `RenderingServer`.

---

## 8. Risks / open questions

1. **Jitter sign/axis** (§2.3/2.7) is the highest-risk item: a wrong sign is not a
   validation error, only doubled/blurred edges. The mapping derived here is
   `taa_jitter = (-2*jx/W, -2*jy/H)` (pmndrs jitter is a render-pixel sample offset; the
   projection offset is its negation in the final WebGPU clip, and Godot's projection
   carries a `flip_y` while the WebGPU container negates output Y via
   `negate_position_y`). `docs/PMNDRS_TEMPORAL_PORT.md` §5.6 proposes the same X but the
   opposite Y; run the numeric test in §2.7 before merging either, and keep the sign as a
   single named constant/factor in `PmndrsTemporal` so it can be flipped in one place.
2. **Velocity convention** (`motionScale = [-1,-1]`, derived in §2.3) must be confirmed
   against the port contract and validated with `VIEWPORT_DEBUG_DRAW_MOTION_VECTORS` +
   camera pan; a sign error smears moving objects in the wrong direction.
3. **Invalid-motion sentinel**: Godot clears velocity to `(-1,-1,0,0)`
   (`render_forward_clustered.cpp:2335`) for FSR2's derive-invalid option; pmndrs has no
   such sentinel. Use a `(0,0,0,0)` clear for mode 7 (recommended) or add sentinel handling
   in the adapter. Otherwise sky/background pixels get a fake 1-texel motion per frame.
4. **Sky motion**: with velocity 0 on sky pixels, a rotating camera will ghost the
   background. FSR2 solves this by deriving motion from depth+reprojection; pmndrs's
   depth clip works on depth but the background has no geometry. Options: derive
   camera-only motion for depth==far pixels in the adapter (a small extra pass or a variant
   of reconstruct), or accept it and document. Open question for the port contract.
5. **G1 (depth aspect) and G2 (unfilterable r32float)** are hard blockers: without them the
   temporal path either fails bind-group validation or silently binds blank/quantized
   fallbacks. Do not start quality work before both are verified with smoke tests.
6. **G3/G4 unexercised raw-WGSL features** (storage buffers/atomics, samplers): code paths
   look complete but no shader in the tree uses them; a Dawn validation surprise here
   delays the whole port. Smoke-test first (step 1a).
7. **Reactive mask unavailable** on WebGPU (§3.2); expect more ghosting on transparent/
   particle overdraw than FSR2. v1 skips reactivity; a later alpha-extract pass can help.
8. **MSAA**: pmndrs requires single-sample input; mode 7 must disable 3D MSAA (mirroring
   MetalFX temporal). If users need MSAA, a separate depth variant + G2 is required.
9. **Multiview/XR**: FSR2 itself is effectively untested for XR; v1 of mode 7 should
   require `view_count == 1` and fall back otherwise.
10. **Reset on camera cuts**: no engine signal; v1 relies on disocclusion. Teleports and
    hard cuts may leave 1-2 frames of ghosting. Add the projection/transform-change
    heuristic later (§6.5).
11. **Held frames / begin-vs-dispatch bookkeeping**: `begin_frame()` runs in
    `render_scene()`; if `upscale()` is skipped for any reason after that, the effect's
    phase/frame-index bookkeeping must be rolled back (port contract must expose this, or
    the renderer must guarantee the call pair). Add an assert/counter in debug builds.
12. **VRAM/perf**: 2×RGBA16F render-res history + guides + storage buffers is a real
    budget (tens of MB at 1080p); the full FSR2-style graph on WebGPU compute may be
    expensive on integrated GPUs. Ship gate: `is_supported()` + perf measurements, and
    keep modes 6/1 as the cheap options.
13. **`float32-filterable` variability** (G2 option 2): engines/browsers without the
    feature cannot run mode 7; decide the support matrix before advertising the mode.
14. **Effect output format assumption**: `Parameters::output` is RGBA16F only because
    Forward+ uses RGBA16F (`renderer_scene_render_rd.cpp:1209-1211`). If a future mobile/
    desktop format preference changes it (e.g. A2B10G10R10 on mobile), the effect must
    check and fall back; guard with `rb->get_base_data_format()`.
15. **Exposure semantics**: Godot's luminance buffer is average luminance, while pmndrs's
    `exposureTexture` is an app exposure; confirm the mapping in the port contract before
    binding it (or use the fixed-exposure fallback in §3.3).
16. **Uniform-set churn / ring sizing** across multiple viewports and frames (§5.2 G8);
    a too-small ring serializes the CPU/GPU or binds stale constants (visible as
    "the effect uses the previous frame's jitter").
17. **Docs/build choreography**: mode 7 touches files listed in
    `patches/layers/0005-engine-renderer-patches.files` (and new effect files); the patch
    stack must be regenerated and round-trip verified (per `docs/PATCH_STACK.md`). Keep
    `doc/classes/*.xml` edits in the same patch.

---

## 9. Quick evidence index

| claim | location |
| --- | --- |
| FSR2 scale selection / motion requirement / upscale call / tonemap order | `render_forward_clustered.cpp:1862-1907,1928,2459-2464,2530-2558,2572-2653` |
| FSR2 per-rb context + lifetime | `render_forward_clustered.cpp:93-97,118-149,287-295,5445,5464-5467`; `render_forward_clustered.h:98,147` |
| FSR2 Parameters + dispatch mapping | `fsr2.h:156-173`; `fsr2.cpp:909-952` |
| FSR2 WebGPU disable (B10G11R11) | `fsr2.cpp:516-527`; `renderer_viewport.cpp:175-190`; `render_forward_clustered.cpp:1868-1878` |
| upscaled texture creation/access/presentation | `render_scene_buffers_rd.cpp:721-728`; `render_scene_buffers_rd.h:310-320`; `renderer_scene_render_rd.cpp:484,514-515,838` |
| jitter chain (Halton → CameraData → SceneData → UBO) | `renderer_viewport.cpp:302-313,358`; `renderer_scene_cull.cpp:54-64,2709-2730,3727-3737`; `renderer_scene_render_rd.cpp:1428-1445`; `render_scene_data_rd.cpp:42-66,74-122,275-305` |
| motion vectors subtract jitter | `scene_forward_clustered.glsl:3215-3222`; `taa_resolve.glsl:316-319` |
| velocity format/resolve/clear | `render_scene_buffers_rd.cpp:730-740,866`; `render_forward_clustered.cpp:2536-2554,2335` |
| depth format/usage | `render_scene_buffers_rd.cpp:811-826,845-863` |
| exposure/luminance | `luminance.cpp:86-157`; `render_forward_clustered.cpp:2576-2579` |
| reactive view (swizzle) | `render_scene_buffers_rd.h:266-273`; `rendering_device_driver_webgpu.cpp:1929-1934,2092-2148` |
| scaling mode enum/type/hints/bindings | `rendering_server_enums.h:470-499`; `scene/main/viewport.h:104-111`; `scene/main/viewport.cpp:5423,5489`; `rendering_server.cpp:2938,3786-3797`; `render_scene_buffers.cpp:55` |
| viewport guards/fallbacks/sizes/jitter phase | `renderer_viewport.cpp:136-333,1049,1072-1112,1175-1179` |
| raw-WGSL RD API | `rendering_device.cpp:4711-4797`; `rendering_device.h:1072-1103`; `rendering_device_commons.h:1041-1044`; `docs/WGSL_COMPUTE_PATH.md` §5/§7/§11 |
| raw-WGSL WebGPU driver support/gaps | `rendering_device_driver_webgpu.cpp:10652-10690,1973-1985,2092-2190,2274-2420,3051-3111,3231-3242,4405-4700,4713-4723,4740-4775,4810,4935-4980,4986-5065,5193-5206,5242-5292,5377-5410,5953-6020,6013-6015,6076-6090,9935-10010` |
| WebGPU shell features/limits | `platform/web/js/engine/engine.js:340-430` |
| pmndrs jitter/velocity/format contract | upstream `docs/engine-contract.md#jitter,#motion`; `packages/core/src/shaders/common.ts:30-64`; `packages/core/src/UpscalerCore.ts:760-790,900-945`; sibling `docs/PMNDRS_TEMPORAL_PORT.md` |
| spatial effect as raw-WGSL/godot pattern | `effects/pmndrs_upscaler.{h,cpp}`; `docs/PMNDRS_UPSCALER_SPATIAL.md` |
