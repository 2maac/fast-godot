> **ARCHIVED — 2026-11-26.** This is the historical checklist for adopting
> `komm64/godot-webgpu` (4.7) and is kept for reference only. Its branch
> (`integrate/komm64-webgpu`) and the 4.7.2 stack have been deleted. For current
> status, build/export instructions, the MSAA fix, encryption and open work, use
> [`WEBGPU_HANDOFF.md`](WEBGPU_HANDOFF.md) instead.

# WebGPU driver migration: adopt `komm64/godot-webgpu` (4.7)

Status: **in progress** on branch `integrate/komm64-webgpu` (see Progress at the end). This document is the checklist for replacing the
WebGPU driver + engine-side WebGPU patches in this checkout with the maintained
4.7 port from [`komm64/godot-webgpu`](https://github.com/komm64/godot-webgpu)
(branch `4.7`, commit `ce9595df83` as of 2026-11-25), **while keeping fast-godot's
HTML5 export style (Grit `HTML5 Fast`)**.

## Why

Both trees descend from `dwalter/godotwebgpu`, so the `RenderingDeviceDriver` /
`RenderingContextDriver` interface and the push-constant ring design are the same;
komm64's branch is the actively maintained, 4.7-tested evolution of it. Its
`webgpu_notes/` documents fixes for exactly the stalls measured here
(`[FRAME]`/`[BUFUP]`/`[TEXUP]` profiling, `WEBGPU_HANDOFF.md`):

| Note | What it explains |
|---|---|
| `BUFFER_UNMAP_32MB_FLUSH_ROOT_CAUSE.md` | every `buffer_unmap()` copied the whole staging buffer (32 MB) for a 64-byte write; fixed with dirty ranges (we have that) and diff-flush (we just ported it) |
| `IPC_OPTIMIZATION_SUMMARY.md` | per-draw `SetBindGroup`/`SetVertexBuffer`/`DrawIndexed` each cost a Mojo IPC crossing; 99.96% reduction via command-stream recording |
| `COMMAND_BUFFERING_WASM_TO_JS.md`, `COMMAND_STREAM_RECORDER_PERF_OPTIM.md` | the wasm-side recorder that batches those crossings into one JS call |
| `push_constant_ring_buffer.md` | PC ring + dirty tracking, dynamic offsets |
| `STARTUP_PROFILING.md`, `SLOW_WINDOW_OR_TEXTURE_CREATION.md` | startup/texture-creation hitches |
| `precompile_specialized_to_wgsl.md`, `naga_to_tint.md` | shader precompilation / backend history |

Their driver is ~9,960 lines with 18 SPIR-V prepasses vs our 9,645 / 15, and
already contains depth-only views, `viewFormats`, MSAA handling and sRGB pairing
(verify per-file during the port).

## What must NOT change: the HTML5 export style

fast-godot's export model is not in komm64's tree and must be preserved verbatim:

- `platform/web/export/export_plugin_fast.h` (+ `.cpp`) — `EditorExportPlatformWebFast`:
  the Grit-driven per-project template, "no template yet" must not disable Export,
  Forward Mobile + `webgpu` renderer enforcement (UI, `GritExportPlugin::prepare_export`,
  `export_project` backstop), Advanced-Options-only custom-template fields.
- `platform/web/export/export_plugin.{h,cpp}` — `EditorExportPlatformWeb` base; the
  `shader_baker/enabled` option, `shader_baker` feature, `get_shader_baker_driver()`
  returning `webgpu`, and `get_preset_features()` (s3tc/…/wasm32/`nothreads`).
- `modules/grit/**`, `tools/grit_build.py`, `tools/build-editor.sh`,
  `tools/stage_editor.sh`, `tools/export_webgpu_grit.sh` — Grit build/export path,
  including the `GODOT_VERSION_HASH_OVERRIDE` pinning added for shader cache keys.
- `misc/dist/html/*` WebGPU-aware shell + `platform/web/js/engine/engine.js`
  (device creation, `maxInterStageShaderVariables` request).
- Our editor-side additions: `editor/shader/shader_baker/shader_baker_export_plugin_platform_webgpu.*`,
  `editor/export/shader_baker_export_plugin.*` (WGSL table baking),
  `drivers/webgpu/wgsl_cache_blob.h`, `core/debugger/frame_profile.h` + hooks in
  `main/main.cpp` / `rendering_server_default.cpp`.

## What gets replaced

1. `drivers/webgpu/` — all driver files (`rendering_device_driver_webgpu.*`,
   `rendering_context_driver_webgpu.*`, `rendering_shader_container_webgpu.*`,
   `webgpu_objects.h`, `pixel_formats_webgpu.h`, `spirv_preprocess.*`,
   `spirv_to_wgsl.*`, `tint_wrapper.*`, `wgsl_precompile.py`, `SCsub`, `README.md`).
2. Engine-side WebGPU patches they carry (to be enumerated from their tree):
   `servers/rendering/**` WebGPU paths, `platform/web/**` (display server, JS shell,
   engine.js), `main/main.cpp` device bootstrap, `thirdparty/tint` + `spirv-tools`
   pin, `modules/glslang` glue — keeping our `core/debugger/frame_profile.h` hooks.
3. Their `webgpu_notes/` into `webgpu_notes/` here (reference material).

## Port order (each step must build)

1. **Branch**: `integrate/komm64-webgpu` off the current `fresh` work.
2. **Vendor the driver**: copy their `drivers/webgpu/` over ours. Re-apply from our
   side only what the export layer needs there: the `wgsl_cache_blob.h` include +
   `_load_baked_wgsl_cache()` / `_read_baked_wgsl()` hooks in
   `shader_create_from_container`, and the frame-profiler counters/`[TEXUP]`/`[BUFUP]`
   helpers (port the diff-flush only if their unmap path lacks it).
3. **Engine patches**: diff their tree against the same 4.7 base as ours and apply
   the WebGPU-specific hunks, skipping anything under `platform/web/export/` (ours).
4. **Build the editor** (`./tools/build-editor.sh`), then a Grit `HTML5 Fast` export
   of `test_project` with the shader baker enabled.
5. **Verify, in this order**:
   - `tools/smoke_webgpu.js` → PASS, 60 FPS, 12 draws, 0 GPU/other errors.
   - Fresh-profile TPS: shader cache hits, 0 GPU errors, `[FRAME]` load/hitch profile
     vs the current baseline (84 s load burst, 2–6 s gameplay hitches).
   - Upstream suites: preprocessing (198), shader corpus (13), driver unit (327).
6. **Keep or revert per measurement**: batching (`batch_instance_draws`), the
   `GODOT_VERSION_HASH_OVERRIDE` pin (needed for cache keys — their template build
   may already pin it), and our WGSL baked table (drop only if their
   precompile/command-stream work makes it redundant).

## Risks / gotchas

- Their driver expects their engine-side helpers (`webgpu_probe`, `spirv_to_wgsl`,
  a different `SCsub` with Tint/Naga wiring); a partial copy will not link.
- Their web export platform is stock (no Grit); do not copy anything under
  `platform/web/export/`.
- Our `test_project/project.godot` loses `rendering_device/driver.web` whenever a
  **windowed** native editor session rewrites project settings (known); restore it
  after such sessions.
- Re-verify the shader-cache key story if their template build does not inherit the
  editor's `GODOT_VERSION_HASH`: without it every baked `.webgpu.cache` misses.
- Their notes reference Naga in older files and Tint in newer ones; read
  `naga_to_tint.md` before assuming the backend.

## Progress log

1. Branch `integrate/komm64-webgpu` created; our `drivers/webgpu` backed up to
   `/tmp/our_webgpu_driver`.
2. Vendored all 17 files of their `drivers/webgpu/` (driver, context driver,
   shader container, `webgpu_objects.h`, `pixel_formats_webgpu.h`,
   `spirv_preprocess.*`, `spirv_to_wgsl.*`, `tint_wrapper.*`, `wgsl_precompile.py`,
   `SCsub`, `README.md`) plus their `tint_cli/` (our copy referenced the old
   prepass names: `fix_depth_images`, `inline_user_functions`,
   `strip_storage_nonreadable`).
3. `drivers/SCsub` uses their hunk: `SConscript("webgpu/SCsub")` also for
   `editor_build`, and their SCsub has the `if not env["webgpu"]` bake branch
   (`WEBGPU_BAKE_WGSL`) that compiles Tint + SPIRV-Tools into the editor.
4. Their `editor/shader/shader_baker/shader_baker_export_plugin_platform_webgpu.*`
   and the baker `SCsub` replace ours (the platform file is identical to ours).
5. Our separate WGSL side-table (`_bake_wgsl_cache()`) is disabled: their shader
   container bakes **WGSL per stage into `.webgpu.cache`** (`HEADER_FLAG_HAS_WGSL`),
   which removes the runtime Tint step without a second blob.
6. `thirdparty/tint/src/tint/utils/strconv/parse_num.cc` patched: libc++ on the
   SDK here gates float `std::from_chars` behind macOS 26 availability while the
   editor targets 11.0; it now uses `strtof`/`strtod`/`strtold`.
7. Stale Grit profile trees removed (their `webgpu/SCsub` globs
   `#drivers/webgpu/*.cpp`, so removed files used to linger in the profile copy).

Still to do:
- Fix compile errors from their driver against our engine tree (their engine-side
  patches are not applied yet — this is the next chunk).
- Keep our export layer untouched (`platform/web/export/**`, Grit tools).
- Re-verify: smoke test, Grit `HTML5 Fast` export, TPS load/hitch numbers, upstream
  test suites.
- Drop `drivers/webgpu/wgsl_cache_blob.h` (unused after step 5) or re-wire it if
  their container path underperforms.

## Engine-side patch surface (69 files vs upstream `4.7.2-stable`)

Computed by comparing `git ls-tree -r` of `komm64/4.7` (`ce9595d`) against
`godotengine/godot` tag `4.7.2-stable` (`ed1daf0`). These are the only modified
files; everything else is additive (`drivers/webgpu/`, `webgpu_notes/`,
`webgpu_tests/`, Tint/SPIRV-Tools sources, `tint_cli/`, `webgpu_site/`).

Do **not** take `platform/web/export/export_plugin.{h,cpp}` from them — that is
our HTML5 Fast platform (Grit) and must stay. Everything else should be merged
hunk-by-hunk where fast-godot also touched the file (our repo has fast-godot +
port commits in most of them; a whole-file copy risks dropping those).

- `.gitattributes`
- `.github/workflows/web_builds.yml`
- `.gitignore`
- `COPYRIGHT.txt`
- `README.md`
- `SConstruct`
- `core/object/worker_thread_pool.cpp`
- `doc/classes/SoftBody3D.xml`
- `drivers/SCsub`
- `drivers/vulkan/rendering_device_driver_vulkan.cpp`
- `drivers/vulkan/rendering_device_driver_vulkan.h`
- `editor/editor_node.cpp`
- `editor/export/shader_baker_export_plugin.cpp`
- `editor/shader/shader_baker/SCsub`
- `main/main.cpp`
- `misc/dist/html/full-size.html`
- `modules/glslang/config.py`
- `platform/web/detect.py`
- `platform/web/display_server_web.cpp`
- `platform/web/display_server_web.h`
- `platform/web/emscripten_helpers.py`
- `platform/web/export/export_plugin.cpp`
- `platform/web/export/export_plugin.h`
- `platform/web/js/engine/config.js`
- `platform/web/js/engine/engine.js`
- `platform/web/js/libs/audio.worklet.js`
- `platform/web/js/libs/library_godot_audio.js`
- `platform/web/os_web.cpp`
- `platform/web/web_main.cpp`
- `platform/windows/display_server_windows.cpp`
- `scene/3d/physics/soft_body_3d.cpp`
- `scene/gui/color_picker.cpp`
- `scene/gui/color_picker.h`
- `scene/resources/2d/tile_set.cpp`
- `scene/resources/compressed_texture.cpp`
- `servers/display/display_server.cpp`
- `servers/rendering/renderer_rd/effects/tone_mapper.cpp`
- `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.cpp`
- `servers/rendering/renderer_rd/forward_mobile/render_forward_mobile.h`
- `servers/rendering/renderer_rd/pipeline_deferred_rd.h`
- `servers/rendering/renderer_rd/pipeline_hash_map_rd.h`
- `servers/rendering/renderer_rd/renderer_canvas_render_rd.cpp`
- `servers/rendering/renderer_rd/renderer_compositor_rd.cpp`
- `servers/rendering/renderer_rd/shader_rd.cpp`
- `servers/rendering/renderer_rd/shader_rd.h`
- `servers/rendering/renderer_rd/shaders/canvas_occlusion.glsl`
- `servers/rendering/renderer_rd/shaders/canvas_sdf.glsl`
- `servers/rendering/renderer_rd/shaders/effects/resolve.glsl`
- `servers/rendering/renderer_rd/shaders/effects/smaa_edge_detection.glsl`
- `servers/rendering/renderer_rd/shaders/effects/smaa_weight_calculation.glsl`
- `servers/rendering/renderer_rd/shaders/environment/sdfgi_direct_light.glsl`
- `servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl`
- `servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl`
- `servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile_inc.glsl`
- `servers/rendering/renderer_rd/shaders/skeleton.glsl`
- `servers/rendering/renderer_rd/storage_rd/light_storage.cpp`
- `servers/rendering/renderer_rd/storage_rd/light_storage.h`
- `servers/rendering/renderer_rd/storage_rd/material_storage.cpp`
- `servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp`
- `servers/rendering/renderer_rd/storage_rd/mesh_storage.h`
- `servers/rendering/renderer_rd/storage_rd/texture_storage.cpp`
- `servers/rendering/rendering_device.cpp`
- `servers/rendering/rendering_device.h`
- `servers/rendering/rendering_device_driver.cpp`
- `servers/rendering/rendering_device_driver.h`
- `servers/rendering/rendering_device_graph.cpp`
- `servers/rendering/rendering_device_graph.h`
- `thirdparty/README.md`
- `thirdparty/spirv-headers/include/spirv/unified1/spirv.hpp11`

First concrete blocker found: their driver declares

```cpp
virtual void buffer_flush(BufferID p_buffer, uint32_t p_used_size = UINT32_MAX) override final;
virtual bool buffer_map_ready(BufferID p_buffer) override final;
```

so `servers/rendering/rendering_device_driver.{h,cpp}` (and the Vulkan driver,
`rendering_device{,_graph}.{h,cpp}`, `pipeline_hash_map_rd.h`, etc. from the list
above) must be updated to their API before the web template compiles. The editor
already builds with their driver sources (offline bake path).

## Round 2 results (branch `integrate/komm64-webgpu`)

The swap now **builds, passes the smoke test and runs TPS with 0 GPU errors**:

- 11 merge conflicts resolved (engine API `buffer_flush(..., used_size)` /
  `buffer_map_ready` / `API_TRAIT_BUFFER_MAP_RETURNS_SHADOW_COPY`, their batching
  in `render_forward_mobile.cpp` + `scene_forward_mobile.glsl`
  (`batch_instance_index`), `shader_rd` `p_format`, staging-unmap trait guard,
  `safe_name`, JS `config.js`/`engine.js` device creation).
- Two merge artifacts fixed by hand: duplicate
  `is_force_omni_dual_paraboloid()` in `light_storage.h`, duplicate
  `OpTypeVectorIdEXT` in `spirv.hpp11`.
- Ported our **BC7/ASTC sRGB `viewFormats`** fix into their driver
  (`_get_srgb_view_format` + `_is_srgb_format`): without it TPS's BC7 textures
  fail with "was not created with the texture view format (BC7RGBAUnormSrgb)"
  and cascade into 2500+ validation errors (invalid views → bind groups →
  command buffers).
- `test_project`: smoke test PASS (60 FPS, 12 draws, 0 GPU/other errors),
  screenshot correct.
- TPS: 0 GPU validation errors, renders; `[PERF]` shows their draw path working
  (`FI/f=2114` firstInstance, draws/f 3,215, SetBG 657).

Measured trade-off on TPS (komm64 vs our driver + batching + diff-flush):

| metric | ours | komm64 |
|---|---|---|
| load long tasks | 20.9 + 17.7 + 66 s | 18.2 + 63.2 + 14.6 s |
| gameplay hitches | 2.4 s / 1.7 s | 3.1 s / 3.4 s |
| steady fps | 59-60 | 38-59 |
| draws/frame | 3,556 | 3,215 |
| firstInstance draws/frame | 0 (disabled) | 2,114 |
| shader modules | 1,084 (baked table: 563 hits) | 1,015 (`baked=88`, `tint=450`, `precompiled=8`) |
| specialised modules | none (WGSL overrides, `d8cb1fd7`) | **690 / 21.6 s** |
| render pipelines | 361, ~3 ms JS | 361, **36.9 s** wasm |

So their draw-side IPC work is real (`firstInstance` in use, fewer bound
commands), but our port's `d8cb1fd7` win (spec constants as WGSL `override`s, one
module per variant) and our baked-WGSL coverage are missing there: 21.6 s of
specialised module creation + 36.9 s of pipeline creation dominate the load.

