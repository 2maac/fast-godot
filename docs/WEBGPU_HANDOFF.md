# Fast Godot WebGPU handoff — 4.8-beta5 port

Resume doc. Last updated 2026-11-26 ~21:00. Ring hardening landed and was
browser-checked (owner, 2026-11-26: the TPS export works fine): the push-constant
ring is flushed only between passes, so a mid-frame submit can no longer
interrupt an in-pass MSAA resolve. The `4.8-beta5` patch tag was moved to the
hardening tip (`4209e2f7`) so fresh toolchains build the hardened driver, and the
remote branch is now simply `4.8-beta5` (no `patches/` prefix; patch files still
live in `patches/`; `4.8-beta5` is the repository default branch).
The toolchain bootstrap builds the engine source
from Godot's repository + that tag; the obsolete source release and its tag were
deleted. `patches/` is a self-contained portable bundle (patch files +
`layers/` + `upstream.lock`); worktree HEAD `582d619026`, editor
`4.8.beta.custom_build.582d61902`, and this file lives in `docs/` together with
`PATCH_STACK.md` and the migration history.

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
the same base commit plus working-tree changes, so the editor version stays
`4.8.beta.custom_build.582d61902`; see "Forward+ on WebGPU (2026-11-27)" below
for what changed and the remaining limitations.

## Where to start (docs map)

| doc | status |
|---|---|
| `README.md` | repo mechanics (patch stack, tools, rules) |
| **this file** | current state: toolchain, build/export, MSAA fix, encryption, open work |
| `PATCH_STACK.md` (same folder) | what every patch layer contains; apply/regenerate commands |
| `WEBGPU_DRIVER_MIGRATION.md` | archived history of the komm64 driver adoption (4.7) |

## What this repository is

The repo is a **patch stack**, not a Godot fork checkout:

- ~~`patches/4.7.2`~~ — the old 4.7.2 stack, **dropped and deleted 2026-11-26**
  from GitHub (as was `integrate/komm64-webgpu`). The project is consolidated on
  the 4.8 stack. The full-source `4.7.2` branch remains on the remote for
  history only (`origin/4.7.2` = `d8cb1fd77c`, a Godot 4.7.2 tree); a leftover
  local branch `fresh` still points at that same commit and can be deleted.
- The 4.8 stack (branch and tag `4.8-beta5`, repo default branch) is a
  **self-contained portable bundle in `patches/`**: patch files
  (`0001-…0099-*.patch`), the `layers/*.files` regeneration lists and
  `upstream.lock` (the Godot pin) all live there; ported onto godot `master`
  @ `67421f36e5` (4.8.0-beta). Copying `patches/` + `tools/apply.sh` reproduces
  the stack elsewhere. Only one stack exists now.
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
| branch `4.8-beta5` = `origin/4.8-beta5` | see `git log --oneline -1` | the stack; branch and tag share the simple name now (use `refs/heads/4.8-beta5` / `refs/tags/4.8-beta5` when the ambiguity matters) |
| tag `4.8-beta5` | `6f717b8475` | release point, **moved 2026-11-26** to the current tip (ring hardening + self-contained `patches/` bundle) so fresh toolchain installs fetch it; docs commits after the move sit on the branch above it |
| `origin/4.7.2` | `d8cb1fd77c` | full-source **Godot 4.7.2** snapshot (single parentless commit), history only, not maintained |
| ~~tag `4.8-beta5-source`~~ | — | **release + tag removed 2026-11-26.** It held a parentless 4.8 source snapshot (`35e42f7d`, re-cut from the wrong 4.7.2 tree earlier that day) and the `fast-godot-4.8-beta5-src.zip` asset. Editors built before 2026-11-26 hardcoded that asset URL for first-use provisioning and can no longer bootstrap a fresh toolchain. New builds use Godot's repo + the `4.8-beta5` patch tag |

**Toolchain bootstrap (2026-11-26) no longer uses a source release.** New editor
builds download the patch stack tag archive (~8 MB), read `godot_commit` from its
`patches/upstream.lock`, download Godot's own source at that commit from
`godotengine/godot` (~76 MB), apply the stack's layers with `git apply`, and move
the result into `<toolchain>/engine/source` — see the Toolchain bootstrap
subsection under Build / toolchain. The old release asset and its tag were
**removed 2026-11-26**: editors built before that date hardcoded the asset URL
and can no longer provision a fresh toolchain (exports from machines that
already have a toolchain are unaffected).

