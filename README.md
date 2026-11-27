# Fast Godot — patch stack

This repository holds **our changes to Godot as patches**, not a copy of the
engine. There is a single active stack (`4.8-beta5`), stored directly at the
repository root:

```
patches/                      # self-contained portable stack bundle:
  *.patch                     #   ordered .patch files (git format)
  layers/                     #   path lists used to regenerate the stack
  upstream.lock               #   Godot repo + exact commit the stack applies to
tools/apply.sh                # apply the stack to a Godot checkout
tools/make-patches.sh         # regenerate the stack from a modified checkout
docs/PATCH_STACK.md           # what every patch does and why
docs/WEBGPU_HANDOFF.md        # primary handoff (start here)
docs/WEBGPU_DRIVER_MIGRATION.md  # archived 4.7 driver history
```

The Godot source itself is a link, not content: clone it from
<https://github.com/godotengine/godot> at the commit pinned in `upstream.lock`.

**Start here:** [`docs/WEBGPU_HANDOFF.md`](docs/WEBGPU_HANDOFF.md) — current
state, build/export flow, MSAA/ring status, encryption and open work.

Both renderers run on the WebGPU device: **Forward Mobile** and, since
2026-11-27, **Forward+** — the TPS demo exported through HTML5 Fast renders the
full 3D level with 0 WebGPU validation errors, 0 SPIR-V→WGSL failures and 60 fps
in Chrome/Dawn ([screenshot](docs/webgpu_forward_plus_tps.png)).

## Quick start

```sh
git clone https://github.com/godotengine/godot ../godot
git -C ../godot checkout $(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' patches/upstream.lock)

./tools/apply.sh ../godot
```

Then build the editor / web template as usual (see `docs/PATCH_STACK.md`).

## Rules

- Do not modify upstream `ResourceLoader::_run_load_task()`.
- Built editors are staged to `out/editor-staging/Fast_Godot_4.8.app`.
- HTML5 exports are written straight to the Godot Web Server site folder
  (`~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/`).
- Patch order matters; keep the numbering when adding a patch.

## Branch

| branch | Godot base |
|---|---|
| `4.8-beta5` branch and tag (self-contained stack bundle in `patches/`) | godot master `67421f36e5` (4.8.0-beta) |

`4.8-beta5` is the repository default branch (since 2026-11-26; it was `4.7.2`).
The `4.7.2` full-source branch is kept on the remote for history only (not
maintained): `origin/4.7.2` = `d8cb1fd77c`. A leftover local branch `fresh`
points at the same commit and can be deleted.

## Documentation

| doc | what it is |
|---|---|
| [`docs/WEBGPU_HANDOFF.md`](docs/WEBGPU_HANDOFF.md) | **primary handoff** — current state, build/export flow, MSAA/ring status, encryption, open work |
| [`docs/PATCH_STACK.md`](docs/PATCH_STACK.md) | what every patch layer contains and how to apply/regenerate |
| [`docs/WEBGPU_DRIVER_MIGRATION.md`](docs/WEBGPU_DRIVER_MIGRATION.md) | archived history of the komm64 WebGPU driver adoption (4.7) |