Next steps, in order:
1. Port the WGSL-override specialisation approach into their driver (their
   `spec_modules` path) and re-measure.
2. Investigate why only `baked=88` stages came from the shader cache (their
   container bake) vs our 563 hits - possibly bake coverage or host Tint failures.
3. Re-check MSAA flicker and LightmapGI on the swapped build.

## Reference builds of komm64/4.7 (for comparison testing)

Built from a clean clone at `/Users/orangetek/komm64-godot-webgpu`
(`ce9595d`, `4.7.2.stable`) with one local patch: the same Tint
`std::from_chars` -> `strtod` fix as our tree (macOS libc++ gates float
`from_chars` at macOS 26; the editor targets 11.0).

Editor:

```
scons platform=macos arch=arm64 target=editor \
  vulkan_sdk_path=$HOME/fast_godot_toolchain/moltenvk \
  tests=no debug_symbols=no angle=no production=yes
```

`/Users/orangetek/komm64-godot-webgpu/bin/godot.macos.editor.arm64`
(`4.7.2.stable.custom_build.ce9595df8`, 136 MB; built with ccache hits from our
tree, ~3 min).

Web template (the flags their CI uses, release variant):

```
EMSDK=$HOME/fast_godot_toolchain/emsdk EM_CACHE=<fast-godot>/out/emcache \
scons platform=web target=template_release arch=wasm32 threads=no \
  dlink_enabled=yes webgpu=yes opengl3=no tests=no debug_symbols=no \
  production=yes
```

