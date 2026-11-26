# Fast Godot WebGPU handoff — 4.8-beta5 port

Resume doc. Last updated 2026-11-26 ~14:20.

## What this repository is

The repo is a **patch stack**, not a Godot fork checkout:

- ~~`patches/4.7.2`~~ — the old 4.7.2 stack, **dropped 2026-11-26**: the project is
  consolidated on the 4.8 stack, and the 4.7.2 branch is no longer built,
  tested or maintained (kept only for history).
- `patches/4.8-beta5` — the 4.8 stack, same layers ported onto godot `master`
  @ `67421f36e5` (4.8.0-beta).
- `tools/apply.sh <godot-checkout> <stack>` applies a stack; `tools/make-patches.sh`
  regenerates it from a modified checkout. Patches are generated with
  `git diff --binary` (icons/splash/logo/SPIR-V fixtures are binary patches).
- `upstream.lock` pins the Godot base per branch.

| ref | commit | notes |
|---|---|---|
| `refs/heads/patches/4.7.2` | `f067ebc1d2` | frozen/deprecated (4.7.2 dropped) |
| `refs/heads/patches/4.8-beta5` | latest | base godot master 67421f36e5 |
| `refs/tags/4.8-beta5` | same as branch | |
| release `4.8-beta5-source` | `fast-godot-4.8-beta5-src.zip` | patched 4.8 source tree used by the Grit toolchain bootstrap |

## 4.8 layer map

1. `0001-modules-grit` — Fast/Grit AOT compiler (adapted to 4.8 GDScript
   internals: GDType member maps, const MethodBind, parser resolved_type).
2. `0002-modules-cffi`.
3. `0003-web-html5-fast-export` — HTML5 Fast platform + web hooks.
4. `0004-webgpu-driver` — komm64/4.7 WebGPU driver rebased to 4.8
   (`buffer_flush(used_size)`, `buffer_map_ready`, `buffer_create_with_data`,
   readback/upload conversion, layered copy/init, per-stage SPIR-V version).
5. `0005-engine-renderer-patches` — batching (`batch_instance_index`), shader
   container format override, driver traits, GDScript native-call hooks, export
   window, WGSL shader baker plugin, icons/splash, frame profiler.
6. `0006-thirdparty-vendored` — Tint + SPIRV-Tools.
7. `0007-tools-fixtures-docs` — tools/, toolchain, test_project, docs.
8. `0099-misc`.

## Build / toolchain

## Single toolchain

There is exactly **one toolchain** for the whole project (4.7.2 was dropped):

- Path: `~/fast_godot_toolchain` (~4.3 GB), provisioned by
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
  3.9 and emcc refuses it.

Editor (macOS, from a checkout + applied stack):
```
scons platform=macos arch=arm64 target=editor \
  vulkan_sdk_path=$HOME/fast_godot_toolchain/moltenvk \
  tests=no debug_symbols=no angle=no production=yes
```
The current 4.8 build tree is a **volatile git worktree at `/tmp/godot48`**
(branch `port48`, HEAD `f578f89fa9`, not pushed — it descends from the Godot
history which the repo does not carry). If /tmp is wiped, rebuild by applying
`patches/4.8-beta5` to a fresh Godot checkout.

Staged build (gitignored): `out/editor-staging/`
- `Fast_Godot_4.8.app` (`4.8.beta.custom_build.f578f89fa`, Fast Godot icon,
  ad-hoc signed) and `Fast_Godot_4.8-macos.zip`
- `godot.web.template_release.wasm32.nothreads.zip`
- `Fast_Godot_4.8_BUILD.txt` (version/time/flags)
Launchable install: `~/Applications/Fast_Godot_4.8.app`.
Note: the app folder's mtime can read like the template's old timestamp — use
`Contents/MacOS/Godot` mtime/version.

## Export flow (HTML5 Fast)

Preset platform `HTML5 Fast`, `shader_baker/enabled=true`. Export from a
**windowed** editor (the baker needs a renderer):
```
GRIT_JOBS=6 Fast_Godot_4.8.app/Contents/MacOS/Godot --path <project> \
  --rendering-method mobile --rendering-driver vulkan \
  --export-release "HTML5 Fast" "<site>/index.html"
```
Exports go straight to the Godot Web Server site folder:
`~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/`.
Results: test_project cold export 5m22s, warm 12s (SCons object cache + host
tint-cli cache); TPS demo export 2m37s, boots at 59-60 FPS with 0 GPU errors.

Grit build performance/cancel fixes in the 4.8 stack:
- host `tint_convert_cli` cached under `$toolchain/tint-cli-cache`
  (~/.cache/fast-godot/tint-cli fallback), serialized by a lock, capped to 6
  jobs, -O1;
