# Fast Godot WebGPU handoff — 4.8-rc1 port

Resume doc. Last updated 2026-11-28 ~13:38. **The stack is rebased onto Godot
4.8-rc1** — upstream `godotengine/godot` master **`7c97c29779`** ("Bump version
to 4.8-rc", `version.py` status `rc`), 86 commits ahead of the previous
4.8.0-beta pin `67421f36e5`. Upstream has **no git tag named `4.8-rc1`** (its
tags stop at `4.7.2-stable`), so "4.8-rc1" is our name for that master rc-state
commit. The repo is **tag-free**: **all tags were deleted on 2026-11-28**, and
the two branches are **`4.8-rc1`** (current stack, repo default since
2026-11-28) and **`4.7.2`** (history only). The bootstrap therefore fetches
the patch-stack archive from the **branch**
(`…/archive/refs/heads/4.8-rc1.tar.gz`; patch `0001`). The old `4.8-beta5`
branch and tag were removed the same day — their commits survive as ancestors
of `4.8-rc1` (last beta5 tag `a7a5aefbe2`, branch tip `5da498f468f`).
**Forward+ and Forward Mobile both
run on the WebGPU device, SDFGI is enabled on the WebGPU device, and
pmndrs/upscaler milestone 1 (spatial EASU+RCAS) is implemented and
browser-verified as a first-class Viewport scaling mode (mode 6).** The
pmndrs/upscaler **milestone 2 (temporal path) was attempted and then fully
reverted** (2026-11-28): the owner's browser test showed severe ghosting, the
temporal code was removed (not just the UI), and the stack is back to the
spatial-only mode-6 state plus the SDFGI layer-completeness fix. The two
temporal research docs are kept as archived research, clearly marked
**IMPLEMENTED THEN REVERTED — NOT SHIPPED**. **Web platform (2026-11-28):
occlusion culling is compiled into the web/WebGPU template (raycast/Embree
override in patch `0003`), and HTML5 `threads=yes` is FIXED and browser-
verified (opt-in: it needs COOP/COEP hosting, so the shipped TPS preset keeps
`variant/thread_support=false`).**

Repo state: the 4.8-rc1 rebase is **stack commit `76d82e9993`**
(it is the commit that carries the regenerated
`patches/` + `patches/upstream.lock` re-pin), with a docs commit above it.
When the lead pushes, branch **`4.8-rc1`** carries that stack commit. Fresh
toolchain installs fetch the patch-stack archive from that **branch**
(`refs/heads/4.8-rc1`; the bootstrap URL in patch `0001` names it) — the
repository has **no tags at all** since 2026-11-28. The old `4.8-beta5`
branch and tag were removed the same day (last tag `a7a5aefbe2`, the threads
fix; branch tip `5da498f468f`; their commits are ancestors of `4.8-rc1`).
Verify with `git ls-remote origin refs/heads/4.8-rc1`.
`patches/` is a self-contained portable bundle (patch files + `layers/` +
`upstream.lock`); the volatile build worktree is `/tmp/godot48-rc1` (detached
at upstream `7c97c297793`, with the whole stack applied as working-tree
changes — the tree itself is not committed; the old `/tmp/godot48` beta
worktree is retained as history). Editor version is now
`4.8.rc.custom_build.7c97c2977`. This file lives in `docs/` together with
`PATCH_STACK.md`, `WGSL_COMPUTE_PATH.md`, `PMNDRS_UPSCALER_SPATIAL.md`, the two
archived temporal research docs (`PMNDRS_TEMPORAL_PORT.md`,
`GODOT_TEMPORAL_UPSCALER_INTEGRATION.md` — NOT SHIPPED) and the archived
migration history.

Current state in one screen:

- **Godot 4.8-rc1 rebase (2026-11-28)**: stack applied to upstream master
  `7c97c29779` (86 commits above the old beta pin), editor
  `4.8.rc.custom_build.7c97c2977`. Verified: default rc1 TPS Forward+ WebGPU
  59-60 fps VSync (draws/f ≈5.5-5.6k), pmndrs spatial active, no occlusion
  warning, 0 GPU validation errors, 0 shader errors; threaded rc1 also clean
  (0 GPU/shader/worker errors), uncapped threaded 131.3 fps. Branch/tag
  `4.8-rc1` created with the rebase; see the rc1 section and the environment
  snapshot.
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
- **Web/WebGPU occlusion culling is ENABLED** (2026-11-28; in the working
  `patches/0003`, committed next): upstream's web `detect.py` disables the
  raycast/Embree module
  (GH-70621), so web builds used the stub and printed "Occlusion culling is
  disabled at build-time"; the Fast Godot stack overrides
  `module_raycast_enabled: True`, so TPS's `use_occlusion_culling=true` +
  baked occluders now run the real CPU culling path. Cost: +1.18 MB wasm
  (template wasm 50,471,223 → 51,648,024 B) plus Embree's memory; no TPS win
  measured (the TPS test camera never has an instance fully covered by the
  baked occluder). See the web-platform section below.
- **HTML5 `threads=yes` is FIXED and browser-verified (2026-11-28), opt-in.**
  Threaded web templates now boot and play the TPS level at 60 fps VSync with 0
  GPU/shader/worker errors (uncapped 137.3 fps vs 124 fps single-threaded).
  Two fixes: upstream pthread flags restored in `platform/web/detect.py`
  (`-pthread` + `PTHREAD_POOL_SIZE=Module['emscriptenPoolSize']||8` +
  `MAXIMUM_MEMORY=2048MB`; the fork's legacy `-sUSE_PTHREADS`/pool=0 scheme
  caused the startup wedge), and `can_create_resources_async()` returns false
  under `WEBGPU_ENABLED` so threaded resource creation is queued to the
  render/main thread instead of calling emdawnwebgpu off-thread
  (`createTexture of undefined`). Threaded builds need COOP `same-origin` +
  COEP `require-corp` hosting, so the shipped preset keeps
  `variant/thread_support=false`; threading is an opt-in preset option. See the
  web-platform section.
- The Godot Web Server site export is regenerated after each build (last
  written 2026-11-28 12:36:00, the 4.8-rc1 default nothreads build; wasm
  sha256 `f28e7f77…`); browsers need a hard reload to pick it up. The threaded
  rc1 build is opt-in and is not published to the default site.

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
the same base commit plus working-tree changes, so the editor version stayed
`4.8.beta.custom_build.582d61902` (then; the current rc1 build is
`4.8.rc.custom_build.7c97c2977`); see "Forward+ on WebGPU (2026-11-27)" below
for what changed and the remaining limitations.

## Where to start (docs map)

| doc | status |
|---|---|
| `README.md` | repo mechanics (patch stack, tools, rules) |
| **this file** | current state (stack rebased to **Godot 4.8-rc1**): toolchain, build/export, Forward+ (incl. MSAA/TAA/SDFGI), pmndrs spatial upscaler (mode 6), web occlusion culling, HTML5 threads (fixed, opt-in), browser verification, MSAA/ring fixes, encryption, temporal-attempt history, open work |
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
- The 4.8 stack (branch `4.8-rc1`; the repo is tag-free — the previous
  `4.8-beta5` branch/tag are **removed** and history is kept through
  `4.8-rc1` ancestry) is a
  **self-contained portable bundle in
  `patches/`**: patch files (`0001-…0099-*.patch`), the `layers/*.files`
  regeneration lists and `upstream.lock` (the Godot pin) all live there;
  ported onto godot `master` @ `7c97c29779` (4.8-rc1 rc state; no upstream git
  tag for it). Copying `patches/` + `tools/apply.sh` reproduces the stack
  elsewhere. Only one active stack exists.
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
| branch `4.8-rc1` = `origin/4.8-rc1` | 4.8-rc1 stack commit `76d82e9993` | **current stack branch** (a docs commit sits above it) and the **only stack ref** — the repo is tag-free since 2026-11-28, so fresh toolchain installs fetch the stack archive from this branch (`refs/heads/4.8-rc1`; patch `0001`'s provisioner names it). Base: upstream master `7c97c29779` (4.8-rc1 rc state). Also the repo default branch (switched 2026-11-28) |
| ~~branch + tag `4.8-beta5`~~ — **both removed 2026-11-28** | was branch `5da498f468f`, tag `a7a5aefbe2` | previous stack refs (branch + lightweight tag sharing one name), deleted locally and on `origin` on 2026-11-28 because the beta5 stack is fully superseded by `4.8-rc1`. All stack, bootstrap and doc references now point at **`4.8-rc1`**. Their commits are **ancestors of `4.8-rc1`**, so nothing was lost: the threads fix `a7a5aefbe2` (last tag value) plus docs (`ed3dbf540e9`, `5da498f468f`). Earlier history: temporal revert, `cb0aa639c7` (superseded), `91a0a3e8e4` spatial mode 6, `d8b3d583ae` raw-WGSL + pmndrs spatial. Was the repo default branch until 2026-11-28 |
| `origin/4.7.2` | `d8cb1fd77c` | full-source **Godot 4.7.2** snapshot (single parentless commit), history only, not maintained |
| ~~tag `4.8-beta5-source`~~ | — | **release + tag removed 2026-11-26.** It held a parentless 4.8 source snapshot (`35e42f7d`, re-cut from the wrong 4.7.2 tree earlier that day) and the `fast-godot-4.8-beta5-src.zip` asset. Editors built before 2026-11-26 hardcoded that asset URL for first-use provisioning and can no longer bootstrap a fresh toolchain. New builds use Godot's repo + the `4.8-rc1` patch tag |

**Toolchain bootstrap (2026-11-26) no longer uses a source release.** New editor
builds download the patch stack **branch** archive (~8 MB), read `godot_commit` from its
`patches/upstream.lock`, download Godot's own source at that commit from
`godotengine/godot` (~76 MB), apply the stack's layers with `git apply`, and move
the result into `<toolchain>/engine/source` — see the Toolchain bootstrap
subsection under Build / toolchain. The old release asset and its tag were
**removed 2026-11-26**: editors built before that date hardcoded the asset URL
and can no longer provision a fresh toolchain (exports from machines that
already have a toolchain are unaffected).

## Environment snapshot (verified on this machine, 2026-11-28 ~12:40)

| item | actual value |
|---|---|
| patch stack repo | branch **`4.8-rc1`** = `origin/4.8-rc1` — created/pushed with the 4.8-rc1 rebase (stack commit `76d82e9993`; a docs commit sits above it). The repo is **tag-free** since 2026-11-28 (all tags deleted), so fresh toolchain installs fetch the stack archive from the **`4.8-rc1` branch** (`…/refs/heads/4.8-rc1.tar.gz`; patch `0001`'s provisioner names it). The old **`4.8-beta5` branch and tag were both removed 2026-11-28** (last tag `a7a5aefbe2`; branch tip `5da498f468f`; their commits are ancestors of `4.8-rc1`, so nothing was lost); the repo default branch is now `4.8-rc1` (switched 2026-11-28). Base/`upstream.lock`: upstream master **`7c97c29779`** (4.8-rc1 rc state; upstream has no git tag `4.8-rc1` — that is our name). Patch sizes: `0001` **508,875 B**, `0002` 203,698 B, `0003` 46,094 B, `0004` **893,686 B**, `0005` 5,941,786 B, `0006` 15,133,768 B, `0007` 3,541,650 B, `0099` 29,738 B; 1,790 layer paths total (`0004` 34, `0005` 160) |
| engine base | Godot `master` @ **`7c97c29779`** ("Bump version to 4.8-rc"; `version.py` major 4 / minor 8 / patch 0 / status `"rc"`), pinned in `patches/upstream.lock` — **86 commits ahead** of the previous 4.8.0-beta pin `67421f36e5`. Upstream tags stop at `4.7.2-stable`: there is no `4.8-rc1` git tag upstream, so "4.8-rc1" is our name for this master rc-state commit |
| object model | this repo is a **partial clone** of `godotengine/godot` with `blob:none` promisor packs, so Godot-history blobs are fetched from upstream on demand. `port48` carries 87,467 commits not on `origin`, so full-source refs are pushed as single parentless snapshots instead (as `origin/4.7.2` is) |
| build worktree | **`/tmp/godot48-rc1`** — detached at upstream `7c97c297793` with the whole stack applied as working-tree changes (**1,790 changed paths = the 1,790 layer-listed paths**; 161 modified / 1,615 added / 12 deleted / 2 added-then-modified at the index level after `git add`). The rebase was applied with `git apply --3way`; the patch stack was regenerated from it 2026-11-28 ~12:30, so a fresh checkout of `7c97c29779` + `tools/apply.sh` reproduces it. The old `/tmp/godot48` beta worktree (`port48` @ `582d619026`) is retained as history |
| patch round-trip | 2026-11-28 ~12:31: applying the full regenerated rc1 stack to a pristine checkout of `7c97c29779` (`/tmp/applytest-rc1`, all **1,790** layer paths staged) and re-diffing each layer with its `patches/layers/*.files` path list reproduces **all 8 `patches/*.patch` files byte-identically** (independently re-checked; sizes as in the row above). The rebase had exactly **one conflict**, in `servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl`: resolved by keeping our `batch_instance_index` rename and taking rc1's new `.xyz` on `prev_data.inv_view_precision` / `data.inv_view_precision` (rc1 made those precision-aware vec4 accesses); every other stack file applied cleanly |
| layer completeness | all **1,790/1,790** changed paths are covered by the union of `patches/layers/*.files` (the worktree status set exactly matches the layer union after the rebase). `0004`'s layer list gained `tint_cli/core/os/os.h` and `tint_cli/core/string/ustring.h` (**34** paths, was 32). The per-layer round-trip check only diffs the *listed* paths, so still cross-check `git status` against the union |
| other worktrees | `/tmp/godot48` (old 4.8-beta5 build tree, history) and `/tmp/ref472` (detached `d8cb1fd77c`, 4.7.2 reference only) |
| toolchain bootstrap | downloads `godotengine/godot` @ `7c97c29779` (~76 MB) + the `fast-godot` **`4.8-rc1` branch** archive (~8 MB) and applies the stack with the system **git**; `GRIT_ENGINE_PROBE=1` runs just this step headlessly. Patch `0001`'s provisioner `PATCH_STACK_REF` / `PATCH_STACK_ARCHIVE_URL` now name `4.8-rc1` (was `4.8-beta5`) |
| toolchain (single SDK) | `~/fast_godot_toolchain`, 6.4 GB: Emscripten 6.0.12, Python 3.12.16 (+ zstandard 0.25.0), SCons 4.11.1, MoltenVK 1.4.2, `engine/source` = 4.8-rc1 + the current stack, **re-synced for the rc1 builds, then for the tag-free bootstrap at 2026-11-28 13:24** (`version.py` on disk 11:57:40 with status `rc`; the tint_cli shims on disk 12:14:26; the branch-URL provisioner file on disk 13:24) |
| staged editor | `out/editor-staging/Fast_Godot_4.8.app` = **`4.8.rc.custom_build.7c97c2977`**, **rebuilt from the rc1 tree 2026-11-28 13:24:38** (`/tmp/editor_tagfree_build.log`: `scons: done building targets.`, 6.96 s) so the embedded provisioner fetches the branch archive; `Contents/MacOS/Godot` **111,257,056 B**, sha256 **`9499ffae…`** (= `9499ffaebb253bbc…`). `Fast_Godot_4.8-macos.zip` written 13:25:26 (58,382,609 B, sha256 `381b4c3c…`) |
| installed editor | `~/Applications/Fast_Godot_4.8.app` is **updated to the same rebuilt binary** (sha256 `9499ffae…`; boots `4.8.rc.custom_build.7c97c2977`) |
| web template | **Nothreads (default):** `out/editor-staging/godot.web.template_release.wasm32.nothreads.zip` written 12:36:01, **14,358,369 B**, zip sha256 **`c61caa74…`**; `godot.wasm` **51,648,673 B**, sha256 **`f28e7f77…`** (the wasm the site runs). **Threaded (opt-in):** `out/editor-staging/godot.web.template_release.wasm32.zip` written 12:37:49, **14,357,917 B**, zip sha256 **`f80d59e6…`**; its TPS export wasm is 51,133,278 B (`/tmp/site-rc1-final-threaded`). Both contain 12 `RaycastOcclusionCull` strings, 12 pmndrs strings (spatial mode 6), no temporal code, and no `[WGPU-DBG]` instrumentation strings |
| export test site | `~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/` — default **nothreads** TPS HTML5 Fast Forward+ export published 2026-11-28 **12:36:00** (`/tmp/export_rc1_default_final3.log`): `index.wasm` 51,648,673 B sha256 **`f28e7f77…`**, `index.pck` 987,358,944 B. Runtime check (`/tmp/rc1_final_default_console.log`): `Godot Engine v4.8.rc.custom_build.7c97c2977`, 59-60 fps VSync, draws/f ≈5.5-5.6k, pmndrs spatial active, **no** "Occlusion culling is disabled at build-time", 0 GPU validation errors, 0 shader errors. The **threaded** rc1 export (`/tmp/site-rc1-final-threaded`, wasm 51,133,278 B) was served with COOP/COEP on `127.0.0.1:8225` (`/tmp/rc1_threaded_final_server.log`): 59-60 fps, draws/f ≈5.2-5.3k, 0 GPU validation/shader/worker errors, no occlusion warning (`/tmp/rc1_final_threaded_console.log`); uncapped threaded **131.3 fps** (`/tmp/rc1_threaded_uncap2_console.log`; the previous beta stack measured 137.3 threaded vs 124 single-threaded). The default rc1 uncapped figure is **112 fps** (`/tmp/rc1_default_uncap_console.log`) |
| build record | `out/editor-staging/Fast_Godot_4.8_BUILD.txt` is **refreshed for the tag-free rebuild** (mtime 2026-11-28 13:25:32, 8,505 B): `Fast Godot 4.8-rc1 build`, editor `4.8.rc.custom_build.7c97c2977`, editor sha256 `9499ffae…`, base `7c97c29779`, branch-archive bootstrap, both template zip hashes, site wasm `f28e7f77…`; it is not part of `patches/` |
| source release | **removed 2026-11-26** (was tag `4.8-beta5-source` = `35e42f7d` + `fast-godot-4.8-beta5-src.zip`). New builds bootstrap from Godot's repo + the current patch **branch** (`4.8-rc1`; all tags are gone since 2026-11-28); editors built before the removal can no longer provision a fresh toolchain |

## 4.8 layer map

1. `0001-modules-grit` — Fast/Grit AOT compiler (adapted to 4.8 GDScript
   internals: GDType member maps, const MethodBind, parser resolved_type).
   **4.8-rc1 rebase + tag-free bootstrap (2026-11-28):** the Grit provisioner
   uses `PATCH_STACK_REF = "4.8-rc1"` and
   `PATCH_STACK_ARCHIVE_URL = …/archive/refs/heads/4.8-rc1.tar.gz` (the
   **branch** archive; all tags were deleted 2026-11-28), so fresh toolchain
   boots fetch the rc1 branch tip.
2. `0002-modules-cffi`.
3. `0003-web-html5-fast-export` — HTML5 Fast platform + web hooks. **Web
   occlusion culling enabled (2026-11-28)**: overrides upstream's
   `platform/web/detect.py` default `module_raycast_enabled: False` (Embree is
   heavy — GH-70621) to `True`, so the `raycast` module (CPU Embree) is compiled
   into web/WebGPU templates and
   `rendering/occlusion_culling/use_occlusion_culling` is honoured instead of
   hitting the stub. Embree is vendored in-tree (`thirdparty/embree`, 337
   tracked files) and wasm32 builds it with SIMD; cost ≈1.18 MB extra wasm plus
   Embree's memory. Remove the override to return to upstream's
   memory-optimized default. **HTML5 threads=yes fixed (2026-11-28)**: the
   `env["threads"]` block was restored to upstream's configuration (`-pthread`
   in CCFLAGS/LINKFLAGS, `-sPTHREAD_POOL_SIZE="Module['emscriptenPoolSize']||8"`,
   `-sMAXIMUM_MEMORY=2048MB`, plus the existing `PTHREAD_NO_RENAME` define and
   DEFAULT_PTHREAD_STACK_SIZE); the fork's legacy `-sUSE_PTHREADS=1` +
   `PTHREAD_POOL_SIZE=0` scheme had caused the threaded-startup wedge.
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
   here; **both were reverted with milestone 2**.) **4.8-rc1 rebase
   (2026-11-28): added the standalone host-tool shims
   `drivers/webgpu/tint_cli/core/os/os.h` and
   `drivers/webgpu/tint_cli/core/string/ustring.h` (the pre-existing
   `core/templates/vector.h` shim now includes the ustring shim), fixing a
   latent fresh-build failure of `tint_convert_cli`: `spirv_preprocess.cpp`
   includes `core/os/os.h`/`core/string/ustring.h` and `spirv_preprocess.h`
   uses `String`, which the shim tree did not provide — old trees only worked
   because `bin/tint_convert_cli` was prebuilt. `0004` is now **893,686 B**
   and its layer list is **34** paths (was 32).
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
   **HTML5 threads fix (2026-11-28):**
   `servers/rendering/renderer_rd/renderer_compositor_rd.h`
   `can_create_resources_async()` now returns **false** under
   `#ifdef WEBGPU_ENABLED`, so threaded resource loading queues texture/buffer
   creation through the render-server command queue on the render/main thread
   instead of calling emdawnwebgpu from a pthread (which failed with
   "Cannot read properties of undefined (reading 'createTexture')"); that path
   is now in the `0005` layer list.
   **Layer completeness (kept):** the `0005` list is **160 paths** — 156 base
   paths + the three SDFGI files that had been missing from every layer list
   (`environment/gi.h`, `shaders/environment/gi.glsl`,
   `shaders/environment/sdfgi_integrate.glsl`), added so the stack can
   reproduce the SDFGI build, + `renderer_compositor_rd.h` for the threads fix
   (`0005` is 5,941,474 B for that reason). The M2 temporal effect/mode-7
   entries were removed with the revert.
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

1. download `https://github.com/2maac/fast-godot/archive/refs/heads/4.8-rc1.tar.gz`
   (~8 MB) — the patch stack from the `4.8-rc1` **branch** (the provisioner's
   `PATCH_STACK_ARCHIVE_URL`, patch `0001`; all tags were deleted 2026-11-28);
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
- **the `4.8-rc1` branch is what fresh installs fetch** (since the 2026-11-28
  tag-free switch): the provisioner downloads the branch archive
  (`refs/heads/4.8-rc1`, stack tip `76d82e9993`) via patch `0001`. The old
  `4.8-beta5` branch and tag were removed 2026-11-28 (last tag value
  `a7a5aefbe2`, the threads fix; it was
  moved several times earlier that day: `529407b9fc` SDFGI tip → `91a0a3e8e4`
  spatial mode 6 → `cb0aa639c7` temporal → revert stack). Fresh installs now
  bootstrap the **4.8-rc1** driver/template (base upstream master
  `7c97c29779`) and read the self-contained `patches/` bundle. `4.8-rc1` is
  our branch name: upstream has no such git tag yet (its tags stop at
  `4.7.2-stable`), so the reference resolves only on this repo. Keep the
  branch pointing at the stack you ship; docs commits sit above the stack
  commit, and editor-only changes don't matter for template builds;
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
The current 4.8 build tree is the **volatile git worktree `/tmp/godot48-rc1`**
— detached at upstream `7c97c297793` (4.8-rc1) with the whole stack applied as
working-tree changes (**1,790 changed paths = the 1,790 layer-listed paths**;
the rebase used `git apply --3way` with one resolved shader conflict, see the
rc1 section). The old `/tmp/godot48` worktree (`port48` @ `582d619026`, the
4.8.0-beta stack) is retained as history and is not used by rc1 builds. The
patch stack is the source of truth: regenerate from `/tmp/godot48-rc1`, or
apply the repo's `patches/*.patch` files to a fresh checkout of `7c97c29779`
if `/tmp` is wiped. Ancestors of the beta worktree (historical): bundle-lock
bootstrap `582d619026`, patches/ layout `cdc0907bf6`, ring hardening
`9267990dcd`, source bootstrap `130ed7f48d`, MSAA mitigation `a495fc3e93`,
pack-key port `834c3d4fc2`.

Staged build (gitignored): `out/editor-staging/`
- `Fast_Godot_4.8.app` (**`4.8.rc.custom_build.7c97c2977`**, Fast Godot icon,
  ad-hoc signed; **tag-free rebuild 2026-11-28 13:24:38**, 111,257,056 B,
  sha256 `9499ffae…`) and `Fast_Godot_4.8-macos.zip` (written 13:25:26,
  58,382,609 B, sha256 `381b4c3c…`)
- `godot.web.template_release.wasm32.nothreads.zip` (default rc1 build; written
  2026-11-28 12:36:01, **14,358,369 B**, zip sha256 `c61caa74…`; its
  `godot.wasm` sha256 `f28e7f77…` is the one the site export runs; spatial
  mode 6 + occlusion culling, threads=no, no temporal)
- `godot.web.template_release.wasm32.zip` (**opt-in threaded rc1 build**;
  written 12:37:49, 14,357,917 B, zip sha256 `f80d59e6…`; its TPS export wasm
  is 51,133,278 B in `/tmp/site-rc1-final-threaded`)
- `Fast_Godot_4.8_BUILD.txt` — **refreshed for the tag-free rebuild** at
  13:25:32 (8,505 B): title/version/base, editor sha256 `9499ffae…`, both
  template zip hashes and site wasm `f28e7f77…`
The launchable install `~/Applications/Fast_Godot_4.8.app` is **updated to the
same rebuilt binary** (sha256 `9499ffae…`).
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
`/tmp/node-test/`). The current site export is the **4.8-rc1 default nothreads
build**: exported 2026-11-28 12:36:00 (`/tmp/export_rc1_default_final3.log`) into the
Web Server site folder — wasm 51,648,673 B sha256 `f28e7f77…`, pck 987,358,944 B;
the exported wasm is `Godot Engine v4.8.rc.custom_build.7c97c2977` and contains
the pmndrs spatial path, **no** temporal upscaler, and the real
Embree/RaycastOcclusionCull path. The **threaded rc1** build is opt-in and was
exported separately 2026-11-28 12:25 (`/tmp/export_rc1_threaded.log`) into
`/tmp/site-rc1-final-threaded` (wasm 51,133,278 B) for the verification run; it is
not published to the default site. Earlier exports remain as history: the
beta threads-fix build (2026-11-28 09:55:13, wasm `a3772909…`), the
occlusion-only build (02:15:57, wasm `e50a901b…`), the reverted spatial-only
build (00:54:36, wasm `98ac9e82…`), the M2 temporal build (00:28:52, wasm
`3fab49b3…`), the spatial-mode build (2026-11-27 21:53:27, wasm `3b25ba0b…`)
and the 20:47 bool-setting A/B (`/tmp/site-pmndrs`, `/tmp/site-fsr1`).

