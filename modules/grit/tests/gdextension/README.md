# GDExtension fixtures

End-to-end checks for the `.gde` source → project GDExtension class path. Each
fixture is a small Godot project that the driver copies to a temporary directory,
builds with the staged editor and runs headless:

| fixture | what it covers |
|---|---|
| `plain` | a class with plain methods: properties, a signal, loops, a helper method |
| `cast` | `is`/`as` for builtins, native classes, typed arrays and typed dictionaries; `assert`; built-in statics (`Color.from_hsv`, `Vector2.from_angle`); `super` calls; optional parameters; static functions |
| `lambda` | lambdas with captures, passed to `callable`-taking APIs and called directly |

`await` is not converted: a GDExtension method cannot suspend and resume, so a
script that awaits has that method reported instead (see HANDOFF §22).

## The flow under test

A `.gde` file is a **class source**, not a script: `class_name X` plus
`extends Base` declares the generated class. It is inert — the engine never
compiles, instantiates or executes it — so the fixture scene already uses the
class as its root node's type and attaches no script:

```
[gd_scene format=3]

[node name="Main" type="PlainMain"]
```

All `.gde` sources in a project share one extension at `res://gdext/`, named
after the project (`<ProjectName>` is `config/name` from `project.godot`,
sanitized to an identifier):

```
res://gdext/<ProjectName>.gdextension
res://gdext/bin/lib<ProjectName>.<platform>.<arch>.debug.<ext>
res://gdext/src/...
res://gdext/.grit_manifest.json
```

The driver builds exactly that, with:

```
<editor> --headless -e --path <copy> \
    --grit-gdx-generate res://main.gde --grit-gdx-build
```

`--grit-gdx-generate` scans every `.gde` in the project; the scene is never read
or rewritten and no `.gds` snapshot is produced.

## Running

```
modules/grit/tests/gdextension/run_tests.py \
    --editor out/editor-staging/Fast_Godot.app/Contents/MacOS/Godot \
    --godot-cpp /path/to/godot-cpp \
    --scons /path/to/scons
```

`--godot-cpp` and `--scons` fall back to `GRIT_GDX_GODOT_CPP` / `GRIT_GDX_SCONS`.
`--only NAME` runs one fixture, `--keep` leaves the built project behind,
`--timeout` (default 600 s) bounds each step. All fixtures share one `HOME` per
run, so the editor's extension API dump lives in one place instead of one per
fixture.

## What the driver checks

The extension has to be built for real: the scene's `type="X"` only resolves if
the project extension loaded, so the driver first verifies the generated state
and refuses to run the project without a library.

- the `.gde` source exists and its `class_name X` is the generated class;
- the scene's root node has `type="X"` (the class parsed from the source);
- the scene does not reference the `.gde` (`path="res://main.gde"` is absent) and
  the root node has no `script = ExtResource(...)` line (the `cast` fixture's
  `Helper` node keeps its `helper.gd` script);
- `res://gdext/<ProjectName>.gdextension` exists, where `<ProjectName>` is the
  project's `config/name` sanitized the way the build does it, and the class
  appears in the descriptor (`[icons]`) or the manifest;
- the manifest entry for the class has `script == "res://main.gde"` and
  `source_hash == sha256(<the .gde file>)`;
- a library exists under `res://gdext/bin/` before the project runs;
- the headless run prints every `expected.txt` line in order, at most as often
  as expected (the `.gde` is never executed, so a doubled line means something
  ran twice), and no `forbidden.txt` line.

## Fixture layout

```
fixtures/<name>/
  project.godot, *.gde, *.tscn   the project (built in a copy, never in place)
  fixture.json                  script, scene, extra run arguments
  expected.txt                  lines that must appear, in order (substring match)
  forbidden.txt                 optional: lines that must not appear
```

A conversion that cannot translate a method is refused by default; the fixtures
are all fully convertible so a regression shows up as a failed build rather than
as a stub.

## Notes

- Builds run with a throwaway `HOME` per suite run so the editor dumps its
  extension API inside the temporary directory. Building the extension needs a
  godot-cpp checkout the process may write to.
- The fixtures are copied to a temp directory because the build writes
  `res://gdext/` (sources, manifest, descriptor, library) into the project. The
  `.gde` sources and the scenes are left exactly as committed.
- The descriptor is published only after its library exists, so a failed build
  leaves no half-usable extension behind.
- Verified on macOS with the staged editor (`classes=1079`). The Linux (WSL) and
  Windows runs predate the `.gde` rewrite and need to be repeated.
- Run a verification from a **committed tree**: `git archive HEAD | tar -x -C
  <dir>`, or a clean worktree. Copying a working tree while it is being edited
  produces a mix of commits, which once made a "verified" run mean nothing (the
  copy was detected by comparing blob hashes with the commit).