- SCons objects shared across profile trees via `cache_path`;
- cancel (Esc) kills the whole process group, so compilers stop too.

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

Fix: ring grows to 8 MB (32768 slots, ~15x headroom), plus a one-shot warning
if an extreme frame ever overflows. Verified with the orbit capture: before,
2-10 black frames per 48; after, none and `RingOF=0`. User-verified in a real
browser on the TPS demo with MSAA 4x.

Residual risk: the ring flush path itself is still the weak point (a frame with
>32k push-constant slots would overflow). A proper fix would make the flush not
interrupt in-pass resolves (e.g. defer flush to pass boundaries, or use a
per-frame growing ring), and is the next hardening step.

Diagnostics kept in the driver: `rendering/webgpu/debug_frames` prints per-frame
pass/draw/resolve/ring counters to the browser console;
`rendering/webgpu/disable_batching` disables the batch/firstInstance traits and
was used to rule the batching path out.

## Forward+ status (studio-foundation comparison, 2026-11-26)

The studio-foundation 4.7.1 stack (patches 0001-0033) was built locally and used
to export TPS in Forward+ for comparison. It does **not** run TPS:

- Tint crashes with `TINT_UNIMPLEMENTED unhandled SPIR-V instruction:
  OpImageTexelPointer` (image atomics) and the wasm traps.
- Forward+ scene pipelines exceed WebGPU limits: "number of storage buffers
  (12) in the Vertex stage exceeds the maximum per-stage limit (10)",
  so `SceneForwardClusteredShaderRD` layout/pipelines are invalid.
- MSAA resolve bindings mismatch: `sampler2DMS` texture-vs-sampler in
  `ResolveShaderRD`/`SsEffectsDownsample`, and a sample-count-4 texture bound to
  a `multisampled: 0` layout entry.
- Their mobile export boots (60 fps) but renders only the orange environment,
  because their shader bake does not cover TPS's scene-embedded materials.

Conclusion: Forward+ on their stack needs engine work (reduce vertex storage
bindings below 10, image-atomic lowering/fallbacks, resolve layout fixes) and a
different bake path before it can host TPS; our 4.8 mobile+Grit stack remains
the working option.

## Open issues / next steps

1. ~~**MSAA flicker**~~ — **RESOLVED** (see the section below). Root cause was
   the WebGPU push-constant ring overflowing mid-frame; the flush broke the
   in-pass MSAA resolve. Fixed by an 8 MB ring (plus a one-shot warning).
2. **LightmapGI/GI probes**: komm64's 4.7.2 build had them working; the 4.8
   port has not been checked.
3. **Perf comparison**: 4.8 TPS stats - shaders=168/607/247ms, tint=370/13.7s,
   baked=280, spec_modules=788/27.5s, render_pipelines=409/34s,
   draws/frame ~1750, fps 60. Compare against the 4.7.2 stack.
4. **White-box pack key**: not ported (shim in `core/grit_script_key.cpp`);
   encrypted web templates are unavailable. `FileAccessEncrypted` v2 chunk
   format was reverted to the 4.8 base.
5. The **4.7.2 stack is dropped** (2026-11-26): one toolchain, one engine line.
   No fixes will be mirrored there; `patches/4.7.2` is frozen for history only.
6. Bake-time noise: `Index p_pass out of bounds (draw_passes.size() = 1)` for
   particle shaders and a few leaked particle shader RIDs at editor exit; the
   exported build is unaffected so far.
7. `~/notarize-work/Fast_Godot.app` is an old 4.7.2 Developer-ID build with the
   stock icon; a Dock tile pinned to it shows the old icon. Either re-pin
   `~/Applications/Fast_Godot_4.8.app` or re-sign a refreshed copy.

## Practical notes

- Never run two exports concurrently (they used to stampede the machine; now
  serialized by the tint-cli lock, but still).
- Building/staging inside Dropbox triggers FileProvider/Spotlight churn; keep
  build trees outside Dropbox (`/tmp` or `~`), stage the final app only.
- Tag pushes on `patches/*` were force-pushed historically (branch renamed
  from `4.7.2`/`4.8-beta5`); `.gitignore` on those branches must exclude `out/`
  or a 100 MB app binary gets committed and GitHub rejects the push.
- One SDK (Emscripten 6.0.12) serves everything; do not pin a second version.
- The macOS editor build tree `/tmp/godot48` also carries the ring fix; re-apply
  `patches/4.8-beta5` to a fresh checkout to reproduce it.
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