### Browser verification recipe (Forward+)

1. Export the TPS project with `--rendering-method forward_plus` (project setting
   `rendering_method.web="forward_plus"`) into the site folder above.
2. Serve it on a **fresh port** (`python3 -m http.server <port> --bind 127.0.0.1`
   from the site dir — never reuse a port that may still have an old server).
3. Open `index.html` in a Chromium with WebGPU (Chrome for Testing or Chrome/
   Brave with `--enable-unsafe-webgpu` if needed) and capture the console.
4. Expect: `Godot Engine v4.8.rc.custom_build.7c97c2977` (rc1 base),
   `WebGPU 1.0 - Forward+ - Using Device #0`, the TPS menu, then the level after
   PLAY; `[PERF] fps≈60 draws/f≈5.5-5.6k RingOF=0` in gameplay; and **0**
   `GPUValidationError` / `SPIR-V to WGSL failed` lines.
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
8. For the **occlusion-enabled** build (2026-11-28 onwards), confirm the
   console no longer prints `WARNING: Occlusion culling is disabled at
   build-time.` (0 occurrences in the final 40 s TPS run,
   `/tmp/final_occl_console.log`), and that the wasm contains
   `RaycastOcclusionCull`/`raycast_occlusion` strings (the real Embree path).
   TPS has `rendering/occlusion_culling/use_occlusion_culling=true` and a baked
   `OccluderInstance3D`; note that no TPS cull *win* was measured (see the
   web-platform section).
