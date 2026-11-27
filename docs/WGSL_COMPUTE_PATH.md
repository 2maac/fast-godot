# Raw-WGSL compute path for the WebGPU driver — design

Status: **implemented and browser-verified 2026-11-27.** §1–§10 are the design as written
before implementation; **§11 "Implementation notes (as built)" is the authoritative record**
of what landed, including the few deviations. The pmndrs spatial effect that consumes the path
is documented in `docs/PMNDRS_UPSCALER_SPATIAL.md`, and the browser evidence is in
`docs/WEBGPU_HANDOFF.md` ("pmndrs/upscaler spatial upscaler on WebGPU (milestone 1)").
Engine tree: `/tmp/godot48` = Godot 4.8.0-beta `67421f36e5` + the committed `patches/*.patch`
stack (byte-identical to the worktree). Every code reference below was verified in that tree.
The raw-WGSL path itself only changes files already listed in
`patches/layers/0004-webgpu-driver.files` and `patches/layers/0005-engine-renderer-patches.files`;
the pmndrs effect added three new files to the 0005 layer list (see §11).

Scope of this document: the shader-creation plumbing needed to run the hand-written WGSL
spatial EASU/RCAS compute shaders from `pmndrs/upscaler` without SPIR-V and without Tint.
It does not cover the upscaler pass graph / temporal path / renderer wiring; those are a
separate work item (see `docs/WEBGPU_HANDOFF.md`, "WebGPU-native upscaler port").

---

## 0. TL;DR

1. **The container hypothesis is confirmed, with one correction.** The WebGPU shader container
   already carries per-stage WGSL that the driver prefers over SPIR-V
   (`RenderingShaderContainerWebGPU::stage_wgsl`, `HEADER_FLAG_HAS_WGSL`,
   `drivers/webgpu/rendering_shader_container_webgpu.h:43-57,79`,
   consumed at `drivers/webgpu/rendering_device_driver_webgpu.cpp:4103-4107`). But there is
   **no public API to build such a container from a raw WGSL string + reflection**, because
   `RenderingShaderContainer::set_from_shader_reflection()`, `ReflectShader` and the
   `reflection_*` storage are all `protected`
   (`servers/rendering/rendering_shader_container.h:47-281`).
2. **Cheapest path:** add a small WebGPU-only factory
   `RenderingShaderContainerWebGPU::set_code_from_wgsl_compute(...)` (it is a derived class, so
   it can fill the protected reflection storage), expose it through one new driver virtual
   `RenderingDeviceDriver::shader_create_from_wgsl(...)` (default implementation fails cleanly,
   so Vulkan/Metal/D3D12/GLES3 need no edits), and add
   `RenderingDevice::shader_create_from_wgsl(const WGSLShaderData &)` that validates, builds
   `RenderingDeviceCommons::ShaderReflection`, calls the driver and registers the RID. Existing
   `shader_create_from_container`, uniform sets, compute pipelines and compute lists are then
   used unchanged.
3. Only **two edits are needed in the existing build path**:
   `_shader_build()` must accept a stage with WGSL but no SPIR-V
   (`rendering_device_driver_webgpu.cpp:4086` currently rejects it), and the Tint-output-specific
   WGSL text rewrites (`:4117-4622`) must be skipped for raw-WGSL stages.
4. The **WGSL binding convention is the #1 trap**: the driver multiplies every Godot binding by 2
   (`entry.binding = u.binding * 2`, e.g. `:5149,5164,5189-5201,5232`) because
   `spirv_preprocess.cpp:1341` doubles SPIR-V bindings before Tint. Hand-written WGSL must declare
   `@binding(2*N)` for reflected binding `N`. The container helper applies this rewrite
   automatically (§5).
5. Rejected alternatives (driver-level pipeline/dispatch API, exposing the driver from RD,
   hand-built serialized container bytes, GLSL wrapper + Tint round-trip) are in §2.
6. Compile check (~5–15 min incremental, command in §9) and Tint-bypass verification are in §9;
   top risks are in §10.

---

## 1. Verdict: use the container route (with a WebGPU container factory + one driver virtual)

### 1.1 What the container already provides

`RenderingShaderContainerWebGPU` is exactly a "reflection + per-stage code" carrier:

| piece | evidence |
|---|---|
| WGSL per stage, empty when the driver should translate SPIR-V itself | `rendering_shader_container_webgpu.h:54-57,79`; filled by `_set_code_from_spirv()` at `rendering_shader_container_webgpu.cpp:57-115` (only under `WEBGPU_BAKE_WGSL`) |
| `HEADER_FLAG_HAS_WGSL` + `HeaderData` (push-constant slot, no-push-constant sentinel) | `rendering_shader_container_webgpu.h:44-51` |
| serialization of per-stage WGSL | `rendering_shader_container_webgpu.cpp:121-169`; container base `to_bytes()`/`from_bytes()` at `rendering_shader_container.cpp:801-1000` |
| reflection storage (per-set binding tables, compute local size, stages, push constants) | `rendering_shader_container.h:58-107`; `set_from_shader_reflection()` `rendering_shader_container.cpp:683-737`; `get_shader_reflection()` `:746-798` |
| the public reflection type the driver consumes | `RenderingDeviceCommons::ShaderReflection` / `ShaderUniform` at `rendering_device_commons.h:1119-1180` |

The driver consumes the container in a **deferred** build:
`shader_create_from_container()` only allocates a `WGShader` and keeps the container
(`rendering_device_driver_webgpu.cpp:3951-3960`, `webgpu_objects.h:144-149`); the first
`compute_pipeline_create()` or `uniform_set_create()` runs `_shader_ensure_built()` →
`_shader_build()` (`:3963-3972`, `:3974`). `_shader_build()` creates one `WGPUShaderModule` per
stage from `get_stage_wgsl(i)` when present, else from Tint (`:4103-4107`), builds one
`WGPUBindGroupLayout` per uniform set from the reflection *plus a WGSL text scan* (`:5069-5470`),
one `WGPUPipelineLayout` (`:5478-5600`), and `shader_free()` releases them (`:5664-5686`).
Compute pipelines use entry point `"main"` (`:10021`) and the shader's pipeline layout
(`:9968-10030`). Uniform sets, compute lists, usage tracking and pass splitting then work
unchanged (see §4).

So: the *shape* is right; what is missing is a way to put raw WGSL + hand-built reflection into
a `RenderingShaderContainerWebGPU` and get a driver `ShaderID` out of it. Adding that reuses
every downstream mechanism instead of duplicating it.

### 1.2 Corrections to the task's hypothesis (API names to not use)

Verified absent in the 4.8-beta tree:

- `RenderingDevice::shader_create_from_container()` — does not exist. RD has
  `shader_create_from_bytecode()` (`rendering_device.h:1059`, `rendering_device.cpp:4596`), which
  parses a serialized container and calls `driver->shader_create_from_container()`
  (`rendering_device_driver.h:570`). The driver method exists; the RD method is bytecode-only.
- `shader_get_uniform_set_size()` — does not exist.
- `_uses_combined_image_samplers` / `get_uniform_set_count()` — do not exist in this tree. The
  combined-sampler behaviour is unconditional: `spirv_preprocess` doubles every binding and splits
  combined image-samplers (`spirv_preprocess.cpp:847-848,1230-1350`), and the driver expands
  `UNIFORM_TYPE_SAMPLER_WITH_TEXTURE` into two BGL entries at `binding*2` / `binding*2+1`
  (`rendering_device_driver_webgpu.cpp:5189-5225`).
- `_shader_owner` / `_texture_owner` — the real owners are `shader_owner`
  (`rendering_device.h:971`), `texture_owner` (`:424`), `uniform_buffer_owner`/`storage_buffer_owner`
  (`:218-219`). RID→driver-id translation is `owner.get_or_null(rid)->driver_id`; that is exactly
  why a driver-level API cannot be driven from renderer code without exposing these (see §2).
- `RenderingDevice::Shader` is `struct Shader : public ShaderReflection` (`rendering_device.h:961`);
  RD copies the container reflection into it verbatim on creation
  (`rendering_device.cpp:4642`), then sorts each set and assigns a uniform-set format hash
  (`:4648-4668`). Any new shader-creation path must do the same.

---

## 2. Rejected alternatives

### 2.1 New driver-level pipeline/dispatch API ("keep it private to the effect")

