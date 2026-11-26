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

| patch | contents |
|---|---|
| `0001-modules-grit` | Fast/Grit module |
| `0002-webgpu-driver` | 4.8-native WebGPU driver + shader bake support (`drivers/webgpu`, brotli, webgpu scripts) |
| `0003-engine-renderer-patches` | rendering/editor/main/platform patches for the 4.8 driver |
| `0004-web-html5-fast-and-grit` | HTML5 Fast export wiring, Grit build tools, test project, docs |

Provenance: the initial 4.8 WebGPU port incorporates MIT-licensed code from
`davtamay/Godot_WebGPU` and `komm64/godot-webgpu`, adapted and maintained as
our own stack from here on.

Regenerate: `./tools/make-patches.sh <godot-checkout> 4.8-beta5`