9. To test the **threaded** build: set the TPS export preset
   `variant/thread_support=true` and export (the editor passes `threads` to
   `tools/grit_build.py`, scons builds `threads=yes`, and the template loses the
   `.nothreads` suffix). Serve the export with `Cross-Origin-Opener-Policy:
   same-origin` + `Cross-Origin-Embedder-Policy: require-corp` (a plain static
   server will not work) and check `crossOriginIsolated === true` in the page
   before loading — otherwise SharedArrayBuffer/workers are unavailable. Expect
   the same as the nothreads build (60 fps VSync, 0 GPU/shader/worker errors,
   no occlusion warning); the final verification used `/tmp/serve_coop_log.py`
   on 127.0.0.1:8225 (`/tmp/threaded_final_console.log`). Remember the Grit
   stale-staging gotcha below when switching a profile tree between threads
   modes.

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
patches in `0004`/`0005`, so fresh toolchain installs of that era got the same
driver (all tags were later deleted 2026-11-28; `4.8-rc1` is the current
branch).

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
paths covered at that point — 57/57 after the later web-platform changes;
round-trip all 8 patches byte-identical). The kept SDFGI
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

## Web platform (HTML5 Fast): occlusion culling enabled; threads=yes fixed and verified (opt-in) (2026-11-28)

