# Fast Godot WebGPU handoff — 4.8-beta5 port

Resume doc. Last updated 2026-11-26 ~21:00 (facts re-verified on the live
machine: worktree/staged editor at `834c3d4fc2`, ref table and environment
snapshot corrected, stale "pack key not ported" item gone, `tools/make-patches.sh`
fixed and round-trip verified, and the `4.8-beta5-source` tag re-cut to a
parentless 4.8 snapshot after byte-verifying the release asset). The
push-constant ring flush path is still only mitigated, not structurally hardened
— it stays open (open issue 2).

## Where to start (docs map)

| doc | status |
|---|---|
| `README.md` | repo mechanics (patch stack, tools, rules) |
| **this file** | current state: toolchain, build/export, MSAA fix, encryption, open work |
| `docs/PATCH_STACK.md` | what every patch layer contains; apply/regenerate commands |
| `WEBGPU_DRIVER_MIGRATION.md` | archived history of the komm64 driver adoption (4.7) |

## What this repository is

The repo is a **patch stack**, not a Godot fork checkout:

- ~~`patches/4.7.2`~~ — the old 4.7.2 stack, **dropped and deleted 2026-11-26**
  from GitHub (as was `integrate/komm64-webgpu`). The project is consolidated on
  the 4.8 stack. The full-source `4.7.2` branch remains on the remote for
  history only (`origin/4.7.2` = `d8cb1fd77c`, a Godot 4.7.2 tree); a leftover
  local branch `fresh` still points at that same commit and can be deleted.
- `patches/4.8-beta5` — the 4.8 stack, same layers ported onto godot `master`
  @ `67421f36e5` (4.8.0-beta).
- `tools/apply.sh <godot-checkout> <stack>` applies a stack; `tools/make-patches.sh`
  regenerates it from a modified checkout with `git diff --binary`
  (icons/splash/logo/SPIR-V fixtures are binary patches: 8 hunks in `0005`, 15
  in `0007`). The script was fixed 2026-11-26 (it lacked `--binary`, used
  bash-4-only `mapfile`, and rejected linked worktrees); its round-trip from
  `/tmp/godot48` is byte-identical to the committed patches.
- `upstream.lock` pins the Godot base per branch.

| ref | commit | notes |
|---|---|---|
| `origin/patches/4.8-beta5` (local branch `4.8-beta5`) | see `git log --oneline -1` | the stack; only the remote ref carries the `patches/` prefix |
| tag `4.8-beta5` | `305312690f` | last release point; docs/tools commits sit on the branch above it |
| `origin/4.7.2` | `d8cb1fd77c` | full-source **Godot 4.7.2** snapshot (single parentless commit), history only, not maintained |
| tag `4.8-beta5-source` | `35e42f7df2` | single parentless commit holding the 4.8-beta5 source tree; **re-cut 2026-11-26** from `d8cb1fd77c` (the 4.7.2 tree). The release asset `fast-godot-4.8-beta5-src.zip` (96,229,476 B, sha256 `25bb6f17…`) is byte-verified to this tree |

Note: the toolchain provisioner copies the engine source from the local checkout
(`engine_root/../..` in `download_toolchain.sh`), so a fresh
`~/fast_godot_toolchain` does not download the `4.8-beta5-source` release; the
tag only matters when cutting the source-release artifact. Verified 2026-11-26:
extracting `fast-godot-4.8-beta5-src.zip` yields exactly the 16,064 tracked
files of the `834c3d4fc2` worktree (no differing file; only generated build
artifacts are absent), and the zip's comment is that commit hash.

## Environment snapshot (verified on this machine, 2026-11-26 ~19:15)

