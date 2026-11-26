# Patch stack

Each branch is a full, ordered series against the commit in `upstream.lock`.
`tools/apply.sh <godot-src> <branch-name>` applies them with `git apply --3way`.

## `4.7.2` (base `4.7.2-stable`, `ed1daf0bf0`)

| patch | contents |
|---|---|
| `0001-modules-grit` | Fast/Grit module: export compiler, runtime, editor plugins |
| `0002-modules-cffi` | CFFI module |
| `0003-web-html5-fast-export` | "HTML5 Fast" export platform + web platform hooks (detect.py, js/engine, display server, export options) |
| `0004-webgpu-driver` | `drivers/webgpu` WebGPU driver, SCsub, Vulkan driver API additions |
| `0005-engine-renderer-patches` | RenderingDevice/RendererRD/editor/main patches the driver needs (batching, buffer_flush, shader container, bake registration, frame profiler) |
| `0006-thirdparty-vendored` | vendored Tint + SPIRV-Tools for the export-time WGSL bake |
| `0007-tools-fixtures-docs` | tools/, test_project/, docs, webgpu notes/tests |
| `0099-misc` | remaining touched files |

Regenerate: `./tools/make-patches.sh <godot-checkout> 4.7.2`

## `4.8-beta5` (base: godot `master` at `67421f36e5`, 4.8.0-beta)

Ported from the `4.7.2` stack — same layers, adapted to 4.8:

| patch | contents |
|---|---|
| `0001-modules-grit` | Fast/Grit module (adapted to 4.8 GDScript internals: GDType member maps, const MethodBind codegen, parser resolved_type/type_constraint) |
| `0002-modules-cffi` | CFFI module |
| `0003-web-html5-fast-export` | "HTML5 Fast" export platform + web platform hooks |
| `0004-webgpu-driver` | komm64/4.7 WebGPU driver (MIT) rebased onto 4.8: base RenderingDeviceDriver gained `buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`, readback/upload conversion hooks, layered copy/initialize, and the shader container format now returns the SPIR-V version per stage |
| `0005-engine-renderer-patches` | engine/renderer patches the driver needs (batching with `batch_instance_index`, shader container format override, driver traits, GDScript native-call hooks, frame profiler) |
| `0006-thirdparty-vendored` | vendored Tint + SPIRV-Tools for the export-time WGSL bake |
| `0007-tools-fixtures-docs` | tools/, test_project/, docs, webgpu notes/tests |
| `0099-misc` | remaining touched files |

Build requirements: Emscripten 6.0.1+ for web, `zstandard` Python module for SCons.
Verified on macOS: editor builds and boots; web template builds
(`platform=web target=template_release threads=no webgpu=yes opengl3=no`).

Deferred when porting: the white-box pack-key generator from `core_builders.py`
(shimmed, so encrypted web templates are unavailable) and the v2 authenticated
chunk format in `FileAccessEncrypted` (4.8 API drifted).