`bin/godot.web.template_release.wasm32.nothreads.dlink.zip` (14 MB), installed as
`~/Library/Application Support/Godot/export_templates/4.7.2.stable/`:

- `web_nothreads_release.zip` (what a preset with `variant/extensions_support=false` looks up)
- `web_dlink_nothreads_release.zip` (the real name for extension presets)

Verified: exporting their `webgpu_tests/test_project` with this editor uses the
template and boots as `WebGPU 1.0 - Forward Mobile`
(`renderingDriver":"webgpu"`). Note their export plugin only emits `webgpu` when
the project sets `rendering/renderer/rendering_method.web` to `mobile` or
`forward_plus`; without it the export falls back to `opengl3`.

Only the **release** template was built; `template_debug` would be the same
command with `target=template_debug dev_mode=yes`.

## davtamay/Godot_WebGPU reference build (4.8.0-beta, `webgpu-stack` @ a09020c)

Their web runtime has **no shader compiler**: shaders must be baked to WGSL at
export time by an external `tint` (only Linux/Windows binaries are published).
Local macOS toolchain built for testing:

- Editor app: `~/davtamay-godot-webgpu/out/editor-staging/Davtamay_Godot.app`
  (`4.8.beta.custom_build.a09020c12`, ad-hoc signed). Build needs the Python
  `zstandard` module -> used an isolated venv `~/davtamay-venv`.