### A. Occlusion culling is enabled in web/WebGPU builds (committed in `96a71a19bf`)

- **Background.** Godot's occlusion culling is implemented **only** in the
  `raycast` module (`modules/raycast`, CPU Embree). Upstream
  `platform/web/detect.py` forces `"module_raycast_enabled": False` for web
  with the comment "Embree is heavy and requires too much memory (GH-70621)"
  (upstream commit `8c40edf8c0`, "Web: Disable raycast module by default (no
  occlusion culling)"), so every upstream web template ships the stub:
  `renderer_scene_occlusion_cull.h` prints
  "WARNING: Occlusion culling is disabled at build-time." and TPS's
  `occlusion_culling/use_occlusion_culling=true` + baked `OccluderInstance3D`
  (`level/geometry/scenes/structure.tscn` → `occluder.occ`) did nothing.
- **Change.** Patch `0003` overrides `detect.py` to
  `"module_raycast_enabled": True` with an explanatory comment. Embree is
  vendored in-tree (`thirdparty/embree`, 337 tracked files, ≈4.4 MB of source)
  and builds for wasm32 with SIMD. Cost: template wasm
  **50,471,223 → 51,648,024 B (+1.18 MB, +2.3%)** plus Embree's runtime memory;
  remove the override to return to upstream's memory-optimized default.
