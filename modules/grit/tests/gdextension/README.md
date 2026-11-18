# GDExtension conversion fixtures

End-to-end checks for the script → generated GDExtension class path. Each
fixture is a small Godot project that the driver copies to a temporary
directory, converts with the staged editor and runs headless:

| fixture | what it covers |
|---|---|
| `plain` | a class with plain methods: properties, a signal, loops, a helper method |
| `cast` | `is`/`as` for builtins, native classes, typed arrays and typed dictionaries; `assert`; built-in statics (`Color.from_hsv`, `Vector2.from_angle`); `super` calls; optional parameters; static functions |
| `lambda` | lambdas with captures, passed to `callable`-taking APIs and called directly |

`await` is not converted: a GDExtension method cannot suspend and resume, so a
script that awaits has that method reported instead (see HANDOFF §22).

## Running

```
modules/grit/tests/gdextension/run_tests.py \
    --editor out/editor-staging/Fast_Godot.app/Contents/MacOS/Godot \
    --godot-cpp /path/to/godot-cpp \
    --scons /path/to/scons
```

`--godot-cpp` and `--scons` fall back to `GRIT_GDX_GODOT_CPP` / `GRIT_GDX_SCONS`.
`--only NAME` runs one fixture, `--keep` leaves the converted project behind,
`--timeout` (default 600 s) bounds each step.

The driver checks that the extension library was actually built before running
the project: without it the project silently falls back to the GDScript VM and
every assertion would still pass.

## Fixture layout

```
fixtures/<name>/
  project.godot, *.gd, *.tscn   the project (converted in a copy, never in place)
  fixture.json                  extension name, script, scene, extra run arguments
  expected.txt                  lines that must appear, in order (substring match)
  forbidden.txt                 optional: lines that must not appear
```

A conversion that cannot translate a method is refused by default; the fixtures
are all fully convertible so a regression shows up as a failed conversion rather
than as a stub.

## Notes

- Verified on macOS, on Linux (WSL: the editor is built from the commit with the
  bundled GCC 9 toolchain, godot-cpp built by the conversion, ~6m20s per editor
  build) and on Windows (generated sources built with the Windows toolchain and
  run under the 4.7.2 release editor).
- Run a verification from a **committed tree**: `git archive HEAD | tar -x -C
  <dir>`, or a clean worktree. Copying a working tree while it is being edited
  produces a mix of commits, which once made a "verified" run mean nothing (the
  copy was detected by comparing blob hashes with the commit).

- Conversions run with a throwaway `HOME` so the editor dumps its extension API
  inside the temporary directory. Building the extension needs a godot-cpp
  checkout the process may write to.
- The fixtures are copied to a temp directory because converting rewrites the
  scene, archives the script as a detached `.gds` next to it and writes
  `gdext/`. The converted node becomes the script's `class_name` and has no script
  attached, so a new `.gd` can be added later.
- Converting on one host writes only that host's library entries into the
  descriptor. To run the same converted project on another platform, add the
  matching `platform.debug`/`platform.release` lines and build the generated
  sources where that toolchain lives (the sources themselves are portable).