- Web template: built with **emsdk 6.0.12** (`~/emsdk-6`) and their pinned
  emdawnwebgpu remote port, flags
  `platform=web target=template_release threads=no webgpu=yes
  use_closure_compiler=no`; installed as
  `~/Library/Application Support/Godot/export_templates/4.8.beta/web_nothreads_release.zip`
  (the dir is `4.8.beta`, not `4.8.beta.custom_build`).
- Tint for macOS: built from `google/dawn` @ `v20250531.224602` (the ref their
  bake CI pins) with CMake, tint cmd tools only:
  `cmake -S . -B out -DDAWN_FETCH_DEPENDENCIES=ON -DTINT_BUILD_CMD_TOOLS=ON
  -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_WRITER=ON ...` then
  `cmake --build out --target tint_cmd_tint_cmd`.
  Binary at `~/dawn-tint/out/tint`, installed beside the editor
  (`Contents/MacOS/tint`, which their baker auto-locates) and in
  `~/davtamay-godot-webgpu/bin/tint`. `GODOT_TINT_PATH` also works.
  The optional `--allow-non-uniform-subgroup-operations` patch (needed only for
  the secondary subgroups translator, `GODOT_TINT_PATH_SUBGROUPS`) does not
  apply at that Dawn rev; normal baking does not use it.

Verified end-to-end with their `misc/webgpu_scripts/rendering_fixture`:
windowed export (baker needs a real renderer, headless skips it) produced 107
`.webgpu.cache` entries and the build boots as `WebGPU 1.0 - Forward Mobile`
with **0 errors**. Gotchas: the web rendering method must be **mobile**
(forward_plus needs cluster shaders the bake excludes and fails at runtime with
"missing from the baked shader cache"), and a few variants are expected bake
exclusions (their census: TonemapMobile x2, Octmap x6).