## Environment snapshot (verified on this machine, 2026-11-26 ~19:15)

| item | actual value |
|---|---|
| patch stack repo | branch `4.8-beta5` = `origin/4.8-beta5`, **repository default branch since 2026-11-26** (simple name, same as the tag); tag `4.8-beta5` = `6f717b8475` — moved 2026-11-26 to the current tip (ring hardening + `patches/` bundle), which is what the bootstrap downloads |
| engine base | Godot `master` @ `67421f36e5` (4.8.0-beta), pinned in `patches/upstream.lock`; still an ancestor of the worktree HEAD |
| object model | this repo is a **partial clone** of `godotengine/godot` with `blob:none` promisor packs, so Godot-history blobs are fetched from upstream on demand. `port48` carries 87,467 commits not on `origin`, so full-source refs are pushed as single parentless snapshots instead (as `origin/4.7.2` is) |
| build worktree | `/tmp/godot48` — branch `port48`, HEAD `582d619026` (bundle upstream.lock bootstrap; `cdc0907bf6` patches/ layout, `9267990dcd` ring hardening, `130ed7f48d` bootstrap, `a495fc3e93` ring mitigation and `834c3d4fc2` pack-key port are ancestors), clean, not pushed (volatile) |
| other worktree | `/tmp/ref472` — detached at `d8cb1fd77c` (the 4.7.2 full-source tree, reference only) |
| toolchain bootstrap | downloads `godotengine/godot` @ `67421f36e5` (~76 MB) + the `fast-godot` tag `4.8-beta5` archive (~8 MB) and applies the stack with the system **git**; `GRIT_ENGINE_PROBE=1` runs just this step headlessly |
| toolchain (single SDK) | `~/fast_godot_toolchain`, 6.4 GB: Emscripten 6.0.12, Python 3.12.16 (+ zstandard 0.25.0), SCons 4.11.1, MoltenVK 1.4.2, `engine/source` = 4.8.0-beta |
| staged editor | `out/editor-staging/Fast_Godot_4.8.app` = `4.8.beta.custom_build.582d61902`, staged 2026-11-26 ~21:05 (bootstrap reads `patches/upstream.lock` in the stack archive); `~/Applications/Fast_Godot_4.8.app` is the same build. The ring hardening is driver/template-only, so it is in the template, not the editor binary |
| web template | rebuilt 2026-11-26 20:13 from the hardened driver (new warning + `passFlush` present, the old mid-frame warning gone) and exported into the test site |
| export test site | `~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/` — currently holds the TPS demo export (title "Godot Third-Person Shooter Demo"), written 2026-11-26 17:46 |
| source release | **removed 2026-11-26** (was tag `4.8-beta5-source` = `35e42f7d` + `fast-godot-4.8-beta5-src.zip`). New builds bootstrap from Godot's repo + the `4.8-beta5` patch tag; editors built before the removal can no longer provision a fresh toolchain |

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

1. download `https://github.com/2maac/fast-godot/archive/refs/tags/4.8-beta5.tar.gz`
   (~8 MB) — the patch stack;
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
- **the `4.8-beta5` tag is what fresh installs fetch**: it was moved on
  2026-11-26 to the current tip (`6f717b8475`), so new toolchains build the
  hardened driver and read the self-contained `patches/` bundle. Move the tag again when engine-side fixes land; docs commits
  after a move sit on the branch above it, and editor-only changes don't matter
  for template builds;
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
The current 4.8 build tree is a **volatile linked git worktree of this repo at
`/tmp/godot48`** (`git worktree list` shows it; branch `port48`, HEAD
`582d619026`, clean, not pushed). Ancestors: bundle-lock bootstrap `582d619026`,
patches/ layout `cdc0907bf6`, ring hardening `9267990dcd`, source bootstrap
`130ed7f48d`, MSAA mitigation `a495fc3e93`, pack-key port `834c3d4fc2`. If /tmp
is wiped, rebuild by applying the repo's `patches/*.patch` files to a fresh
Godot checkout.