| item | actual value |
|---|---|
| patch stack repo | branch `4.8-beta5`, pushed to `origin/patches/4.8-beta5`; tag `4.8-beta5` = `305312690f` is the last release point, docs/tools commits sit on top |
| engine base | Godot `master` @ `67421f36e5` (4.8.0-beta), pinned in `upstream.lock`; still an ancestor of the worktree HEAD |
| object model | this repo is a **partial clone** of `godotengine/godot` with `blob:none` promisor packs, so Godot-history blobs are fetched from upstream on demand. `port48` carries 87,467 commits not on `origin`, so full-source refs are pushed as single parentless snapshots instead (as `origin/4.7.2` and `4.8-beta5-source` are) |
| build worktree | `/tmp/godot48` — branch `port48`, HEAD `834c3d4fc2`, clean, Godot 4.8.0-beta, not pushed (volatile) |
| other worktree | `/tmp/ref472` — detached at `d8cb1fd77c` (the 4.7.2 full-source tree, reference only) |
| toolchain (single SDK) | `~/fast_godot_toolchain`, 6.4 GB: Emscripten 6.0.12, Python 3.12.16 (+ zstandard 0.25.0), SCons 4.11.1, MoltenVK 1.4.2, `engine/source` = 4.8.0-beta |
| staged editor | `out/editor-staging/Fast_Godot_4.8.app` = `4.8.beta.custom_build.834c3d4fc`, staged 2026-11-26 18:21; `~/Applications/Fast_Godot_4.8.app` is the same build (launchable) |
| export test site | `~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/` — currently holds the TPS demo export (title "Godot Third-Person Shooter Demo"), written 2026-11-26 17:46 |
| source release | tag `4.8-beta5-source` = `35e42f7df2` (parentless snapshot, 16,064 files, tree = `834c3d4fc2`; re-cut and force-pushed 2026-11-26, old ref `d8cb1fd77c`). Asset `fast-godot-4.8-beta5-src.zip`, sha256 `25bb6f17…`, byte-verified |

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

Editor (macOS, from a checkout + applied stack):
```
scons platform=macos arch=arm64 target=editor \
  vulkan_sdk_path=$HOME/fast_godot_toolchain/moltenvk \
  tests=no debug_symbols=no angle=no production=yes
```
The current 4.8 build tree is a **volatile linked git worktree of this repo at
`/tmp/godot48`** (`git worktree list` shows it; branch `port48`, HEAD
`834c3d4fc2`, clean, not pushed). `a495fc3e93` is the push-constant
ring/MSAA fix and `834c3d4fc2` the white-box pack-key port; `f578f89fa9` was the
tip recorded before those two commits. If /tmp is wiped, rebuild by applying
`patches/4.8-beta5` to a fresh Godot checkout.

Staged build (gitignored): `out/editor-staging/`
- `Fast_Godot_4.8.app` (`4.8.beta.custom_build.834c3d4fc`, Fast Godot icon,
  ad-hoc signed, staged 2026-11-26 18:21) and `Fast_Godot_4.8-macos.zip`
- `godot.web.template_release.wasm32.nothreads.zip`
- `Fast_Godot_4.8_BUILD.txt` (version/time/flags). Two quirks: its `HEAD` line
  reads `a495fc3e93` (recorded before the `834c3d4fc2` commit landed) and its
  `Known gaps` paragraph is stale (it still says the pack key was not ported);
  trust the binary's `--version`.
Launchable install: `~/Applications/Fast_Godot_4.8.app` (same `834c3d4fc` build).
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

Fix: ring grows to 8 MB (32768 slots, ~15x headroom), plus a one-shot warning
if an extreme frame ever overflows. Verified with the orbit capture: before,
2-10 black frames per 48; after, none and `RingOF=0`. User-verified in a real
browser on the TPS demo with MSAA 4x.

Residual risk — **still open, tracked as open issue 2**: the ring flush path
itself is the weak point (a frame with >32k push-constant slots would still
overflow). A proper fix would make the flush not interrupt in-pass resolves
(e.g. defer flush to pass boundaries, or use a per-frame growing ring); that is
the next hardening step and has **not** been done. Do not close this thinking
the 8 MB ring fixed it.

### Structural hardening plan (open issue 2 — not started)

A fix must make the flush stop interrupting an MSAA-resolving pass mid-frame.
Notes from the driver (2026-11-26):