(The handoff's original recommendation, `docs/WEBGPU_HANDOFF.md` §"Integration shape" item 2.)

Rejected because:

1. **Renderer code cannot reach driver handles.** Driver IDs (`ShaderID`, `PipelineID`,
   `UniformSetID`, `TextureID`) are opaque and owned inside `RenderingDevice`'s RID owners
   (`rendering_device.h:218-219,424,961-971`). A `renderer_rd/effects/*` class holds RD RIDs, not
   driver IDs. Exposing a "create compute pipeline/dispatch from the driver" API requires new
   RD→driver-id accessors and a handle on RD's command buffer/frame, i.e. wider API surface than
   the one method proposed here.
2. **It would bypass RenderingDevice's graph/barriers.** RD is what inserts the barrier between
   the EASU storage write and the RCAS sampled read, tracks uniform-set usage
   (`rendering_device.cpp:7369-7380`), and orders buffer updates (`buffer_update` before
   `compute_list_begin`). A private dispatch path would re-implement that or deadlock on the
   WebGPU "usage scope" rules the driver already handles (`_compute_pass_usage_guard`,
   `rendering_device_driver_webgpu.cpp:9829-9935`).
3. **It would still reuse `_shader_build()`.** BGL/pipeline-layout creation from a descriptor is
   exactly the code at `rendering_device_driver_webgpu.cpp:5069-5600`; a private API either calls
   into it (so it needs a container anyway) or re-implements WebGPU layout rules.
4. Later upscaler milestones need uniform buffers, samplers, storage buffers and cross-pass
   texture transitions — all already validated by RD's `uniform_set_create`
   (`rendering_device.cpp:4799-5235`).

What the handoff actually wants ("keep it private") is preserved by **not** binding the new C++
method to ClassDB/GDScript (`_bind_methods` at `rendering_device.cpp:9753` onwards stays
untouched). The C++ surface grows by one struct pair + one method.

### 2.2 Expose the `RenderingDeviceDriver *` from RenderingDevice

`RenderingDevice::get_device_driver()` already exists (`rendering_device.h:1046`) but it is for
capability queries. Same problems as 2.1; plus driver handles have driver-specific lifetimes that
RD's deferred-free queues (`rendering_device.cpp:8142-8148,8553-8560`) manage. Rejected.

### 2.3 Hand-build a serialized container and call `shader_create_from_bytecode()`

Technically possible (a WGSL-only container round-trips: the per-stage WGSL serialization in
`rendering_shader_container_webgpu.cpp:121-169` is flag-gated and `from_bytes()` does not require
SPIR-V), but:

- renderer code cannot include `drivers/webgpu/rendering_shader_container_webgpu.h` (driver
  headers are below `servers/` and only compiled on the WebGPU configuration);
- the WebGPU factory would still be needed, so this adds a serialize/parse round trip for no
  benefit, and a non-WebGPU driver would reject the bytes with a confusing "Incorrect format in
  shader container" (`rendering_shader_container.cpp:826`) instead of a clear capability error.

Rejected for v1. Keep the byte serialization only as a future export-bake/test-fixture option —
it works for free, because `_shader_build()` detects a raw stage as "WGSL present + SPIR-V empty"
(§6), which is exactly what a serialized WGSL-only container contains.

### 2.4 Translate the WGSL to GLSL / wrap it as a Godot shader

Defeats the point (reintroduces Tint or glslang, changes the shader), and hand-written WGSL would
have to be valid Vulkan GLSL first. Rejected.

---

## 3. Exact API (implement these names/signatures)

### 3.1 `servers/rendering/rendering_device.h` / `.cpp` (layer 0005) — public RD API

Add in the public section next to the existing shader APIs
(`rendering_device.h:1058-1061`):

```cpp
	// One binding of a hand-written WGSL compute shader. `binding` is the WGSL
	// @binding number as written in the source (see docs/WGSL_COMPUTE_PATH.md —
	// the container applies the WebGPU binding-doubling convention internally).
	struct WGSLUniform {
		UniformType type = UNIFORM_TYPE_MAX;
		uint32_t binding = 0;
		// 0 = let the RD pick the type's natural default. Meaning per type:
		//   UNIFORM_TYPE_UNIFORM_BUFFER(_DYNAMIC): WGSL struct size in bytes
		//     (0 = don't validate the bound buffer size; strongly discouraged).
		//   UNIFORM_TYPE_STORAGE_BUFFER(_DYNAMIC): keep 0 so RD skips its exact-size
		//     check (a raw WGSL runtime-sized array has no compile-time byte size).
		//   SAMPLER/TEXTURE/IMAGE/INPUT_ATTACHMENT: array length; 0 → 1.
		//   UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: rejected for raw WGSL.
		uint32_t length = 0;
		TextureType texture_type = TEXTURE_TYPE_2D;
		DataFormat texture_format = DATA_FORMAT_MAX; // IMAGE: WGSL storage format; else unused.
		bool writable = false; // IMAGE: WGSL write/read_write; STORAGE_BUFFER: read_write.
	};

	struct WGSLShaderData {
		String name; // debug name; also the module/BGL/pipeline label
		String code; // WGSL source. Entry point MUST be named "main"; MUST be valid WebGPU WGSL.
		// Outer index = @group. Inner list = one entry per reflected binding
		// (sorted by RD). Groups must be contiguous and start at 0.
		Vector<Vector<WGSLUniform>> uniform_sets;
		// Must equal the @workgroup_size in the WGSL (used by
		// compute_list_dispatch_threads and by driver validation).
		uint32_t workgroup_size[3] = { 8, 8, 1 };
	};

	// Creates a compute shader from hand-written WGSL, without SPIR-V or Tint.
	// Only available when has_feature(SUPPORTS_RAW_WGSL_COMPUTE) (WebGPU). On any other
	// driver this fails and returns an invalid RID. Reflection validation and
	// binding normalization happen here, before the driver is called.
	RID shader_create_from_wgsl(const WGSLShaderData &p_data);
```

Add in the private section near `struct Shader`/`shader_owner` (`rendering_device.h:961-971`):

```cpp
	// Shared tail of shader_create_from_bytecode_with_samplers() and
	// shader_create_from_wgsl(): makes the RD RID, copies reflection, computes
	// set_formats and stage_bits.
	RID _shader_register(RDD::ShaderID p_driver_id, const ShaderReflection &p_reflection,
			const String &p_shader_name, RID p_placeholder);
```

`shader_create_from_wgsl()` body (§6.1): validate, normalize `WGSLUniform` → `ShaderUniform`
(sort each set by binding, reject duplicates/empty sets/combined types, force
`stages = SHADER_STAGE_COMPUTE_BIT`, default lengths), build `ShaderReflection`
(`pipeline_type = PIPELINE_TYPE_COMPUTE`, `compute_local_size`, `stages_vector = {COMPUTE}`,
`stages_bits.set_flag(SHADER_STAGE_COMPUTE_BIT)`, `push_constant_size = 0`, empty
`push_constant_stages`), then:

```cpp
	ERR_FAIL_COND_V_MSG(!driver->has_feature(SUPPORTS_RAW_WGSL_COMPUTE), RID(),
			"The active rendering driver does not support raw-WGSL compute shaders (WebGPU only).");
	RDD::ShaderID driver_id = driver->shader_create_from_wgsl(p_data.code, reflection, p_data.name);
	ERR_FAIL_COND_V(!driver_id, RID());
	return _shader_register(driver_id, reflection, p_data.name, RID());
```

`_shader_register()` is the code currently inlined at
`rendering_device.cpp:4628-4703` (`_THREAD_SAFE_METHOD_`, make RID or use placeholder, copy
reflection into `Shader`, name, `driver_id`, `driver->shader_get_layout_hash()`, set_formats loop,
stage_bits mapping, `set_resource_name` in DEV builds). Refactor
`shader_create_from_bytecode_with_samplers()` to call it:

```cpp
	RDD::ShaderID shader_id = driver->shader_create_from_container(shader_container, driver_immutable_samplers);
	ERR_FAIL_COND_V(!shader_id, RID());
	String shader_name;
	shader_name.append_utf8(shader_container->shader_name);
	return _shader_register(shader_id, shader_container->get_shader_reflection(), shader_name, p_placeholder);
```

(That replaces lines `4604-4704`; `shader_container->from_bytes()` and the immutable-sampler
translation stay above it.)

### 3.2 `servers/rendering/rendering_device_driver.h` / `.cpp` (layer 0005) — driver hook

Add next to `shader_create_from_container` (`rendering_device_driver.h:570`); **not pure** — every
existing driver must keep compiling and must fail cleanly:

```cpp
	// Creates a shader from hand-written WGSL (no SPIR-V / Tint). Drivers whose shader
	// language cannot represent WGSL keep the default implementation, which returns an
	// invalid ShaderID; RenderingDevice gates the call with SUPPORTS_RAW_WGSL_COMPUTE.
	virtual ShaderID shader_create_from_wgsl(const String &p_wgsl, const ShaderReflection &p_reflection,
			const String &p_shader_name);
```

Default in `rendering_device_driver.cpp` (next to the other non-pure defaults):

```cpp
RenderingDeviceDriver::ShaderID RenderingDeviceDriver::shader_create_from_wgsl(const String &p_wgsl,
		const ShaderReflection &p_reflection, const String &p_shader_name) {
	ERR_FAIL_V_MSG(ShaderID(), "This rendering driver does not support creating shaders from WGSL.");
}
```

`ShaderReflection`/`ShaderID` resolve unqualified because `RenderingDeviceDriver` derives from
`RenderingDeviceCommons` (`rendering_device_driver.h:90`). Vulkan/Metal/D3D12/GLES3 container files
are outside the patch stack and must not be touched — a non-pure virtual guarantees that.

### 3.3 `servers/rendering/rendering_device_commons.h` (layer 0005) — capability flag

Add at the end of `enum Features` (`rendering_device_commons.h:1023-1041`):

```cpp
		// Driver can create compute shaders directly from WGSL
		// (RenderingDevice::shader_create_from_wgsl). Only the WebGPU driver does.
		SUPPORTS_RAW_WGSL_COMPUTE,
```

`RenderingDevice::has_feature()` delegates to `driver->has_feature()` for non-special-cased flags
(`rendering_device.cpp:9734-9751`), and all four drivers' `has_feature()` switches have a
`default: return false`, so this compiles everywhere. Renderer code can then gate the upscaler on
`RD::get_singleton()->has_feature(RD::SUPPORTS_RAW_WGSL_COMPUTE)` without creating a shader to test. The name matches the parallel spatial-path spec
(`docs/PMNDRS_UPSCALER_SPATIAL.md` §9.2 item 5), which expects exactly this bit.

### 3.4 `drivers/webgpu/rendering_shader_container_webgpu.h` / `.cpp` (layer 0004) — container factory

Add in the public section (next to `get_stage_wgsl()`, `rendering_shader_container_webgpu.h:75-84`):

```cpp
	// Fills this container with a single WGSL compute stage + hand-built reflection.
	// Behavior:
	//  - copies p_reflection into the protected base-class reflection tables via
	//    set_from_shader_reflection() (compute-only, no push constants);
	//  - stores `p_wgsl` in stage_wgsl[0], setting HEADER_FLAG_HAS_WGSL;
	//  - rewrites every `@binding(N)` in the WGSL to `@binding(2*N)` (the WebGPU
	//    driver's binding-doubling convention, see docs/WGSL_COMPUTE_PATH.md §5);
	//  - writes shaders[0] = { SHADER_STAGE_COMPUTE, empty code }, and
	//    header_data.push_constant_bind_group = NO_PUSH_CONSTANTS.
	// Returns false on invalid input.
	bool set_code_from_wgsl_compute(const String &p_shader_name, const RDC::ShaderReflection &p_reflection,
			const String &p_wgsl);
```

It is a derived class, so it can use the base's protected `ReflectShader`, `ReflectUniform`,
`set_from_shader_reflection()`, `shader_name`, `shaders` and `header_data` directly — no changes to
`servers/rendering/rendering_shader_container.{h,cpp}` are needed (those files are **not in any
patch layer**, see §8). Implementation details in §6.2.

### 3.5 `drivers/webgpu/rendering_device_driver_webgpu.h` / `.cpp` (layer 0004) — override

Header next to `shader_create_from_container` (`rendering_device_driver_webgpu.h:536`):

```cpp
	virtual ShaderID shader_create_from_wgsl(const String &p_wgsl, const ShaderReflection &p_reflection,
			const String &p_shader_name) override final;
```

Implementation (the .cpp already includes `rendering_shader_container_webgpu.h` at line 35):

```cpp
RDD::ShaderID RenderingDeviceDriverWebGPU::shader_create_from_wgsl(const String &p_wgsl,
		const ShaderReflection &p_reflection, const String &p_shader_name) {
	Ref<RenderingShaderContainerWebGPU> container;
	container.instantiate();
	if (!container->set_code_from_wgsl_compute(p_shader_name, p_reflection, p_wgsl)) {
		return ShaderID();
	}
	Vector<ImmutableSampler> immutable_samplers; // Raw WGSL cannot use immutable samplers.
	return shader_create_from_container(container, immutable_samplers);
}
```

Also add to `RenderingDeviceDriverWebGPU::has_feature()` (`rendering_device_driver_webgpu.cpp:10518`):

```cpp
		case SUPPORTS_RAW_WGSL_COMPUTE:
			return true;
```

### 3.6 Behavior matrix

| driver | `has_feature(SUPPORTS_RAW_WGSL_COMPUTE)` | `shader_create_from_wgsl()` |
|---|---|---|
| WebGPU (web template) | true | WebGPU container → deferred `WGShader`, works with uniform sets + compute lists |
| Vulkan (native) | false (driver `has_feature` default) | default virtual → `ERR_FAIL_V_MSG("This rendering driver does not support creating shaders from WGSL.")`, invalid ID → RD returns `RID()` |
| Metal / D3D12 / GLES3 | false | same default |
| any future WGSL driver | true | implement the virtual |

RD-level validation (empty WGSL, invalid type, duplicate binding, combined sampler, >16 sets,
zero workgroup size, empty inner set) runs **before** the driver/container is called, so a bad
call never reaches WebGPU/Dawn and the error names the offending set/binding.

Alignment with the parallel effect spec (`docs/PMNDRS_UPSCALER_SPATIAL.md`): its §9.3 reflection table
lists the **authored** WGSL bindings (EASU 0,1,2; RCAS 0,1,2,3,4) as the RD binding numbers, which is
exactly what `WGSLShaderData.uniform_sets` expects here; its §9.4 `RD::Uniform(type, binding, rid)`
calls use the same numbers; its §9.2 item 5 capability bit is `SUPPORTS_RAW_WGSL_COMPUTE` as above.

---

## 4. How the rest of the WebGPU path consumes this unchanged

Verified behaviour that the raw-WGSL container inherits for free:

1. **BGL creation** from reflection: one `WGPUBindGroupLayoutEntry` per `ShaderUniform`
   (`rendering_device_driver_webgpu.cpp:5069-5470`). Type mapping relevant here:
   - `UNIFORM_TYPE_UNIFORM_BUFFER` → `buffer.type = Uniform`, `minBindingSize = 0` (`:5259-5266`).
   - `UNIFORM_TYPE_STORAGE_BUFFER` → buffer type picked from the WGSL scan:
     `var<storage, read_write>` → `Storage`, `var<storage, read>`/`var<storage>` →
     `ReadOnlyStorage` (`:4839-4888`, `:5270-5300`). `minBindingSize` stays 0 (set explicitly for UBOs at
     `:5263`, zero-initialized for storage buffers by `entry = {}` at `:5269`).
   - `UNIFORM_TYPE_TEXTURE` → `texture.sampleType` from the WGSL component scan
     (`texture_2d<f32>` → `Float`, `u32` → `Uint`, `i32` → `Sint`, MSAA → `UnfilterableFloat`,
     depth → `Depth`, `:5164-5185`), `viewDimension` from the WGSL type (`:4638-4837`).
   - `UNIFORM_TYPE_IMAGE` → `storageTexture.format` and `.access` from the WGSL declaration scan
     (`texture_storage_2d<rgba16float, write>` → `RGBA16Float` / `WriteOnly`, `:4890-4973`,
     `:5232-5258`); `writable` is only the fallback.
   - `UNIFORM_TYPE_SAMPLER` → `sampler.type` from the WGSL scan: default `Filtering`,
     `Comparison` for `sampler_comparison`, `NonFiltering` only for depth-paired static samples
     (`:5102-5116`, `:5141-5160`).
   - Per-binding stage visibility comes from a per-stage WGSL scan
     (`wgsl_binding_stages`, `:4839-4888`, consumed at `:5077-5090`): for a compute-only shader
     every binding is `Compute`. If a reflected binding is *not* declared in the WGSL its
     visibility is 0 and the bind group silently fails to match → reflection and WGSL must agree
     exactly.
2. **Pipeline layout**: `total_groups = set_count` when the container has no push constants
   (`:5478-5600`); the push-constant ring group is only added when
   `has_push_constants()` (`:5478-5479`). Keep `NO_PUSH_CONSTANTS`.
3. **Uniform sets / bind groups**: `uniform_set_create()` builds a `WGPUBindGroup` from the
   reflection `bind_group_infos`; buffer entries use `binding*2`, image entries use
   `tex->default_view` (`:5743-6120`, image at `:6018-6038`).
4. **Compute lists**: `compute_list_begin()` / `bind_compute_pipeline` / `bind_uniform_set` /
   `dispatch` / `compute_list_end` (`rendering_device.h:1704-1713`,
   `rendering_device.cpp:7124-7200,7270-7380,7554`) map to the driver's compute pass commands;
   `command_bind_compute_pipeline` lazily opens a `WGPUComputePass` and pre-binds empty groups for
   pipeline-layout gaps (`rendering_device_driver_webgpu.cpp:9648-9700`); dispatch calls
   `_compute_pass_usage_guard()` which ends/reopens the pass when a texture written earlier in the
   pass is now read (or written twice) (`:9829-9935`). `compute_list_add_barrier()`
   (`rendering_device.cpp:7532-7551`) is the deterministic version of the same split.
5. **Free path**: RIDs and dependent RIDs are handled by RD
   (`rendering_device.cpp:8142-8148` schedules the driver `shader_free` two frames later at
   `:8553-8560`; uniform sets depend on the shader and on every bound resource,
   `rendering_device.cpp:5244-5253`).

---

## 5. Binding convention (must be documented in the header comment and followed by the WGSL author)

`spirv_preprocess` doubles **every** non-push-constant binding before Tint
(`drivers/webgpu/spirv_preprocess.cpp:1338-1346`, combined sampler at `binding*2`,
texture at `binding*2+1`, `:1243,1328`) because the driver's BGL/bind-group code uses
`u.binding * 2` (`rendering_device_driver_webgpu.cpp:5149,5164,5189-5201,5232,5259,5270,5303,5314,
5347,5366,5390,5505-5511,5788,5906-5947,6029-6116`).

Raw WGSL does not go through the preprocessor, so the container helper rewrites the WGSL text
itself: **`@binding(N)` → `@binding(2*N)` for every declaration**, done as a pure scalar rewrite in
`RenderingShaderContainerWebGPU::set_code_from_wgsl_compute()` (scan for `"@binding("`, parse the
decimal digits, emit `2*N`, keep any `u` suffix and the closing paren). The *reflection* keeps the
authored binding number as the Godot binding (that is what `RD::Uniform.binding` must be).

Mapping for the spatial upscaler (bindings as authored by pmndrs; EASU/RCAS declare no samplers —
they use `textureLoad`, verified in `packages/core/src/shaders/easu.ts:42-48,111`,
`rcas.ts:170-181`):

| WGSL as authored | after helper | RD reflection binding | RD type |
|---|---|---|---|
| `@group(0) @binding(0) var<uniform> C : FsrConstants;` | `@binding(0)` | 0 | `UNIFORM_TYPE_UNIFORM_BUFFER` |
| EASU `@binding(1) var inputColor : texture_2d<f32>;` | `@binding(2)` | 1 | `UNIFORM_TYPE_TEXTURE` |
| EASU `@binding(2) var outputColor : texture_storage_2d<rgba16float, write>;` | `@binding(4)` | 2 | `UNIFORM_TYPE_IMAGE` |
| RCAS `@binding(2) var exposureTex : texture_2d<f32>;` | `@binding(4)` | 2 | `UNIFORM_TYPE_TEXTURE` |
| RCAS `@binding(3) var outputColor : texture_storage_2d<rgba16float, write>;` | `@binding(6)` | 3 | `UNIFORM_TYPE_IMAGE` |
| RCAS `@binding(4) var alphaSource : texture_2d<f32>;` | `@binding(8)` | 4 | `UNIFORM_TYPE_TEXTURE` |

Hard rules for v1 (validated by `shader_create_from_wgsl`):

- exactly one `@group(0)` set (or contiguous groups 0..N-1, one RD uniform set each, none empty);
- no push constants (use the binding-0 UBO; `push_constant_size = 0`,
  `push_constant_stages` empty — RD's debug checks at `rendering_device.cpp:7292-7296` then stay
  silent);
- no `UNIFORM_TYPE_SAMPLER_WITH_TEXTURE` (declare a separate `sampler` + `texture` binding
  instead; combined bindings only exist because of the SPIR-V split convention);
- no binding arrays, no `binding_array<T,N>` (the driver flattens Tint output only);
- no read-only / read_write storage textures (see §7 gotcha 8);
- compute only; entry point `main`; `@workgroup_size` must equal `workgroup_size`;
- no `override`/specialization constants (not needed; the driver would look for `@id()` in the
  WGSL text, `:4975-5062`, and use the SPIR-V specialization path only when SPIR-V exists).

---

## 6. Reflection construction recipe (what to fill, and the exact values)

### 6.1 RD side (`shader_create_from_wgsl`) — validation and normalization

For each set `s` and each `WGSLUniform u`:

1. `u.type != UNIFORM_TYPE_MAX` and `u.type != UNIFORM_TYPE_SAMPLER_WITH_TEXTURE`, else error.
2. No duplicate (set, binding); RD sorts `dst.sort()` then rejects equal neighbours.
3. `su.stages = SHADER_STAGE_COMPUTE_BIT` (bitfield value `1 << 4`; do **not** pass
   `SHADER_STAGE_COMPUTE`, whose value 4 is the wrong bit — see how `reflect_spirv` builds it at
   `rendering_shader_container.cpp:301-317` and how the driver decodes it at
   `rendering_device_driver_webgpu.cpp:3938-3950`).
4. `length` defaults: `SAMPLER`, `TEXTURE`, `IMAGE`, `INPUT_ATTACHMENT` → `u.length ? u.length : 1`;
   everything else as given.
5. `su.texture_type = u.texture_type` (RD checks `texture->type` for TEXTURE/IMAGE at
   `rendering_device.cpp:4991-4998`), `su.texture_format = u.texture_format`
   (IMAGE format mismatch is only a verbose warning at `:5000-5003`, but the driver/WebGPU require
   the BGL storage format to match the bound texture exactly — set it correctly).
6. `su.writable = u.writable` (drives RD's read-write resource tracking for IMAGE at
   `:5006-5015` and storage buffers at `:5135-5155`).

### 6.2 Container side (`set_code_from_wgsl_compute`) — copy + WGSL storage

```cpp
	using RDC = RenderingDeviceCommons;
	ERR_FAIL_COND_V_MSG(p_reflection.pipeline_type != RDC::PIPELINE_TYPE_COMPUTE, false,
			"Raw WGSL containers only support compute shaders.");
	ERR_FAIL_COND_V_MSG(p_reflection.push_constant_size != 0, false,
			"Raw WGSL shaders cannot use push constants; use a uniform buffer.");
	ERR_FAIL_COND_V(p_wgsl.strip_edges().is_empty(), false);

	ReflectShader reflection;
	reflection.pipeline_type = RDC::PIPELINE_TYPE_COMPUTE;
	memcpy(reflection.compute_local_size, p_reflection.compute_local_size, sizeof(reflection.compute_local_size));
	reflection.stages_bits.set_flag(RDC::SHADER_STAGE_COMPUTE_BIT); // bit 4 == 1u << SHADER_STAGE_COMPUTE
	reflection.uniform_sets.resize(p_reflection.uniform_sets.size());
	for (uint32_t s = 0; s < p_reflection.uniform_sets.size(); s++) {
		for (const RDC::ShaderUniform &su : p_reflection.uniform_sets[s]) {
			ReflectUniform ru;
			ru.type = su.type; ru.binding = su.binding; ru.length = su.length;
			ru.writable = su.writable; ru.texture_type = su.texture_type;
			ru.texture_format = su.texture_format;
			ru.stages.set_flag(RDC::SHADER_STAGE_COMPUTE_BIT);
			reflection.uniform_sets[s].push_back(ru); // LocalVector push_back, as reflect_spirv does
		}
	}

	shader_name = p_shader_name.utf8(); // must be set before set_from_shader_reflection()
	set_from_shader_reflection(reflection); // fills reflection_data + binding tables

	shaders.resize(1);
	shaders.write[0].shader_stage = RDC::SHADER_STAGE_COMPUTE;
	// code_compressed_bytes stays empty, code_compression_flags = 0, code_decompressed_size = 0
	stage_wgsl.resize(1);
	stage_wgsl[0] = _double_wgsl_bindings(p_wgsl).utf8();
	header_data.flags |= HEADER_FLAG_HAS_WGSL;
	header_data.push_constant_bind_group = NO_PUSH_CONSTANTS;
	return true;
```

Notes:
- `set_from_shader_reflection()` is protected in the base (`rendering_shader_container.h:279`) and
  callable from the derived class; it copies `uniform_sets`, `stages_bits`, `compute_local_size`,
  `pipeline_type`, `push_constant_size` into the serializable tables and calls
  `_set_from_shader_reflection_post()` (no-op for WebGPU). It must run *after* `shader_name` is set
  (`shader_name_len`, `rendering_shader_container.cpp:702`).
- `stage_wgsl` (not `stage_wgsl.write[i]`; `_set_code_from_spirv` uses `stage_wgsl[i] = ...`,
  `rendering_shader_container_webgpu.cpp:91`).
- Do **not** call `shaders.write[0].code_compressed_bytes = ...`; empty code is the raw-WGSL marker
  (§6.3). `shaders.size()` must stay 1 (the driver iterates it, `:4080`).
- `header_data.flags` is only required for serialization; set it anyway so a `to_bytes()` of this
  container is meaningful.

### 6.3 Driver side — two changes in `_shader_build()` (layer 0004)

**(a) Relax the SPIR-V requirement** (`rendering_device_driver_webgpu.cpp:4076-4110`). Current
code rejects empty SPIR-V at `:4086`. Replace the top of the loop with:

```cpp
		const PackedByteArray &spv_bytes = s.code_compressed_bytes;
		const CharString baked_wgsl = Ref<RenderingShaderContainerWebGPU>(p_shader_container)->get_stage_wgsl(i);
		const bool raw_wgsl = !baked_wgsl.is_empty() && spv_bytes.is_empty();
		if (!raw_wgsl) {
			if (spv_bytes.is_empty()) { error_text = "WebGPU: empty SPIR-V for shader stage."; break; }
			if (spv_bytes.size() % 4 != 0) { error_text = "WebGPU: SPIR-V size must be a multiple of 4."; break; }
			shader->stage_spirv[(int)s.shader_stage] = spv_bytes;
		} else {
			print_verbose(vformat("WebGPU: raw WGSL for '%s' stage %d, Tint bypassed.", shader->name, (int)s.shader_stage));
		}
		char *wgsl_str = !baked_wgsl.is_empty() ? _malloc_wgsl(baked_wgsl)
				: _spv_to_wgsl_cached(spv_bytes.ptr(), (int)spv_bytes.size());
		if (baked_wgsl.length() > 0) { _spv_to_wgsl_baked_hits++; }
```

(Delete the old `baked_wgsl`/`wgsl_str` declarations at `:4103-4104`; the existing
`get_stage_wgsl()` consumption and the `_spv_to_wgsl_baked_hits` counter move up. Because
`stage_spirv` stays empty, `compute_pipeline_create()`'s specialization-constant branch is a no-op,
`:10009-10014`.)

**(b) Skip the Tint-output-specific WGSL rewrites for raw stages.** Wrap the whole region
`rendering_device_driver_webgpu.cpp:4117-4622` (from "Remap unsupported 8-bit storage texture
format names in WGSL" through the end of the read-only-storage-texture conversion block, i.e. just
before `WGPUShaderSourceWGSL wgsl_source = {};` at `:4623`) in:

```cpp
		if (!raw_wgsl) {
			... existing rewrite blocks unchanged ...
		}
```

These blocks assume Tint output (r8/rg8/r16/rg16/`rg11b10ufloat`/`rgba16snorm` format-name
rewrites, vertex `read_write` demotion, `binding_array` flattening, read_write/read storage
texture splits) and can silently corrupt hand-written WGSL. The *scans* after module creation
(`:4638-5041`) must keep running for raw stages — they are the source of BGL view dimension,
sample type, storage format/access, storage-buffer read-only flag and per-stage visibility.
For the EASU/RCAS shaders these rewrites are all no-ops today, so (b) is defensive; do it anyway so
future hand-written WGSL is never text-mangled.

Add a one-line diagnostic in the raw branch (temporary for the milestone; or gate behind
`WEBGPU_VERBOSE` at `:55-64`): a raw stage must never call `_spv_to_wgsl_cached()`
(counters `_spv_to_wgsl_cache_misses` at `:328-375`, baked hits at `:283,4106`).

### 6.4 Concrete recipe for the spatial upscaler

Source check for the bindings below: `pmndrs/upscaler` `main` at commit
`f6e52098edffb66e16298a30c53b4da37b4b7d7f` (fetched 2026-11-27), files
`packages/core/src/shaders/easu.ts` (bindings 1=sampled input, 2=storage output,
`@compute @workgroup_size(8, 8)`, `fn main`, `textureLoad` taps — no sampler),
`rcas.ts` (bindings 1=input, 2=exposure, 3=storage output, 4=alpha source, same
workgroup/entry-point shape, `textureLoad` + `textureStore`) and
`common.ts` (`FsrConstants`, `@group(0) @binding(0)`, 96-byte payload). Keep the
AMD FSR copyright/notice header when vendoring (MIT + AMD notice, see the
handoff). The WGSL is embedded verbatim apart from the mechanical
`@binding(N)` rewrite the container applies; do not hand-edit bindings.

**Shared constants UBO** (`packages/core/src/shaders/common.ts`, `FsrConstants` = 96-byte payload,
per the handoff plan a 256-byte buffer):

```cpp
	RD::WGSLUniform ubo;
	ubo.type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
	ubo.binding = 0;
	ubo.length = 96;          // WGSL struct size: offsets 0..92, align 8, size 96
	// texture_type/texture_format/writable unused for UBO
```

Create the buffer with `uniform_buffer_create(256, {})`, fill 96 bytes per frame with
`buffer_update(buf, 0, 96, &constants)` **before** `compute_list_begin()` (see the
`buffer_update` contract, `rendering_device.h:266-280`).

**Sampled input texture, `texture_2d<f32>`** (EASU `inputColor`, RCAS `inputColor`/`exposureTex`/
`alphaSource`):

```cpp
	RD::WGSLUniform t;
	t.type = RD::UNIFORM_TYPE_TEXTURE;
	t.binding = <authored binding>;      // 1 for EASU/RCAS inputColor
	t.length = 1;
	t.texture_type = RD::TEXTURE_TYPE_2D;
	t.texture_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT; // informational; driver only warns
```

The texture must be created with `TEXTURE_USAGE_SAMPLING_BIT` (`uniform_set_create` enforces this,
`rendering_device.cpp:4956-4959`) and be a **filterable format whose WGSL component matches
`f32`**. RGBA16F is filterable in core WebGPU; the BGL sample type will be `Float`
(`rendering_device_driver_webgpu.cpp:5164-5185`). Avoid `r32float` sources
(see gotcha 9). Use the device's default linear sampler only if a sampler is declared — EASU/RCAS
use `textureLoad`, so no sampler binding at all.

**Storage output, `texture_storage_2d<rgba16float, write>`** (EASU/RCAS `outputColor`):

```cpp
	RD::WGSLUniform img;
	img.type = RD::UNIFORM_TYPE_IMAGE;
	img.binding = <authored binding>;    // 2 for EASU, 3 for RCAS
	img.length = 1;
	img.writable = true;                 // WGSL access is `write`; RD tracks it read-write
	img.texture_type = RD::TEXTURE_TYPE_2D;
	img.texture_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
```

Texture requirements: `format = RGBA16F`; usage `STORAGE_BIT | SAMPLING_BIT`
(+`CAN_COPY_FROM_BIT` if the final blit needs it); `TEXTURE_TYPE_2D`; `mipmaps = 1`;
`array_layers = 1`; `samples = 1`. Rationale:
- `uniform_set_create` requires `TEXTURE_USAGE_STORAGE_BIT`, type 2D and (verbose) format match
  (`rendering_device.cpp:4991-5005`);
- the driver binds the **default view** for the image (`rendering_device_driver_webgpu.cpp:6022-6026`)
  and the default view is created with `mipLevelCount = mipmaps`
  (`:1979-1985`), while a storage-texture BGL entry requires a single-mip view → 1 mip;
- the driver derives `access = WriteOnly` and `format = RGBA16Float` from the WGSL text
  (`:4890-4973`, used at `:5232-5258`);
- RGBA16F is a core WebGPU storage format and `_promote_storage_format()` leaves it unchanged
  (`:3007-3068`), unlike r8/rg8/r16/rg16/rg11b10 which are promoted.

`RD::texture_get_usages_supported_by_format()` reports STORAGE for RGBA16F (list at
`rendering_device_driver_webgpu.cpp:2840-2902`, storage bit set at `:2902`), so the RD-side check
in `texture_create()`/renderer passes.

**Sampler** (not needed by EASU/RCAS; for future passes):

```cpp
	RD::WGSLUniform s;
	s.type = RD::UNIFORM_TYPE_SAMPLER; s.binding = <authored>; s.length = 1;
	// texture_type/format unused. WGSL `var s: sampler` → BGL Filtering by default.
```

For a **non-filtering** sampler (needed when sampling `r32float`/depth with a sampler) the driver
only emits `NonFiltering` from its depth-pairing call-site scan
(`rendering_device_driver_webgpu.cpp:4778-4829,5102-5116`) — a hand-written `var s: sampler` used
with `textureSample` on an r32float texture will get `Filtering` and fail pipeline creation. v1
avoids this entirely (use RGBA16F sampled inputs + `textureLoad`); the extension is a §10 risk.

**Storage buffer** (future temporal passes; `var<storage, read_write> buf: array<atomic<u32>>`):

```cpp
	RD::WGSLUniform sb;
	sb.type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
	sb.binding = <authored>;
	sb.length = 0;      // do not set the byte size: RD then skips the exact-size check
	sb.writable = true; // false for `var<storage, read>`; driver also scans the WGSL text
```

The buffer must be created with `STORAGE_BIT` (`storage_buffer_create`). Keep `length = 0`: the RD
check at `rendering_device.cpp:5145-5148` demands exact equality when non-zero. Driver BGL type is
`Storage`/`ReadOnlyStorage` from the WGSL scan (`:5270-5300`), with vertex visibility stripped for
read_write; compute-only shaders are unaffected.

---

## 7. Gotchas (each one has bitten or would bite this path)

1. **Binding doubling** — see §5. Reflection binding `N` ↔ WGSL `@binding(2*N)`. Do not hand-write
   doubled bindings: the container doubles them, so you'd get 4N. Mismatches are reported by Dawn as
   `CreateBindGroup`/pipeline-layout validation errors, not as compile errors in our code.
2. **Reflection must exactly match the WGSL declarations.** The BGL is built from the reflection for
   types, and from the WGSL text scan for view dimension/sample type/storage format/access/
   visibility (`wgsl_binding_stages` at `rendering_device_driver_webgpu.cpp:4839-4888`,
   `resolve_stage_visibility` `:5077-5090`). A reflected binding that is absent from the WGSL gets
   visibility 0 → `wgpuDeviceCreateBindGroup` fails. A WGSL declaration absent from the reflection
   gets no BGL entry → pipeline creation fails. Keep the WGSL declarations in the exact Tint shape
   the scanners expect: `@group(0) @binding(N) var[<...>] name : type;` on one line or with the
   type after a normal `:` and the statement terminated by `;` (the scanners use
   `parse_group_binding()` + `strchr(':')` + `strchr(';')`, `:161-200,4638-5062`). Do not put a
   comment containing `;` or `texture_` between `@binding(...)` and the declaration.
3. **`UNIFORM_TYPE_SAMPLER_WITH_TEXTURE` is forbidden** for raw WGSL: the driver expands it to
   `binding*2`/`binding*2+1` and stores only the texture half in `bind_group_infos`
   (`:5189-5225`); the raw path has no SPIR-V split to match it. Use separate SAMPLER and TEXTURE
   entries.
4. **Push constants: none.** Container must set `header_data.push_constant_bind_group =
   NO_PUSH_CONSTANTS` (`rendering_shader_container_webgpu.h:44,80`); reflection
   `push_constant_size = 0`. Otherwise the driver's pipeline layout expects a binding-120 ring entry
   and merges the PC group (`rendering_device_driver_webgpu.cpp:5478-5570`), and RD's debug
   validation requires `compute_list_set_push_constant` (`rendering_device.cpp:7276-7296`). Use the
   binding-0 UBO for all parameters. Constants must be uploaded with `buffer_update()` *before*
   `compute_list_begin()`.
5. **`minBindingSize` is 0 everywhere** in the WebGPU BGL (`:5263`; storage-buffer entries are
   zero-initialized by `entry = {}` at `:5269`). The *shader struct* is the
   effective minimum: the bound UBO must be ≥ the WGSL struct size (a 256-byte buffer for the
   96-byte `FsrConstants` is fine; a 96-byte buffer for a 256-byte struct fails at dispatch).
6. **Pipeline-layout group count = number of reflected uniform sets.** WGSL may not reference a
   `@group` that has no reflected set; empty RD sets are rejected (`rendering_device.cpp:4807`)
   and WebGPU requires every group of the pipeline layout to be bound (the driver pre-binds empty
   layouts only for *gap* slots, `:5590-5605`). Start with a single `@group(0)`.
7. **Uniform-set validation** (`rendering_device.cpp:4799-5235`): every reflected binding must be
   supplied exactly once by binding number; `RD::Uniform.uniform_type` must equal the reflected type
   (`:4846`); TEXTURE needs `TEXTURE_USAGE_SAMPLING_BIT` (`:4956`), IMAGE needs
   `TEXTURE_USAGE_STORAGE_BIT` (`:5004`) and matching texture type (`:4997`); UBO size must be
   `>= length` (`:5112`); SSBO size must be exactly `length` if non-zero (`:5147`). Reuse
   `UniformSetCacheRD`/create sets once per resource lifetime, not per frame.
8. **Storage texture access**: `write` (write-only) is core WebGPU for `rgba16float` — no feature
   needed, works in Chrome/Firefox/Safari. `read`/`read_write` access requires the
   `readonly_and_readwrite_storage_textures` WGSL language feature; the driver detects it
   (`has_rw_storage_textures`, `:1017-1026`) and, for Tint shaders, splits/converts
   (`:4311-4622`). The raw path skips those transforms, so **v1 raw WGSL must not use read_only /
   read_write storage textures**: read through a sampled `texture_2d<f32>` + `textureLoad` instead
   (this is exactly what pmndrs EASU/RCAS do).
9. **Filterability / sample type**: BGL sample type is derived from the WGSL component only
   (`texture_2d<f32>` → `Float`, `:5164-5185`), never from the texture format. Binding an
   unfilterable format (r32float, rg32float, rgba32float without `float32-filterable`) to a `Float`
   binding fails `CreateBindGroup`; the driver only substitutes blank fallbacks in its own
   fixup paths (`:5845-5865`). The app requests `float32-filterable` when available
   (`platform/web/js/engine/engine.js:351`), but do not depend on it for the spatial path: use
   RGBA16F. For the temporal path's r32float depth, plan an extension (driver-level
   `UnfilterableFloat` decision driven by `WGSLUniform.texture_format`, or a NonFiltering sampler
   hint) — §10.
