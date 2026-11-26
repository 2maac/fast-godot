# Fast Godot — patch stack

This repository holds **our changes to Godot as patches**, not a copy of the
engine. Each branch targets one Godot version and contains:

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

./tools/apply.sh ../godot 4.7.2      # or: 4.8-beta5
```

Then build the editor / web template as usual (see `docs/PATCH_STACK.md`).

## Rules

- Do not modify upstream `ResourceLoader::_run_load_task()`.
- Built editors are staged to `out/editor-staging/Fast_Godot.app`.
- HTML5 exports are written straight to the Godot Web Server site folder
  (`~/Library/Application Support/Godot/app_userdata/Godot Web Server/site/`).
- Patch order matters; keep the numbering when adding a patch.

## Branches

| branch | Godot base |
|---|---|
| `4.7.2` | `4.7.2-stable` (`ed1daf0bf0`) |
| `4.8-beta5` | `master` at 4.8-beta (`67421f36e5`) |