- **Deferring the flush alone is not enough.** Draws already recorded reference
  ring slots through dynamic offsets; if the ring wraps, a later slot overwrites
  data that an earlier recorded draw still reads at submit time. Deferring the
  submit only moves the corruption later.
- **Chunked ring storage is the viable shape:** allocate ring buffers lazily in
  chunks, one bind group per chunk (dynamic offset within that chunk), switch
  chunk instead of ending the pass when the current one would overflow, and
  reuse a chunk only after the submit that consumed it completes. Removal of the
  mid-pass interruption is the structural part; an extreme frame then costs
  memory, not correctness.
- **Complication:** engine-side uniform sets merge the PC ring entry
  (`merged_pc_group_layout` / `current_pc_bind_group`), so those bind groups
  embed the ring buffer pointer; each needs a per-chunk variant or a rebind path.
  This is the bulk of the work.
- **Also touches:** the single fixed 8 MB `push_constant_shadow` array becomes
  per-chunk shadows with dirty ranges; flush bookkeeping moves from "submit
  mid-frame" to "commit at frame boundary".
- **Test plan:** orbit-camera repro (2-10 black frames per 48 at MSAA 4x), then
  `rendering/webgpu/debug_frames` must show zero mid-pass flushes and `RingOF=0`
  on TPS gameplay, plus test_project smoke and an encrypted export boot; final
  sign-off is a real-browser TPS check.
- **Effort:** driver-only, a few hundred lines, but it needs a web-template
  rebuild and browser verification — not something to land unverified.

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

1. ~~**MSAA flicker**~~ — **RESOLVED** (see the section above). Root cause was
   the WebGPU push-constant ring overflowing mid-frame; the flush broke the
   in-pass MSAA resolve. Fixed by an 8 MB ring (plus a one-shot warning).
2. **Push-constant ring flush — OPEN, structurally unhardened.** The 8 MB ring
   is a mitigation, not a fix: an extreme frame (>32768 slots) would still
   flush mid-frame, end the active pass and corrupt its in-pass MSAA resolve.
   Next step: defer flushes to pass boundaries or grow the ring per frame, plus
   a regression test. Keep this flagged in every status report.
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
8. ~~**Release/tag consistency**~~ — **FIXED 2026-11-26.** The 4.8 source asset
   was correct but the tag pointed at `d8cb1fd77c` (the 4.7.2 tree) and the
   release's `targetCommitish` said `4.7.2`. Re-cut the tag to a parentless
   snapshot of the `834c3d4fc2` tree (`35e42f7df2`, 26 MB pack; old value
   `d8cb1fd77c` for reversal) and force-pushed it; `git ls-remote` and the
   GitHub tag object both return `35e42f7df2`. GitHub keeps
   `targetCommitish: 4.7.2` as creation metadata (the API rejects updating it)
   but the release resolves through the tag, so it shows the correct commit.

## Practical notes

- Never run two exports concurrently (they used to stampede the machine; now
  serialized by the tint-cli lock, but still).
- Building/staging inside Dropbox triggers FileProvider/Spotlight churn; keep
  build trees outside Dropbox (`/tmp` or `~`), stage the final app only.
- Tag pushes on `patches/*` were force-pushed historically (branch renamed
  from `4.7.2`/`4.8-beta5`); `.gitignore` on those branches must exclude `out/`
  or a 100 MB app binary gets committed and GitHub rejects the push.
- Release tags can be force-updated too: `4.8-beta5-source` was re-cut
  2026-11-26 (`d8cb1fd77c` → `35e42f7df2`). Verify with
  `git ls-remote origin refs/tags/<tag>` after any move.
- One SDK (Emscripten 6.0.12) serves everything; do not pin a second version.
- The macOS editor build tree `/tmp/godot48` also carries the ring fix and the
  white-box pack-key port; re-apply `patches/4.8-beta5` to a fresh checkout to
  reproduce it.
- `GODOT_VERSION_HASH_OVERRIDE` is set by `tools/grit_build.py` from the editor
  version hash so baked shader cache keys match the runtime template.