10. **Storage-texture view constraints**: output texture must be 1 mip / 1 layer / 1 sample
    (default view mip count = texture mips, `rendering_device_driver_webgpu.cpp:1979-1985`; storage
    views require a single mip). `texture_type` must be 2D.
11. **Compute-pass splitting / usage scope**: writing the EASU output texture and then reading it as
    a sampled texture in the RCAS dispatch in the same compute list is handled automatically by the
    driver's usage guard (`:9829-9935`) or deterministically by
    `RD::compute_list_add_barrier()` (`rendering_device.cpp:7532-7551`). Use the barrier between
    EASU and RCAS and between RCAS and whatever samples its output. Do not expect a manual
    `barrier()`: it is a no-op deprecated shim (`rendering_device.cpp:7565-7572`).
12. **BGL-compatible rebind**: RD debug builds hard-fail when a pipeline binds a uniform set whose
    format hash differs (`rendering_device.cpp:7300-7325`); set format hashes come from the
    reflection (`:4648-4668`). EASU and RCAS have different binding sets, so create **one uniform
    set per shader** (or reuse only when the reflection is identical). The driver's cross-shader
    bind-group adaptation (`:6280-6520`) is a fallback for the raster path; don't rely on it for
    compute.
13. **Freeing order**: free `ComputePipeline` RIDs before/after shaders freely — RD frees dependent
    RIDs (`rendering_device.cpp:225-251`) and defers driver destruction to a later frame
    (`:8142-8148,8553-8560`). Keep textures alive as long as the uniform sets are alive (the sets
    depend on their resources, `:5244-5253`, so freeing a texture invalidates sets); recreate
    uniform sets after render-target resize. Output RGBA16F textures are render-graph resources and
    should be owned by `RenderSceneBuffersRD` (like the FSR1 intermediate in
    `servers/rendering/renderer_rd/effects/fsr.cpp:77-85`).