- **Verification.** The template contains the real
  `RaycastOcclusionCull`/Embree path (`RaycastOcclusionCull|raycast_occlusion`
  strings present; 12 matches in the final rc1 wasm). The rc1 nothreads TPS run
  (`/tmp/rc1_final_default_console.log`) and the earlier beta runs
  (`/tmp/final_nothreads_console.log`, `/tmp/final_occl_console.log`) all have
  **0** "Occlusion culling is disabled at build-time" warnings, **0** GPU
  validation errors, **0** shader errors and 59-60 fps (5.5-5.6k draws/frame on
  rc1).
- **Measured culling yield on the TPS test camera: zero.** Temporary
  instrumentation (removed afterwards) showed the path active — scenario
  `instances=1`, HZ buffer **30×16** with `hz_min` dropping **4194.36 → 315.08**
  once the occluder was in view, ~530-1000 `is_occluded` AABB tests/frame — but
  `true=0` culls and draws/frame unchanged with the setting on vs off. The
  feature is compiled/working/available, but the TPS camera never has an
  instance AABB fully covered by the baked occluder, so there is no TPS demo
  benefit; a scene with genuinely occluded instances should benefit (not yet
  benchmarked). Upstream disabled it for memory reasons, so watch wasm heap on
  large scenes.
- **How to turn it off:** delete/revert the `module_raycast_enabled: True`
  override in `platform/web/detect.py` (patch `0003`) — the template returns to
  upstream's stub with the build-time warning and ~1.2 MB smaller wasm.

### B. `threads=yes` was broken, then FIXED and browser-verified (opt-in)

**History:** on 2026-11-28 ~01:30-02:20 a threaded template was built and
tested for the first time and it **wedged** (engine logged its init lines, then
the main thread stopped responding, loading overlay never hid, CDP
`Runtime.evaluate` timed out); it was documented as "broken / not shipped".
Later that morning (~09:20-09:56) both root causes were found and fixed, and
the threaded build is now **verified working** — it stays **opt-in** only
because it needs cross-origin-isolation hosting.

- **Root cause 1 — legacy pthread flags.** The fork's `platform/web/detect.py`
  had replaced upstream Godot 4.8's known-good `env["threads"]` configuration
  with legacy/unknown Emscripten settings (`-sUSE_PTHREADS=1`,
  `-sPTHREAD_POOL_SIZE=0`, `-sPTHREAD_POOL_SIZE_STRICT=0`,
  `-sWASM_MEM_MAX=2048MB`; `USE_PTHREADS` is a deprecated alias, `WASM_MEM_MAX`
  legacy). That compiled without the `__EMSCRIPTEN_PTHREADS__` macro and
  spawned every pthread lazily during startup, wedging the page while the main
  thread was inside the wasm entry point. **Fix:** restored upstream's
  configuration in `detect.py` — `-pthread` in `CCFLAGS` and `LINKFLAGS`,
  `-sPTHREAD_POOL_SIZE="Module['emscriptenPoolSize']||8"`,
  `-sMAXIMUM_MEMORY=2048MB` (plus the existing `PTHREAD_NO_RENAME` define and
  `DEFAULT_PTHREAD_STACK_SIZE`).
- **Root cause 2 — async resource creation on a pthread.**
  `servers/rendering/renderer_rd/renderer_compositor_rd.h`
  `can_create_resources_async()` returned `true`, so the render server's
  texture-create fast path (`FUNCRIDTEX*` macros in
  `servers/rendering/rendering_server_default.h`) executed
  `TextureStorage::texture_2d_initialize` immediately on the calling thread.
  Threaded scene loading therefore ran WebGPU calls on a pthread where the
  emdawnwebgpu `GPUDevice` JS object does not exist; workers died with
  `Uncaught TypeError: Cannot read properties of undefined (reading
  'createTexture')` and the TPS level load stopped. **Fix:** under
  `#ifdef WEBGPU_ENABLED`, `can_create_resources_async()` now returns **false**,
  routing resource creation (textures, buffers, ...) through the render-server
  command queue, which executes on the render/main thread (verified
  `create_thread=0`: no separate render thread is used).
  Both fixes are the only product changes; all temporary `[WGPU-DBG]`
  instrumentation was removed, and the final wasm contains none.
- **How it is built.** TPS export preset `variant/thread_support=true` → the
  editor passes `threads` to `tools/grit_build.py` → scons `threads=yes` → a
  template without the `.nothreads` suffix. Serve it with
  `Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy:
  require-corp` (e.g. `/tmp/serve_coop_log.py`) so `crossOriginIsolated ===
  true` (required for SharedArrayBuffer/workers).
- **Verification (2026-11-28; rc1 threaded run at ~12:25).** Threaded TPS
  export (rc1): menu → PLAY loads the full level, **59-60 fps with VSync**,
  draws/f ≈5.2-5.3k, **0 GPU validation errors, 0 shader errors, 0 worker
  errors**, and no "Occlusion culling is disabled at build-time" warning (the
  template also has raycast/Embree enabled). Evidence:
  `/tmp/rc1_final_threaded_console.log`, `/tmp/site-rc1-final-threaded`,
  `/tmp/rc1_threaded_final_server.log`; the previous beta run was
  `/tmp/threaded_final_console.log` (draws/f ≈5.2-5.6k). Uncapped measurement
  (Chrome for Testing, 960×540, `--disable-gpu-vsync
  --disable-frame-rate-limit`): **rc1 threaded 131.3 fps**
  (`/tmp/rc1_threaded_uncap2_console.log`); on the previous beta stack it was
  **threaded 137.3 vs single-threaded 124 fps (~+11% CPU headroom)**, and the
  default rc1 uncapped figure is measured after the rebase (not recorded here).
  Normal play is VSync-capped at 60 for both.