Staged build (gitignored): `out/editor-staging/`
- `Fast_Godot_4.8.app` (`4.8.beta.custom_build.582d61902`, Fast Godot icon,
  ad-hoc signed, staged 2026-11-26 ~21:05) and `Fast_Godot_4.8-macos.zip`
- `godot.web.template_release.wasm32.nothreads.zip` (rebuilt 20:13 with the
  pass-boundary ring flush; the later bootstrap/layout changes are editor-only,
  so the template did not change again)
- `Fast_Godot_4.8_BUILD.txt` (version/time/flags; refreshed with this build)
Launchable install: `~/Applications/Fast_Godot_4.8.app` (same `582d61902` build).
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
the `passFlush=` DBGFRAME field; TPS was exported at 20:16 and the owner's
browser check passed (2026-11-26: the TPS export works fine). For counters
enable `rendering/webgpu/debug_frames`: `passFlush` may be >0 on a heavy frame,
`ringOF` must stay 0. Verified separately that the moved `4.8-beta5` tag archive
contains the hardened driver patch (`_ring_pass_boundary` in `0004`), so fresh
toolchain installs get the same driver.

Diagnostics kept in the driver: `rendering/webgpu/debug_frames` prints per-frame
pass/draw/resolve/ring counters (including `passFlush`) to the browser console;
`rendering/webgpu/disable_batching` disables the batch/firstInstance traits and
was used to rule the batching path out.

## Forward+ on WebGPU (2026-11-27)

Forward+ is the second rendering method running on the WebGPU device (after
Forward Mobile). Verified with the TPS demo exported through HTML5 Fast in a
browser (Chrome for Testing/Dawn, headless + WebGPU): the level renders fully
(player, environment, shadows, reflections, HUD), the console shows **0 WebGPU
validation errors** and **0 SPIR-V→WGSL failures**, and the build holds 60 fps
(VSync) at ~5.8k draws/frame in-game.

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
  `USE_ATTACHMENT` shader variant) and SDFGI's geometry injection is disabled
  (its SDF depth pass has no attachment-capable variant).
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

Forward+ **MSAA (2x/4x) and TAA are browser-verified** (0 validation errors,
full level renders). TAA on WebGPU uses an RGBA16F previous-velocity buffer and
an RGBA16F TAA-shader binding, because RG16F is not a WebGPU storage texture
format (the device would otherwise promote it to RG32F and break the copy-shader
write binding).

Remaining Forward+ limitations (documented, intentional):

- **SDFGI is disabled** on WebGPU (its geometry injection needs attachment-less
  render passes). Volumetric fog, SSAO/SSIL/SSR, VoxelGI and SSR work as far as
  this TPS run exercised them.
- **FSR2 is unavailable** (B10G11R11 storage images are not a WebGPU storage
  texel format); 3D scaling falls back to FSR 1 / off as described above.
- **Bake cost**: Forward+ bakes ~1524 shader-stage variants (mostly clustered
  scene variants), ~3-6 min on the dev machine, versus ~200 for Mobile. Skipped
  stages are harmless (the device never compiles them).