14. **Deferred build**: a wrong WGSL or reflection surfaces at the first
    `compute_pipeline_create()`/`uniform_set_create()`, not at `shader_create_from_wgsl()`
    (`rendering_device_driver_webgpu.cpp:3951-3972`). Create the pipeline immediately after the
    shader and check `pipeline.is_valid()`; log `WGSLShaderData.name` (it becomes the module/BGL/
    pipeline label, `:4631`).
15. **Spec constants are not available** for raw WGSL (no `@id()`/SPIR-V patch path,
    `:9983-10014`). Bake constants into the WGSL string or use the UBO.
16. **Dispatch group counts > 0** — RD debug builds reject zero (`rendering_device.cpp:7276-7279`);
    clamp `ceil(display/(8,8))` to ≥ 1 for tiny render targets.

---

## 8. Per-file step list

All files below are already in the patch layers; **no `patches/layers/*.files` change is needed**
(verified: `servers/rendering/rendering_device{,_driver,_commons}.h/.cpp` are in
`0005-engine-renderer-patches.files:111-116`; `drivers/webgpu/rendering_device_driver_webgpu.*` and
`drivers/webgpu/rendering_shader_container_webgpu.*` are in `0004-webgpu-driver.files:9-12`).
`servers/rendering/rendering_shader_container.{h,cpp}` are **not** patched by this stack and are not
touched by this design.

