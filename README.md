# Fast Godot — patch stack

This repository holds **our changes to Godot as patches**, not a copy of the
engine. There is a single active stack (`4.8-beta5`):

```
upstream.lock                 # Godot repo + exact commit the stack applies to
patches/<godot-version>/      # ordered .patch files (git format)
patches/<godot-version>/layers/  # path lists used to regenerate the stack
tools/apply.sh                # apply the stack to a Godot checkout
tools/make-patches.sh         # regenerate the stack from a modified checkout
docs/PATCH_STACK.md           # what every patch does and why
```

The Godot source itself is a link, not content: clone it from
<https://github.com/godotengine/godot> at the commit pinned in `upstream.lock`.

## Quick start

```sh
git clone https://github.com/godotengine/godot ../godot
git -C ../godot checkout $(sed -n 's/^godot_commit *= *"\(.*\)"/\1/p' upstream.lock)

./tools/apply.sh ../godot 4.8-beta5
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
| `4.8-beta5` branch and tag (patch files live in `patches/4.8-beta5/`) | godot master `67421f36e5` (4.8.0-beta) |

`4.8-beta5` is the repository default branch (since 2026-11-26; it was `4.7.2`).
The `4.7.2` full-source branch is kept on the remote for history only (not
maintained): `origin/4.7.2` = `d8cb1fd77c`. A leftover local branch `fresh`
points at the same commit and can be deleted.

## Documentation

| doc | what it is |
|---|---|
| `WEBGPU_HANDOFF.md` | **primary handoff** — current state, build/export flow, MSAA fix, encryption, open work |
| `docs/PATCH_STACK.md` | what every patch layer contains and how to apply/regenerate |
| `WEBGPU_DRIVER_MIGRATION.md` | archived history of the komm64 WebGPU driver adoption (4.7) |