Historical note: the studio-foundation 4.7.1 stack (patches 0001-0033) was built
locally on 2026-11-26 for comparison and could not run TPS in Forward+ (Tint
traps on image atomics, vertex-stage storage-buffer limits, resolve layout
mismatches, and their bake missed scene materials). Those four classes are each
addressed in our stack as described above; the comparison document is kept in
this file's history.

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
   and tag (`patches/upstream.lock`'s `godot_commit`, patch tag `4.8-beta5`) and applies
   them with system git, but nothing verifies the downloaded tarballs beyond
   HTTPS, and the patch tag is mutable (force-pushable). A silently moved tag
   changes what fresh installs build. Pinning the stack by commit inside the
   editor build is awkward (the commit containing the provisioner only exists
   after the build), so the practical guard is: never move the patch tag
   silently, and keep the provisioner validating the resulting tree.

10. **Forward+ on WebGPU — WORKING, with limits (2026-11-27).** TPS boots and
    renders the full level at 60 fps with zero validation errors; MSAA 4x and TAA
    were verified too (see the 2026-11-27 (later) note). Remaining limitations:
    SDFGI is disabled (needs attachment-less passes); FSR2 is unavailable so 3D
    scaling falls back to FSR 1 / off. Forward+ bake is ~1524 stages (~3-6 min).
11. The Forward+ export was verified with the editor/template built from
    `/tmp/godot48` at HEAD `582d619026` plus the working-tree changes now
    captured in the regenerated patch stack; the editor version string is
    unchanged (`4.8.beta.custom_build.582d61902`). If a distinct version stamp
    is wanted, commit the stack changes in the build tree and rebuild.

## Practical notes

- Never run two exports concurrently (they used to stampede the machine; now
  serialized by the tint-cli lock, but still).
- Building/staging inside Dropbox triggers FileProvider/Spotlight churn; keep
  build trees outside Dropbox (`/tmp` or `~`), stage the final app only.
- Tag pushes on `patches/*` were force-pushed historically (branch renamed
  from `4.7.2`/`4.8-beta5`); `.gitignore` on those branches must exclude `out/`
  or a 100 MB app binary gets committed and GitHub rejects the push.
- Release tags can be force-updated (one was re-cut and then removed on
  2026-11-26). Verify with `git ls-remote origin refs/tags/<tag>` after any move.
- The `4.8-beta5-source` release/asset was **removed 2026-11-26**. New builds
  bootstrap from Godot's repo + the `4.8-beta5` patch tag; editors built before
  the removal can no longer provision a fresh toolchain, so keep their existing
  toolchains or reinstall from a current build.
- **Keep the `4.8-beta5` tag pointing at the stack you ship**: fresh toolchain
  installs download it. It was moved to the ring-hardening tip on 2026-11-26
  (`6f717b8475`); move it again for later engine-side fixes. First-use
  provisioning also needs system `git` on the machine.
- Layout (2026-11-26): the patch stack became a **self-contained portable
  bundle in `patches/`** (patch files + `layers/` + `upstream.lock`, no version
  subfolder), and this handoff + the migration history moved into `docs/` (the
  handoff is `docs/WEBGPU_HANDOFF.md` now). The bootstrap and the editor are
  coupled: editor builds before the layout change look for `patches/4.8-beta5/`
  (and `upstream.lock` at the archive root) and cannot bootstrap from the current
  tag; rebuild the editor whenever the bundle layout changes (current matching
  build: `4.8.beta.custom_build.582d61902`).

Verified 2026-11-26: copying **only** `patches/` + `tools/apply.sh` and applying
them to a fresh Godot `67421f36e5` checkout reproduced all 16,064 worktree files
with zero mismatches, and the moved tag archive contains
`patches/{*.patch,layers/,upstream.lock}` with no root-level lock or layers.
- Ref naming (2026-11-26): the remote branch was renamed from `patches/4.8-beta5`
  to plain `4.8-beta5` (same name as the tag and the local branch; the patch
  files live at the repo root). Use full refs when the ambiguity matters.
  `4.8-beta5` is the repository **default branch** since 2026-11-26 (was
  `4.7.2`), so pushes to it no longer trigger GitHub's "recent pushes → compare
  & pull request" banner; `4.7.2` stays history-only.
- One SDK (Emscripten 6.0.12) serves everything; do not pin a second version.
- The macOS editor build tree `/tmp/godot48` also carries the ring mitigation,
  the pass-boundary ring flush and the white-box pack-key port; re-apply
  the repo's `patches/*.patch` files to a fresh checkout to reproduce it.
- Ring diagnostics: `rendering/webgpu/debug_frames` prints `passFlush=` and
  `ringOF=` per frame; `passFlush` is normal on heavy frames, `ringOF` must stay
  0 (it now means a single pass exceeded the whole ring).
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
- After committing in `/tmp/godot48`, explicitly force `core/version_hash.gen.cpp`
  to regenerate (delete it and its object, rebuild, check `--version` shows the
  new HEAD). On 2026-11-26 a rebuild after committing kept the previous hash
  (`834c3d4fc`) until forced; a stale hash changes ShaderRD cache keys, so
  editor-baked shader caches would miss at runtime.