### 8.1 Layer 0004 — `drivers/webgpu` (implementer 1)

| # | file | change |
|---|---|---|
| 1 | `rendering_shader_container_webgpu.h` | declare public `bool set_code_from_wgsl_compute(const String &p_shader_name, const RDC::ShaderReflection &p_reflection, const String &p_wgsl);` (doc comment from §3.4) |
| 2 | `rendering_shader_container_webgpu.cpp` | implement it per §6.2 + a file-static `static String _double_wgsl_bindings(const String &)` per §5 (`@binding(N)` → `@binding(2*N)`, preserve any `u` suffix; ~25 lines) |
| 3 | `rendering_device_driver_webgpu.h` | declare `virtual ShaderID shader_create_from_wgsl(...) override final;` in the SHADER section (`:536`) |
| 4 | `rendering_device_driver_webgpu.cpp` | implement the override (§3.5); `has_feature()` case (`:10518`); `_shader_build()` changes (§6.3: raw stage accepted, Tint rewrites gated, `print_verbose`); optional `WEBGPU_VERBOSE` diag |

### 8.2 Layer 0005 — engine (implementer 2)

| # | file | change |
|---|---|---|
| 5 | `servers/rendering/rendering_device_commons.h` | add `SUPPORTS_RAW_WGSL_COMPUTE` to `enum Features` (`:1023-1041`) |
| 6 | `servers/rendering/rendering_device_driver.h` | declare non-pure `virtual ShaderID shader_create_from_wgsl(const String &p_wgsl, const ShaderReflection &p_reflection, const String &p_shader_name);` next to `shader_create_from_container` (`:570`) |
| 7 | `servers/rendering/rendering_device_driver.cpp` | default implementation returning invalid `ShaderID()` with `ERR_FAIL_V_MSG` (§3.2) |
| 8 | `servers/rendering/rendering_device.h` | `WGSLUniform`, `WGSLShaderData`, `RID shader_create_from_wgsl(const WGSLShaderData &)` near `:1058`; private `RID _shader_register(...)` declaration near `:961` |
| 9 | `servers/rendering/rendering_device.cpp` | refactor `shader_create_from_bytecode_with_samplers()` (`:4604-4704`) into `_shader_register()` + call; implement `shader_create_from_wgsl()` per §3.1/§6.1 |