- **Deployment caveat — why it stays opt-in.** Threaded builds require
  cross-origin isolation headers (SharedArrayBuffer); a plain static host
  (`python3 -m http.server`, most simple hosting) will **not** work. The shipped
  TPS preset therefore keeps `variant/thread_support=false` (static-host
  friendly) and threading is a preset option for capable hosts. At 60 Hz there
  is 2x headroom single-threaded, so threads only matter for >60 Hz or
  CPU-heavier scenes.

### C. Artifacts / Grit gotcha

- The rc1 stack is regenerated and round-trip-verified on a pristine
  `7c97c29779` checkout (all 8 patches byte-identical, coverage **1,790/1,790**;
  `0003` 46,094 B, `0004` 893,686 B, `0005` 5,941,786 B — full sizes in the
  environment snapshot). Patch `0004` carries the tint_cli host shims that fix
  a latent fresh-build failure; `0005`'s layer list is 160 paths (`0004`'s is
  34 after gaining the two shim files).
- Templates (rc1): nothreads `godot.web.template_release.wasm32.nothreads.zip`
  (12:36:01, zip sha `c61caa74…`, wasm sha `f28e7f77…`) and the staged threaded
  `godot.web.template_release.wasm32.zip` (12:37:49, zip sha `f80d59e6…`; TPS
  export wasm 51,133,278 B in `/tmp/site-rc1-final-threaded`).
- **Grit template-staging gotcha:** switching a Grit profile tree between
  `threads=no` and `threads=yes` can leave stale shared web staging
  (`bin/.web_zip` and the non-suffixed template files), so a later **nothreads**
  export can silently package the **threaded** wasm. Clean the profile tree's
  `bin/` web artifacts (or rebuild after removing them —
  `/tmp/web_cleanup.log`) when switching variants.

## Godot 4.8-rc1 rebase (2026-11-28)

