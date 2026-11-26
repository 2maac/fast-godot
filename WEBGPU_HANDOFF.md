# Fast Godot WebGPU handoff — 4.8-beta5 port

Resume doc. Last updated 2026-11-26 ~14:20.

## What this repository is

The repo is a **patch stack**, not a Godot fork checkout:

- `patches/4.7.2` — the 4.7.2-stack (komm64/4.7 WebGPU driver + engine patches,
  grit, cffi, HTML5 Fast) applied on `4.7.2-stable` (`ed1daf0bf0`).
- `patches/4.8-beta5` — the 4.8 stack, same layers ported onto godot `master`
  @ `67421f36e5` (4.8.0-beta).
- `tools/apply.sh <godot-checkout> <stack>` applies a stack; `tools/make-patches.sh`
  regenerates it from a modified checkout. Patches are generated with
  `git diff --binary` (icons/splash/logo/SPIR-V fixtures are binary patches).
- `upstream.lock` pins the Godot base per branch.

| ref | commit | notes |
|---|---|---|
| `refs/heads/patches/4.7.2` | `f067ebc1d2` | base 4.7.2-stable |
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

Toolchain (re-provisioned by our own script; ~4.5 GB):
`~/fast_godot_toolchain` — Python 3.12.16 (+zstandard 0.25.0), SCons 4.11.1,
MoltenVK 1.4.2, **Emscripten 6.0.12** (4.8 requires >= 6.0.1; 4.7.2 branch
stays on 4.0.11). `tools/toolchain/download_toolchain.sh` is the provisioner;
the editor runs it automatically, and its engine-source URL must point at the
4.8 source release asset (already fixed; non-4.8 trees are rejected by
`version.py` minor check).

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

## Open issues / next steps

1. **MSAA flicker (4.7.2 only so far)**: 4.7.2 builds flicker at MSAA 2x/4x in
   gameplay (whole-scene lit/flat toggling, no GPU errors); menu and MSAA-off
   are stable. Not yet verified on the 4.8 build — test TPS with MSAA 4x.
   Related findings: 4.8 has no `first_instance`/`batch_instance_draws` draw
   path (removed upstream), subpass post-process is already disabled on web.
2. **LightmapGI/GI probes**: komm64's 4.7.2 build had them working; the 4.8
   port has not been checked.
3. **Perf comparison**: 4.8 TPS stats - shaders=168/607/247ms, tint=370/13.7s,
   baked=280, spec_modules=788/27.5s, render_pipelines=409/34s,
   draws/frame ~1750, fps 60. Compare against the 4.7.2 stack.
4. **White-box pack key**: not ported (shim in `core/grit_script_key.cpp`);
   encrypted web templates are unavailable. `FileAccessEncrypted` v2 chunk
   format was reverted to the 4.8 base.
5. The **4.7.2 branch lacks the Grit caching/cancel fixes**; mirror them if
   4.7.2 exports are still in use.
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
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