Ordering: items 5–9 do not depend on 1–4 compiling alone (the engine layer is only built on top of
the fully applied stack, and the WebGPU override sits on top of a virtual declared in 0005 — the
same pattern as every existing driver override). Build once after both implementers are done.

### 8.3 Sketch (~20 lines) of the effect using the API

Placement mirror: `servers/rendering/renderer_rd/effects/fsr.cpp::FSR::process()` (`:64-146`) is the
exact template (compute_list_begin → bind pipeline/sets → dispatch → `compute_list_add_barrier` →
dispatch → end).

```cpp
// One-time, after RD is up and has_feature(SUPPORTS_RAW_WGSL_COMPUTE):
RD::WGSLShaderData easu;
easu.name = "PMNDRS_EASU";
easu.code = easu_wgsl;                       // vendored WGSL: @group(0) @binding(0 UBO,1 tex,2 storage), fn main, @workgroup_size(8,8)
easu.workgroup_size[0] = 8; easu.workgroup_size[1] = 8; easu.workgroup_size[2] = 1;
easu.uniform_sets = { { ubo /*binding 0, length 96*/,
                        sampled /*binding 1, RGBA16F, 2D*/,
                        storage /*binding 2, RGBA16F, 2D, writable*/ } };
RID easu_shader = RD::get_singleton()->shader_create_from_wgsl(easu);
RID easu_pipeline = RD::get_singleton()->compute_pipeline_create(easu_shader);
// ... same for "PMNDRS_RCAS" with 5 bindings (UBO, in, exposure, out storage, alpha)
// then per frame, once textures are allocated:
RD::Uniform u0(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, constants_buf);
RD::Uniform u1(RD::UNIFORM_TYPE_TEXTURE, 1, source_color);
RD::Uniform u2(RD::UNIFORM_TYPE_IMAGE, 2, easu_target);
RID easu_set = RD::get_singleton()->uniform_set_create({ u0, u1, u2 }, easu_shader, 0);

RD::get_singleton()->buffer_update(constants_buf, 0, 96, &fsr_constants); // before compute_list_begin
RD::ComputeListID cl = RD::get_singleton()->compute_list_begin();
RD::get_singleton()->compute_list_bind_compute_pipeline(cl, easu_pipeline);
RD::get_singleton()->compute_list_bind_uniform_set(cl, easu_set, 0);
RD::get_singleton()->compute_list_dispatch(cl, (display_size.x + 7) / 8, (display_size.y + 7) / 8, 1);
RD::get_singleton()->compute_list_add_barrier(cl);   // new usage scope: EASU write -> RCAS read
// ... bind rcas_pipeline + rcas_set (samples easu_target), dispatch, then blit easu/rcas output
RD::get_singleton()->compute_list_end();
```

Shader delivery note: the WGSL must be embedded in the binary (renderer code has no `res://`
dependency); options are a checked-in generated header with C++ raw-string literals (simplest,
recommended for milestone 1) or a new SCsub `.wgsl` → `.gen.h` builder. This is integration work,
not part of the API above.

---

## 9. Compile check (web template)

Toolchain: `~/fast_godot_toolchain` (Python 3.12.16, SCons 4.11.1, Emscripten 6.0.12; verified
`emcc --version` prints 6.0.12 with the commands below). `/tmp/godot48` already has a populated
build state: `6296` objects / 1.3 GB in `bin/obj`, the last web template in `bin/` (wasm 38 MB,
2026-11-26 20:13), and a shared SCons cache (`~/fast_godot_toolchain/scons-cache`, 476 MB / 5 845
entries). The web SCons options used by the Fast export are
`platform=web target=template_release threads=no webgpu=yes opengl3=no`
(`tools/grit_build.py:491-511`; web options at `SConstruct:210,725-727`,
`platform/web/detect.py:80,255-260`; `arch` defaults to `wasm32` in `detect.get_flags()`).

```sh
cd /tmp/godot48
export PATH="$HOME/fast_godot_toolchain/python/bin:$HOME/fast_godot_toolchain/emsdk/upstream/emscripten:$HOME/fast_godot_toolchain/emsdk/node/24.19.0_64bit/bin:$PATH"
export PYTHONPATH="$HOME/fast_godot_toolchain/scons/4.11.1/scons-4.11.1"
"$HOME/fast_godot_toolchain/python/bin/python3" -m SCons \
    platform=web target=template_release threads=no webgpu=yes opengl3=no \
    cache_path="$HOME/fast_godot_toolchain/scons-cache" cache_debug=no -j8
```

- The toolchain python must be first on `PATH`: macOS `python3` is 3.9 and emcc rejects it (see
  `docs/WEBGPU_HANDOFF.md`, "Build / toolchain"). `EM_CONFIG`/`EMSDK` are not required for a
  manual `scons` run (verified); only the `PATH` above matters.
- **Expected duration:** ~5–15 minutes incremental — header fan-out dominates
  (`rendering_device.h` has 44 direct includers, and `rendering_device_commons.h` /
  `rendering_device_driver.h` are included through it by much of `renderer_rd`; the SCons
  dependency scan rebuilds every dependent TU), then a wasm link (~1–2 min) and the `.zip`. A cold
  cache/first build in this tree is 45–90 minutes. Run it once after both implementers' edits are in.
- Artifacts to check (names include `.nothreads` because `threads=no`):
  `bin/godot.web.template_release.wasm32.nothreads.wasm`,
  `bin/godot.web.template_release.wasm32.nothreads.zip`,
  `bin/godot.web.template_release.wasm32.nothreads.{js,engine.js}`.
- The editor's own Grit HTML5 Fast export builds a *seeded copy* of the source
  (`tools/grit_build.py:seed_tree`), not this tree, so a direct `scons` here is the fast
  compile-check; for the browser demo either export through the editor (it re-seeds every build) or
  copy the freshly built template over the export preset's template.

**Spot-check that the raw WGSL bypasses Tint**

1. Build-time evidence (no browser needed): the diagnostic from §6.3 is compiled in. Use
   `print_line` for the milestone (the web build does not pass `--verbose`, so `print_verbose` is
   silent in the browser) and revert it to `print_verbose`/`WEBGPU_DIAG` before merging.
   `grep -a -c "raw WGSL" bin/godot.web.template_release.wasm32.nothreads.wasm` must be ≥ 1
   (0 means the raw branch was compiled out or the string optimized away under the chosen
   `optimize=` level — in that case use the browser check).
2. Browser evidence, DevTools console:
   - the log line `WebGPU: raw WGSL for '<name>' stage 4, Tint bypassed.` appears exactly once per
     upscaler shader (module and BGL creation are deferred, so it appears when the first
     pipeline/uniform set is created);
   - no `WebGPU: SPIR-V→WGSL conversion failed` and no `[SHADER] WGSL stats: ... tint_misses=`
     increase *after* those two lines. `tint_misses` is at
     `rendering_device_driver_webgpu.cpp:375` and is only logged with `WEBGPU_VERBOSE`; the
     `[PERF] create:` line at `:10320-10323` (also `WEBGPU_VERBOSE`) prints the cumulative `tint=`
     ms and `baked=` hits — `baked` increments by 2 for EASU+RCAS;
   - the WebGPU pipeline/module labels contain our shader names (`mod:<name>:stg4`,
     `bgl:<name>:set0`, `plyt:<name>`, `pipe#<n>:<name>`, `:4631,5456,5614,9611`), so Dawn
     validation messages identify our shaders;
   - zero `GPUValidationError` / `[Godot-WebGPU] uncaptured error` lines, and the expected
     dispatch count/visual result.
   Turning on `WEBGPU_VERBOSE` (uncomment `#define WEBGPU_VERBOSE`, `:55-64`) additionally logs the
   JS-side create calls and `[SHADER]`/`[BGL-DUP]` diagnostics; the `rendering/webgpu/debug_frames`
   project setting (`:468`) prints the `[DBGFRAME]`/`[PERF]` lines every frame.

---

## 10. Risks / open questions for the implementers

1. **Binding convention is invisible at compile time** (`@binding(2*N)`). The failure mode is a Dawn
   validation error like "binding index out of range" or "entry not present". Mitigations: assert in
   `_double_wgsl_bindings` that every reflected binding appears as `@binding(2*N)` in the produced
   WGSL (cheap `String.find`), log the mapping once per shader, and add one tiny WGSL compute test
   shader (e.g. `@group(0) @binding(0) var<uniform> u; @group(0) @binding(1) var img:
   texture_storage_2d<rgba16float, write>` writing `u.x` to a full-screen image) before porting
   EASU/RCAS.
2. **Gating the Tint rewrites** (§6.3b): a `if (!raw_wgsl)` around 500 lines must not change baked
   SPIR-V shader behaviour. Keep it mechanical and verify the Forward+ TPS run still shows
   `baked=`/0 validation errors afterwards (the existing browser regression check).
3. **Sample type vs filterable formats** (gotcha 9): the spatial path is safe with RGBA16F; the
   temporal path needs r32float depth (or a linear-depth r32float view) as a sampled `texture_2d`
   and the current BGL always uses `Float` for `texture_2d<f32>`. Decide now whether v1 raw WGSL may
   use `float32-filterable` (requested when available, `platform/web/js/engine/engine.js:351`) or
   whether the driver needs a small extension to derive `UnfilterableFloat` from
   `WGSLUniform.texture_format` (the reflection field is already carried through; only the BGL
   sampleType decision at `rendering_device_driver_webgpu.cpp:5164-5185` would change).
4. **Cross-compartment build verification for non-WebGPU drivers.** The WebGPU container helper is a
   new public method on a driver-only class; the driver virtual is non-pure specifically so
   Vulkan/Metal/D3D12/GLES3 (files outside the patch stack) compile unchanged. A native editor build
   (`docs/WEBGPU_HANDOFF.md` build command) is the cheapest way to verify that once; the web build
   does not compile those drivers.