- **Base.** Upstream `godotengine/godot` master **`7c97c29779`** ("Bump version
  to 4.8-rc"; `version.py` major 4 / minor 8 / patch 0 / status `"rc"`),
  **86 commits ahead** of the previous 4.8.0-beta pin `67421f36e5`. Upstream
  has **no git tag `4.8-rc1`** (tags stop at `4.7.2-stable`), so "4.8-rc1" is
  our name for this master rc-state commit; `patches/upstream.lock` now records
  `godot_tag = "master (4.8.0-rc1)"` + `godot_commit = 7c97c29779…`.
- **Rebase mechanics.** The old stack was applied to a fresh worktree at
  `7c97c29779` with `git apply --3way`. Exactly **one conflict**: in
  `servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl`
  rc1 made `prev_data.inv_view_precision` / `data.inv_view_precision`
  precision-aware vec4 accesses, while our patch's context expected the old
  form. Resolution: keep our `batch_instance_index` rename and take rc1's new
  `.xyz` on those two accesses. Every other stack file applied cleanly.
- **Latent host-build bug fixed (fresh builds).** `drivers/webgpu/tint_cli`
  (the standalone shim tree used to build the build-time `tint_convert_cli`
  host tool) lacked `core/os/os.h` and `core/string/ustring.h`, although
  `drivers/webgpu/spirv_preprocess.cpp` includes both and
  `spirv_preprocess.h` uses `String`. Old trees only worked because
  `bin/tint_convert_cli` was prebuilt, so a fresh worktree failed to build the
  tool. Fix (patch `0004`): added minimal
  `drivers/webgpu/tint_cli/core/os/os.h` and
  `drivers/webgpu/tint_cli/core/string/ustring.h` shims (a small `String` over
  `std::string` + `itos`), and the existing
  `drivers/webgpu/tint_cli/core/templates/vector.h` shim now includes
  `core/string/ustring.h`. `0004` is **893,686 B** (was 891,250) and its layer
  list is **34** paths (was 32).
- **Provisioner / bootstrap ref.** `modules/grit/editor/grit_provisioner.cpp`
  (patch `0001`) sets `PATCH_STACK_REF = "4.8-rc1"` and
  `PATCH_STACK_ARCHIVE_URL = https://github.com/2maac/fast-godot/archive/refs/heads/4.8-rc1.tar.gz`
  — the **branch** archive; the repository is **tag-free since 2026-11-28**
  (the URL was `…/refs/tags/4.8-rc1…` before that, and `4.8-beta5` before
  that), so fresh toolchain boots fetch the current `4.8-rc1` branch tip. The
  old `4.8-beta5` branch and tag were removed 2026-11-28 (their commits are
  ancestors of `4.8-rc1`). The repo default branch was switched from
  `4.8-beta5` to `4.8-rc1` on 2026-11-28. The change is +141 B in `0001`
  (508,734 → **508,875 B**); the editor was rebuilt at 13:24 so its embedded
  provisioner uses the branch URL.
- **Tag-free bootstrap switch + rebuild (2026-11-28 ~13:24).** `0001`'s
  `PATCH_STACK_ARCHIVE_URL` now fetches the archive from `refs/heads/4.8-rc1`;
  the stack was regenerated (`0001` 508,734 → **508,875 B**, +141 B) and
  round-trip verified on a pristine `7c97c29779` (`/tmp/applytest-rc1`: all
  **8 patches byte-identical**, **1,790/1,790** layer paths). The editor was
  rebuilt from `/tmp/godot48-rc1` at 13:24:38 (sha256 `9499ffae…`; embedded
  provisioner now contains `…/refs/heads/4.8-rc1.tar.gz`), restaged and
  reinstalled. **All tags were then deleted** locally and on `origin`
  (`4.8-rc1`, the Godot reference tags and `up-4.7.2`); the repository now has
  exactly two branches, `4.7.2` and `4.8-rc1`. The web templates are
  unchanged — the provisioner is editor-only code. Fresh bootstrap from the
  deleted-tag replacement (the branch archive) was re-verified with
  `GRIT_ENGINE_PROBE=1` on an isolated HOME (`/tmp/grit_engine_probe5.log`,
  ~13:37: "prepared engine source … + 4.8-rc1 patches", "engine probe ok").
- **Regenerated stack.** `0001` **508,875 B**, `0002` 203,698 B, `0003` 46,094 B,
  `0004` 893,686 B, `0005` 5,941,786 B, `0006` 15,133,768 B, `0007` 3,541,650 B,
  `0099` 29,738 B; 1,790 layer paths total (1,790 changed worktree paths, all
  covered). Round-trip on a pristine `7c97c29779` reproduces all 8 patches
  byte-identically.
- **Builds/artifacts.** Editor `4.8.rc.custom_build.7c97c2977` (sha
  `9499ffae…`, rebuilt 13:24:38 for the tag-free bootstrap); nothreads template
  zip `c61caa74…` (wasm `f28e7f77…`), site published 12:36:00; threaded
  template zip `f80d59e6…` (TPS export wasm 51,133,278 B in
  `/tmp/site-rc1-final-threaded`). Details in the environment snapshot.
- **Runtime verification (Chrome for Testing, TPS Forward+ WebGPU).** Default
  rc1: 59-60 fps VSync, draws/f ≈5.5-5.6k, pmndrs spatial active, no occlusion
  warning, 0 GPU validation errors, 0 shader errors. Threaded rc1 (served with
  COOP/COEP, `crossOriginIsolated`): 59-60 fps VSync, draws/f ≈5.2-5.3k, same
  clean errors and warning, uncapped **131.3 fps**
  (`/tmp/rc1_threaded_uncap2_console.log`; previous beta stack: 137.3 threaded
  vs 124 single-threaded). The default rc1 uncapped figure is measured after
  the rebase (not recorded here).

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
   and ref (`patches/upstream.lock`'s `godot_commit`, patch branch `4.8-rc1`,
   previously tags `4.8-rc1`/`4.8-beta5`) and applies
   them with system git, but nothing verifies the downloaded tarballs beyond
   HTTPS, and the patch **branch is mutable** (force-pushable; all tags were
   deleted 2026-11-28, so the branch tip is the only stack pointer). A silently
   moved branch changes what fresh installs build. Pinning the stack by commit
   inside the editor build is awkward (the commit containing the provisioner
   only exists after the build), so the practical guard is: never force-push
   the `4.8-rc1` branch silently, and keep the provisioner validating the
   resulting tree.
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
14. **Web occlusion culling is enabled but shows no TPS win (2026-11-28).**
    Patch `0003` compiles the raycast/Embree module into web builds, so
    `use_occlusion_culling` is honoured and the build-time warning is gone
    (verified). On the TPS test camera the measured yield is **zero** (0 culls,
    draws unchanged; the occluder never fully covers an instance AABB) — a
    scene with genuinely occluded geometry still needs a benchmark, and the
    Embree memory cost (+1.18 MB wasm and runtime memory, upstream's reason for
    disabling it) should be watched on large scenes. See the web-platform
    section.
15. **Web `threads=yes` — FIXED and browser-verified (2026-11-28), opt-in.**
    Threaded templates boot and play the TPS level on both the beta and rc1
    stacks (59-60 fps VSync, 0 GPU/shader/worker errors; uncapped 131.3 fps on
    rc1 threaded, 137.3 vs 124 fps single-threaded on the beta stack). Fixes:
    upstream pthread flags restored in `platform/web/detect.py` (`-pthread` +
    `PTHREAD_POOL_SIZE=Module['emscriptenPoolSize']||8` + `MAXIMUM_MEMORY=2048MB`;
    the legacy `-sUSE_PTHREADS`/pool=0 scheme caused the startup wedge) and
    `can_create_resources_async()` returns false under `WEBGPU_ENABLED` so
    threaded resource creation goes through the render server command queue on
    the render/main thread. It stays opt-in because it needs COOP/COEP hosting
    for `crossOriginIsolated`; the shipped preset keeps
    `variant/thread_support=false`. Do not advertise threads as universally
    available, and note threaded gains only matter above 60 Hz or in CPU-heavy
    scenes. See the web-platform section.
16. **4.8-rc1 rebase follow-ups (2026-11-28).** The base is an upstream master
    rc-state commit that upstream has **not tagged** (`7c97c29779`, "Bump
    version to 4.8-rc"; no `4.8-rc1` git tag upstream — "4.8-rc1" is our name).
    When upstream cuts a real tag, re-pin `patches/upstream.lock` and regenerate
    the stack. The GitHub repo **default branch** was switched from `4.8-beta5`
    to `4.8-rc1` on 2026-11-28 (`gh api -X PATCH repos/2maac/fast-godot -f
    default_branch=4.8-rc1`; remote `HEAD` now resolves to the `4.8-rc1` tip).
    The `4.8-rc1` branch (stack commit `76d82e9993`) is pushed; **all tags
    were deleted on 2026-11-28**, so the bootstrap fetches the branch archive
    (`refs/heads/4.8-rc1`). Fresh bootstrap was verified end-to-end on an
    isolated HOME with `GRIT_ENGINE_PROBE=1` (`/tmp/grit_engine_probe4.log`):
    *"Grit: prepared engine source from Godot 7c97c297793c + 4.8-rc1 patches"*
    followed by *"engine probe ok"*. **Re-verified 2026-11-28 ~13:37 after the
    tag deletion, from the branch archive** (`/tmp/grit_engine_probe5.log`,
    HOME `/tmp/grit-probe-tagfree`): the same two lines, and the prepared
    tree's provisioner contains the `refs/heads/4.8-rc1` URL. `BUILD.txt`
    was refreshed for the tag-free rebuild at 13:25:32. The new `tint_cli` host shims are minimal by
    design — if
    `spirv_preprocess.cpp` starts using more of `String`/`OS`, extend the shims
    rather than pulling in the full engine headers.

## Practical notes

- Never run two exports concurrently (they used to stampede the machine; now
  serialized by the tint-cli lock, but still).
- Building/staging inside Dropbox triggers FileProvider/Spotlight churn; keep
  build trees outside Dropbox (`/tmp` or `~`), stage the final app only.
- Historically `patches/*` tag pushes were force-pushed (branch renamed
  from `4.7.2`/`4.8-beta5`); `.gitignore` on those branches must exclude `out/`
  or a 100 MB app binary gets committed and GitHub rejects the push.
- **The repository is tag-free since 2026-11-28**: every tag was deleted
  locally and on `origin` (`4.8-rc1`, the Godot reference tags and
  `up-4.7.2`). Refs are the two branches only — `4.8-rc1` (current) and
  `4.7.2` (history); verify with `git ls-remote origin`.
- **Editor builds made before 2026-11-28 ~13:25 embed the deleted tag URL**
  (`…/refs/tags/4.8-rc1.tar.gz`), so their first-use toolchain bootstrap now
  fails. Reinstall from the tag-free build (editor sha256 `9499ffae…`,
  `4.8.rc.custom_build.7c97c2977`, rebuilt 13:24:38) or patch the provisioner
  to the branch URL; existing toolchains on a machine keep working.
- The `4.8-beta5-source` release/asset was **removed 2026-11-26**. New builds
  bootstrap from Godot's repo + the current patch **branch** (`4.8-rc1`);
  editors built before the removal can no longer provision a fresh toolchain,
  so keep their existing toolchains or reinstall from a current build.
- **Keep the `4.8-rc1` branch pointing at the stack you ship**: fresh
  toolchain installs download it. The stack commit is `76d82e9993` (patch
  `0001`'s provisioner fetches `refs/heads/4.8-rc1`); the docs commit sits
  above it. The old **`4.8-beta5` branch and tag were removed 2026-11-28**
  (last tag value `a7a5aefbe2`, the threads fix; earlier moves that day:
  `529407b9fc` → `91a0a3e8e4` → `cb0aa639c7` → revert stack). Never
  force-push the `4.8-rc1` branch silently — its tip is the patch-stack
  pointer for fresh installs now that no tag exists. After any push, re-check
  `git ls-remote origin refs/heads/4.8-rc1`. Note `4.8-rc1` is our **branch**
  name — upstream has no such git tag, so the reference resolves only on this
  repo. First-use provisioning also needs system `git` on the machine.
- Layout (2026-11-26): the patch stack became a **self-contained portable
  bundle in `patches/`** (patch files + `layers/` + `upstream.lock`, no version
  subfolder), and this handoff + the migration history moved into `docs/` (the
  handoff is `docs/WEBGPU_HANDOFF.md` now). The bootstrap and the editor are
  coupled: editor builds before the layout change look for `patches/4.8-beta5/`
  (and `upstream.lock` at the archive root) and cannot bootstrap from the current
  tag; rebuild the editor whenever the bundle layout changes (current matching
  build: `4.8.rc.custom_build.7c97c2977`).
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
  are **kept after the revert** (that is why the `0005` layer list grew from 156
  to 159 paths and `0005` from 5,936,829 B; the threads fix took it to 160
  paths / 5,941,474 B, and the rc1 rebase regenerated it at 160 paths /
  5,941,786 B). The rc1 rebase also added the two `tint_cli` host shims to
  `0004` (32 → 34 paths). Current totals: **1,790 layer paths** and
  **1,790/1,790** worktree-changed paths covered.
  When regenerating, cross-check every `git -C /tmp/godot48-rc1 status` path
  against the union of all layer lists in addition to running the round-trip.
- **Round-trip check after regenerating**: apply the full `patches/` bundle to a
  pristine checkout of the pinned commit and re-diff each layer with its
  `layers/*.files` list; the 2026-11-28 ~12:31 rc1 round trip is byte-identical
  for all 8 layers (`0003` 46,094 B, `0004` 893,686 B, `0005` 5,941,786 B — full
  sizes in the Environment snapshot; 1,790 layer paths).
- **Host `tint_convert_cli` shims (rc1, 2026-11-28)**: a **fresh** worktree
  could not build the build-time `tint_convert_cli` host tool because
  `drivers/webgpu/spirv_preprocess.cpp` includes `core/os/os.h` /
  `core/string/ustring.h` and `spirv_preprocess.h` uses `String`, while the
  standalone shim tree (`drivers/webgpu/tint_cli/`) only had
  `core/templates/{vector,hash_map,hash_set}.h`. Old trees only worked because
  `bin/tint_convert_cli` was prebuilt. Patch `0004` now adds minimal
  `tint_cli/core/os/os.h` + `tint_cli/core/string/ustring.h` shims and the
  `tint_cli/core/templates/vector.h` shim includes the ustring shim. If a
  fresh `scons platform=web` fails in `tint_convert_cli` with missing
  `os.h`/`ustring.h`, the layer list or the shims were not applied.
- **4.8-rc1 naming**: upstream has no git tag `4.8-rc1` (its tags stop at
  `4.7.2-stable`); `patches/upstream.lock` pins `godot_commit 7c97c29779`
  ("Bump version to 4.8-rc", `version.py` status `rc`) and calls it
  `master (4.8.0-rc1)`. `4.8-rc1` is our **branch** name (our repo is
  tag-free); when upstream cuts a real tag, re-pin and regenerate.

Verified 2026-11-26: copying **only** `patches/` + `tools/apply.sh` and applying
them to a fresh Godot `67421f36e5` checkout reproduced all 16,064 worktree files
with zero mismatches, and the moved tag archive contains
`patches/{*.patch,layers/,upstream.lock}` with no root-level lock or layers.
- Ref naming (2026-11-26): the remote branch was renamed from `patches/4.8-beta5`
  to plain `4.8-beta5` (same name as the tag and the local branch; the patch
  files live at the repo root). Use full refs when the ambiguity matters.
  `4.8-beta5` was the repository **default branch** from 2026-11-26 (when it
  replaced `4.7.2`) until 2026-11-28, when it was switched to `4.8-rc1`; its
  **branch and tag were both deleted on 2026-11-28** (locally and on
  `origin`), so `4.8-beta5` is no longer a ref at all and the beta5 stack is
  reachable only as history through `4.8-rc1`. **On the same day every other
  tag was deleted too** (`4.8-rc1` and the local Godot reference tags), so the
  repo now has **two branches and no tags**: `4.8-rc1` (current) and `4.7.2`
  (history). The remote `HEAD` resolves to the `4.8-rc1` branch tip.
- One SDK (Emscripten 6.0.12) serves everything; do not pin a second version.
- The **current** macOS editor build tree is **`/tmp/godot48-rc1`** — detached
  at upstream `7c97c297793` with the whole stack applied (1,790 changed paths;
  see the rc1 section for the one resolved shader conflict and the tint_cli
  shim fix). The old `/tmp/godot48` (4.8.0-beta stack) is retained as history.
  Re-apply the repo's `patches/*.patch` files to a fresh checkout of
  `7c97c29779` to reproduce the rc1 tree (or apply to the beta pin for the old
  one).
- Ring diagnostics: `rendering/webgpu/debug_frames` prints `passFlush=` and
  `ringOF=` per frame; `passFlush` is normal on heavy frames, `ringOF` must stay
  0 (it now means a single pass exceeded the whole ring).
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
- After committing in `/tmp/godot48-rc1` (or the old `/tmp/godot48`), explicitly force `core/version_hash.gen.cpp`
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
- **Web occlusion culling (2026-11-28)**: it is now **compiled in** (patch
  `0003` overrides `platform/web/detect.py` to `module_raycast_enabled: True`),
  so web builds honour `rendering/occlusion_culling/use_occlusion_culling` and
  no longer print the build-time warning. This is a deliberate deviation from
  upstream (GH-70621): +1.18 MB wasm and Embree's memory. **To revert**, drop the
  override in `detect.py` (upstream's stub + warning return). **No TPS win was
  measured** (see the web-platform section); benchmark a scene that actually has
  occluded instances before relying on it.
- **Threads are fixed but opt-in (2026-11-28)**: `threads=yes` templates boot
  and play (60 fps VSync, 0 errors; 137.3 vs 124 fps uncapped). To test one,
  set the preset `variant/thread_support=true`, export, and serve it with
  COOP `same-origin` + COEP `require-corp` headers so `crossOriginIsolated ===
  true` (a plain static server will not work). The shipped preset stays
  `variant/thread_support=false`. If a threaded build wedges again, check the
  `detect.py` pthread flags (`-pthread` +
  `-sPTHREAD_POOL_SIZE="Module['emscriptenPoolSize']||8"` +
  `-sMAXIMUM_MEMORY=2048MB`) and `can_create_resources_async()` (must be false
  under `WEBGPU_ENABLED`), and remember the stale-staging gotcha below.
- **Grit template-staging gotcha**: switching a Grit profile tree between
  `threads=no` and `threads=yes` can leave stale shared web staging
  (`bin/.web_zip` and non-suffixed template files), so a later **nothreads**
  export can silently package the threaded wasm. After switching variants,
  remove the profile tree's `bin/` web artifacts (or rebuild after deleting
  them) — `/tmp/web_cleanup.log` is the cleanup rebuild that fixed it.
- **BUILD.txt**: `out/editor-staging/Fast_Godot_4.8_BUILD.txt` is not part of
  `patches/`. It is **refreshed for the tag-free rebuild** (mtime 13:25:32,
  8,505 B): editor
  `4.8.rc.custom_build.7c97c2977`, editor sha256 `9499ffae…`, base `7c97c29779`, both template zip hashes
  and site wasm `f28e7f77…`; treat it as the build record.
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
  FSR 1 instead of FSR 2. The current site build is the default nothreads
  export with the occlusion + threads fixes (2026-11-28 09:55:13); the threaded
  build is opt-in and is not what the default site serves.