5. **NDC/Y-flip conventions.** Hand-written WGSL is not run through `spirv_preprocess`, so it does
   not get the SPIR-V preprocessor's Vulkan→WebGPU adjustments. pmndrs WGSL is authored for WebGPU
   directly (fine), but double-check the upscaler's UV/Y conventions against our render-target
   orientation (`textureLoad` coordinates, not sampler wrapping — EASU/RCAS clamp explicitly).
6. **Uniform-buffer layout vs the CPU writer.** `FsrConstants` must stay byte-identical to the C++
   writer (offset table in `packages/core/src/shaders/common.ts` and
   `internal/ConstantsBuffer.ts`, 96 bytes). No push constants were used here, so a mismatch shows
   up as garbage parameters, not as a validation error.
7. **Shader embedding** (see §8.3): decide generated header vs SCsub builder vs runtime `res://`
   text. Runtime files are fine only if the export filter includes them; the renderer should not
   depend on that for a built-in effect.
8. **Firefox/Safari validation**: WGSL `rgba16float` write-only storage and `textureLoad` on
   `texture_2d<f32>` are core; read_write storage is not (gotcha 8). Validate the spatial path on
   Firefox at least once.
9. **Name ambiguity in logs/caches**: `_spv_to_wgsl_baked_hits` counts both editor-baked SPIR-V
   shaders and our raw WGSL; fine for a bypass check, but don't use it as a "raw shader" counter in
   shipping code — add a dedicated `_raw_wgsl_stages` counter if the diagnostic setting is kept.
10. **Scope creep**: this path is generic; resist exposing it on `RD` to GDScript/ClassDB for
    milestone 1 (the handoff wants it private to the upscaler). If a GDScript demo is needed later,
    add a separate `RDShaderWGSL` resource rather than a Variant-conversion of `WGSLShaderData`.

---

## Appendix — quick evidence index

| claim | location |
|---|---|
| WGSL per stage / driver prefers it | `drivers/webgpu/rendering_shader_container_webgpu.h:43-57,79`; `rendering_device_driver_webgpu.cpp:4103-4107` |
| deferred shader build; container kept | `rendering_device_driver_webgpu.cpp:3951-3972`; `webgpu_objects.h:144-149` |
| SPIR-V emptiness rejection (to relax) | `drivers/webgpu/rendering_device_driver_webgpu.cpp:4086` |
| Tint rewrites (to gate) | `drivers/webgpu/rendering_device_driver_webgpu.cpp:4117-4622` |
| binding doubling in driver | `rendering_device_driver_webgpu.cpp:5149,5164,5189-5201,5232,5259,…`; `spirv_preprocess.cpp:1243,1328,1338-1346` |
| BGL from reflection + WGSL scan | `rendering_device_driver_webgpu.cpp:5069-5470,4638-5041` |
| pipeline layout / no push constants | `rendering_device_driver_webgpu.cpp:5478-5600` |
| entry point "main" | `rendering_device_driver_webgpu.cpp:10021` |
| compute pass/set binding/usage guard | `rendering_device_driver_webgpu.cpp:9648-9700,9803-9935`; `rendering_device.cpp:7124-7380,7532-7551` |
| uniform set validation | `rendering_device.cpp:4799-5246` |
| RD shader registration tail | `rendering_device.cpp:4628-4703` |
| RD/driver shader APIs | `rendering_device.h:1058-1061,1089-1160,1704-1713`; `rendering_device_driver.h:570` |
| container reflection API | `rendering_shader_container.h:47-305`; `rendering_shader_container.cpp:683-798,901-1000` |
| WGSL-only container serialization already supported | `rendering_shader_container_webgpu.cpp:121-169`; `rendering_shader_container.cpp:801-895` |
| storage format promotion (RGBA16F unaffected) | `rendering_device_driver_webgpu.cpp:1877-1900,3007-3068`; usages `:2840-2902` |
| default texture view uses all mips | `rendering_device_driver_webgpu.cpp:1979-1985` |
| float32-filterable requested | `platform/web/js/engine/engine.js:351` |
| RD free/dependency handling | `rendering_device.cpp:225-251,8142-8148,8553-8560` |

---

## 11. Implementation notes (as built)

Status: implemented on top of the patch stack (layers 0004 for `drivers/webgpu/`, 0005
for `servers/rendering/`; no `patches/layers/*.files` changes). Everything in §3–§6 is
implemented as specified; the notes below record the built shape, the small deviations,
and the verification evidence.

### 11.1 Final API (as built)

```cpp
// servers/rendering/rendering_device.h (public, next to the other shader APIs)
struct WGSLUniform {
	UniformType type = UNIFORM_TYPE_MAX;
	uint32_t binding = 0;
	uint32_t length = 0;
	TextureType texture_type = TEXTURE_TYPE_2D;
	DataFormat texture_format = DATA_FORMAT_MAX;
	bool writable = false;
};
struct WGSLShaderData {
	String name;
	String code;
	Vector<Vector<WGSLUniform>> uniform_sets;
	uint32_t workgroup_size[3] = { 8, 8, 1 };
};
RID shader_create_from_wgsl(const WGSLShaderData &p_data);

// servers/rendering/rendering_device.h (private)
RID _shader_register(RDD::ShaderID p_driver_id, const ShaderReflection &p_reflection,
		const String &p_shader_name, RID p_placeholder);

// servers/rendering/rendering_device_driver.h (non-pure virtual, next to
// shader_create_from_container; default in rendering_device_driver.cpp fails cleanly)
virtual ShaderID shader_create_from_wgsl(const String &p_wgsl,
		const ShaderReflection &p_reflection, const String &p_shader_name);

// drivers/webgpu/rendering_shader_container_webgpu.h (public)
bool set_code_from_wgsl_compute(const String &p_shader_name,
		const RDC::ShaderReflection &p_reflection, const String &p_wgsl);

// drivers/webgpu/rendering_device_driver_webgpu.h
virtual ShaderID shader_create_from_wgsl(const String &p_wgsl,
		const ShaderReflection &p_reflection, const String &p_shader_name) override final;

// servers/rendering/rendering_device_commons.h
enum Features { ..., SUPPORTS_RAW_WGSL_COMPUTE };
```

### 11.2 Deviations / details worth knowing

1. **Binding convention**: authored WGSL `@binding(N)`; the container rewrites the WGSL
   text to `@binding(2*N)` in `_double_wgsl_bindings()` (§5 option A, "rewrite on load"),
   and the reflection keeps `N`. `WGSLUniform::binding` / `RD::Uniform.binding` are
   therefore the authored numbers (0,1,2 / 0..4 for EASU/RCAS). The WGSL author never
   writes doubled numbers. This settles the open question in
   `docs/PMNDRS_UPSCALER_SPATIAL.md` §8.2 in favour of option (A).
2. **Reflection/WGSL consistency check** (added, stronger than §10 risk 1 asked for):
   `set_code_from_wgsl_compute()` collects every `(@group(G), @binding(B))` pair from the
   rewritten WGSL with the *same grammar as the driver's `parse_group_binding()`*
   (`@group(G)` then spaces then `@binding(B)` on the same declaration), and fails with
   an RD error naming the shader/set/binding if a reflected binding has no matching
   declaration. The mapping is logged once per shader with `print_verbose` (the format
   string survives into the wasm build, see §11.3). Consequence for WGSL authors: keep
   the declaration on one line in the exact Tint shape the driver's scanners expect
   (`@group(G) @binding(B) var... : type;`) — a newline between the two attributes is
   *not* accepted by the existing driver scan either.
3. **RD validation** (§6.1) additionally rejects empty inner uniform sets, more than
   `MAX_UNIFORM_SETS` (16) sets, empty WGSL, and zero workgroup-size components. Each
   set is copied, normalized (`stages = SHADER_STAGE_COMPUTE_BIT`, length defaults) and
   sorted before duplicate-binding detection. `push_constant_size` stays 0 and
   `push_constant_stages` empty.
4. **`_shader_register()`** is the exact code that used to be the tail of
   `shader_create_from_bytecode_with_samplers()` (RID/placeholder, reflection copy,
   set-format cache, `stage_bits`, `set_resource_name` in DEV builds; it takes
   `_THREAD_SAFE_METHOD_`). Both creation paths call it; the bytecode path passes the
   container's name/reflection, the WGSL path the values it already validated.
5. **`_shader_build()` raw-stage detection** is `raw_wgsl = !baked_wgsl.is_empty() &&
   spv_bytes.is_empty()` — i.e. exactly the "WGSL present + SPIR-V empty" marker, so a
   future serialized WGSL-only container (§2.3) is accepted too. For a raw stage:
   `stage_spirv[]` is left empty (so the specialization-constant re-conversion branch in
   `compute_pipeline_create()` is inert), the empty-SPIR-V/multiple-of-4 checks are
   skipped, and the 500-line Tint-output WGSL rewrite region is skipped by wrapping it
   in `if (!raw_wgsl) { ... }`; the region was not re-indented to keep the patch
   mechanical. The post-module WGSL *scans* (view dimension, sample type, storage
   format/access, SSBO read-only flag, per-stage visibility, override ids) still run for
   raw stages — they are the source of the BGL metadata.
6. **`print_verbose` vs `print_line`** for the §6.3 diagnostic: built with
   `print_verbose("WebGPU: raw WGSL for '%s' stage %d, Tint bypassed.")`, so the browser
   (no `--verbose`) stays quiet. The literal is still compiled into the wasm for the
   build-time bypass check.
7. **WebGPU driver override** builds a `RenderingShaderContainerWebGPU`, calls
   `set_code_from_wgsl_compute()`, then `shader_create_from_container(container, {})`
   (deferred build as usual; no immutable samplers). `has_feature(SUPPORTS_RAW_WGSL_COMPUTE)`
   returns true only here; every other driver keeps the default virtual (invalid
   `ShaderID()` + `ERR_FAIL_V_MSG`), gated by RD's feature check.
8. **Container side state**: `shaders.resize(1)` with `shader_stage = COMPUTE` and empty
   `code_compressed_bytes` (the raw marker), `stage_wgsl[0]` = rewritten WGSL,
   `HEADER_FLAG_HAS_WGSL` set, `push_constant_bind_group = NO_PUSH_CONSTANTS`. The
   container also mirrors `ReflectShader::stages_vector` (the base derives the
   serialized stage list from `stages_bits`, so this is redundant but keeps the local
   `ReflectShader` self-consistent for a future `to_bytes()` test fixture).
9. Compute pipelines use the driver's hard-coded entry point `"main"`; raw WGSL has no
   `override`/spec-constant support, no push constants, no combined image-samplers and
   no read/read_write storage textures (v1 rules from §5/§7 remain in force).

### 11.3 Verification evidence

- **Web template compile-check** (command from §9, run in `/tmp/godot48`):

  ```sh
  cd /tmp/godot48
  export PATH="$HOME/fast_godot_toolchain/python/bin:$HOME/fast_godot_toolchain/emsdk/upstream/emscripten:$HOME/fast_godot_toolchain/emsdk/node/24.19.0_64bit/bin:$PATH"
  export PYTHONPATH="$HOME/fast_godot_toolchain/scons/4.11.1/scons-4.11.1"
  "$HOME/fast_godot_toolchain/python/bin/python3" -m SCons \
      platform=web target=template_release threads=no webgpu=yes opengl3=no \
      cache_path="$HOME/fast_godot_toolchain/scons-cache" cache_debug=no -j8
  ```

  Result: `scons: done building targets.` — `INFO: Time elapsed: 00:03:24.88`, exit 0
  (incremental; relinked `bin/godot.web.template_release.wasm32.nothreads.wasm`).
  The pmndrs effect (`renderer_rd/effects/pmndrs_upscaler.cpp`, which consumes this new
  RD API) compiled and linked against the new headers in the same build.
  `grep -a -c "raw WGSL" bin/godot.web.template_release.wasm32.nothreads.wasm` → `4`
  (≥1 as required by §9 item 1).
- **Native editor compile-check** (proves the non-WebGPU drivers compile unchanged with
  the new non-pure virtual; `rendering_device_driver_webgpu.cpp` is not part of this
  target, but `rendering_shader_container_webgpu.cpp` is compiled in its
  `WEBGPU_BAKE_WGSL` editor configuration):

  ```sh
  cd /tmp/godot48
  export PATH="$HOME/fast_godot_toolchain/python/bin:$PATH"
  export PYTHONPATH="$HOME/fast_godot_toolchain/scons/4.11.1/scons-4.11.1"
  "$HOME/fast_godot_toolchain/python/bin/python3" -m SCons \
      platform=macos arch=arm64 target=editor production=yes lto=none tests=no \
      debug_symbols=no angle=no vulkan_sdk_path="$HOME/fast_godot_toolchain/moltenvk" \
      cache_path="$HOME/fast_godot_toolchain/scons-cache" -j8
  ```

  Result: `scons: done building targets.` — exit 0 (only pre-existing
  `-mmacosx-version-min` linker warnings from the Tint objects).
- **Binding-transform check** on the vendored shaders (mirror of the C++ string logic
  run over the two raw WGSL blocks): EASU authored `@binding(0,1,2)` → rewritten
  `@binding(0,2,4)`, refound pairs `(0,0),(0,2),(0,4)`; RCAS authored `@binding(0..4)`
  → rewritten `@binding(0,2,4,6,8)`, pairs `(0,0),(0,2),(0,4),(0,6),(0,8)` — exactly
  the reflected doubled bindings, i.e. no "reflection/WGSL mismatch" failure.

---

## 12. M2 raw-WGSL driver extensions (temporal path)

Milestone 2 (pmndrs temporal upscaler, `docs/PMNDRS_TEMPORAL_PORT.md`) needed three
driver-side extensions on top of §11. They are implemented in
`drivers/webgpu/` only; no `servers/rendering/` API change was needed. The container
format, binding doubling (§5/§11.2) and reflection contract are unchanged.

### 12.1 Gap A — sampled-texture sample-type inference (`texture_2d<f32>` load-only)

WebGPU's auto-layout rule for a sampled f32 texture is: `float` if it is statically
used *with a sampler* (`textureSample`/`textureSampleLevel`/`textureSampleBias`/
`textureSampleGrad`/`textureGather`), otherwise `unfilterable-float`. The raw-WGSL
temporal passes bind `r32float` dilated depth / history / shading textures via
`textureLoad` only, so the previous component-only inference (`f32 → Float`) produced
an invalid BGL.

Implemented in `_shader_build()`:

- the existing call-site scan (`wgsl_sampler_needs_nonfiltering`) now also records
  `wgsl_tex_sampled_with_sampler[(set << 16) | binding] = true` for every texture
  resolved as the first argument of one of those five builtins with a sampler as the
  second argument;
- the standalone `UNIFORM_TYPE_TEXTURE` / `INPUT_ATTACHMENT` BGL entry computes
  `sampleType` as before for Depth/Uint/Sint/MSAA, but for an f32 binding of a
  **raw-WGSL shader** (`raw_wgsl_shader`, set when a WGSL-only stage is present) it is
  `UnfilterableFloat` when the binding is not sampled with a sampler and `Float`
  otherwise;
- the rule is scoped to raw-WGSL containers: SPIR-V/Tint stages keep byte-identical
  BGL entries (the inference map is populated for every stage but only consumed when
  the container carries a raw WGSL stage — raw containers are single-stage compute by
  construction). Per-binding decisions are logged with `print_verbose`.

Verified against all 16 vendored temporal modules with a mirror of the driver's
scanner (declaration scan + call-site scan): the produced sample types match
`docs/PMNDRS_TEMPORAL_PORT.md` Appendix A exactly — e.g. accumulate b4/b7 `float`,
everything else `unfilterable-float`; exposure b1 `float`, b3/5/6/7
`unfilterable-float`; blit b1/b5 `float`, b3 `unfilterable-float`; shading-change b9
`uint`; reconstruct b1 `depth`; RCAS/debug all `unfilterable-float`.

Residual risk: detection follows *static* call sites. A raw shader that passes the
texture handle or sampler through a function parameter (so the call site names neither
binding) would not be detected and would get `UnfilterableFloat` for a
sampler-sampled texture (pipeline creation error) — the vendored WGSL only uses direct
`textureSampleLevel(<binding>, <binding>, …)` calls, which is the supported shape.
Also, a raw shader sampling an unfilterable format (`r32float`) with a sampler cannot be
described by this inference (auto-layout would demand `float`); use `textureLoad`.

### 12.2 Gap B — depth-only view for `texture_depth_2d` bindings

The engine scene depth is a combined depth-stencil texture (`D32_SFLOAT_S8_UINT` when
the WebGPU feature is present, otherwise `D24_UNORM_S8_UINT` → `depth24plus-stencil8`)
whose default view has `aspect: all`. WebGPU requires a single-aspect
(`depth-only`) view for a `Depth` sample type, so binding scene depth as
`texture_depth_2d` failed.

Implemented:

- `WGTexture::depth_view` — a lazily created, cached `aspect: depth-only` view that
  keeps the same format, dimension, base mip/layer and mip/layer counts as the default
  view. It is created by `RenderingDeviceDriverWebGPU::_depth_only_view()`, stored on
  the owning `WGTexture`, released in `texture_free()`, and explicitly nulled in
  `texture_create_shared()` / `texture_create_shared_from_slice()` (those copy the
  `WGTexture` struct, so the cached view must not be shared/double-released). A verbose
  line is printed the first time such a view is created.
- `uniform_set_create()`'s standalone `UNIFORM_TYPE_TEXTURE` path: when the BGL entry's
  `sampleType` is `Depth`, the depth-only view is bound instead of `default_view`
  (falling back to `default_view` for non-depth-format textures). The depth-only view is
  also created for depth-only formats (an explicit `depth-only` aspect is always valid
  and matches the existing depth-copy/MSAA-depth paths).
- `_get_compatible_bind_group()` (BGL rebinding fallback): a cached entry created for a
  non-Depth layout whose target BGL expects `Depth` is adapted to the bound texture's
  depth-only view, mirroring the existing Depth→Float adaptation.

Effect-side requirements (report): declare the depth pass binding as
`UNIFORM_TYPE_TEXTURE`, `TEXTURE_TYPE_2D`, `length = 1` (WGSL `var sceneDepth :
texture_depth_2d;`); bind an RD depth texture created with `TEXTURE_USAGE_SAMPLING_BIT`
(scene depth already has it, see `RenderSceneBuffersRD::get_depth_usage_bits()`), 1
sample, 1 mip, 1 layer; `texture_format = DATA_FORMAT_D32_SFLOAT` is informational (RD
only warns on mismatch, the BGL sample type comes from the WGSL text). MSAA depth is not
supported through `texture_depth_2d` (use `texture_depth_multisampled_2d` + MSAA
binding, or a resolved depth target).

### 12.3 Gap C — storage buffers / atomics, samplers, multiple bind groups (verified)

Audited, no code change required:

- **Storage buffers incl. `array<atomic<u32>>`**: the reflection
  (`UNIFORM_TYPE_STORAGE_BUFFER`, `length = 0`, `writable` true for `read_write`) maps to
  the BGL buffer type via the WGSL scan (`wgsl_ssbo_readonly` parses the exact spellings
  the vendored modules use: `var<storage, read_write>`, `var<storage, read>`,
  `var<storage>`); bind-group creation binds `offset 0` and `size = buffer size`
  (multiple of 4, ≤ `maxStorageBufferBindingSize`); `buffer_update` before the compute
  list and `storage_buffer_create(size, data)` (initial `0x7f800000` scatter data) use
  the normal transfer-worker path. Atomic operations (`atomicMin` etc.) need no driver
  work (core WGSL, plain `Storage` buffer BGL entry).
  Caveat (pre-existing, not raw-specific): the driver's writable-storage aliasing stub
  redirects a *second* binding of the same `WGPUBuffer` in one uniform set to a 64 KB
  dummy. The temporal passes never bind one buffer to two storage bindings (the
  scatter ping-pong buffers are distinct RIDs, and depth-clip's read/write buffers
  differ), so this must stay true.
- **Samplers**: plain `var s : sampler` bindings get `Filtering` (correct for the linear
  clamp sampler used by exposure/accumulate/blit); the bind group passes the created
  `RD::sampler_create` sampler through. Only samplers statically paired with a
  `texture_depth_*` binding are switched to `NonFiltering` (existing behavior).
- **Multiple bind groups**: reflection `uniform_sets` → one BGL per set → pipeline layout
  with `bindGroupLayoutCount = set_count`; uniform-set bindings are per set. This is
  structurally supported for any number of sets (RD `MAX_UNIFORM_SETS = 16`); only the
  every-texel exposure form (two groups, deferred per
  `docs/PMNDRS_TEMPORAL_PORT.md` §12.3 Gap C) would exercise it. Not end-to-end tested
  in this milestone (no raw shader with `@group(1)` is shipped yet); the mandatory
  temporal slice uses one bind group throughout.
- **Not required / still deferred**: multiple entry points per module (every-texel
  exposure), read/read_write storage textures (all temporal storage images are
  `write` only, formats `r32float`/`rgba8unorm`/`rgba16float`/`rgba32uint` are all in
  the driver's format table).
