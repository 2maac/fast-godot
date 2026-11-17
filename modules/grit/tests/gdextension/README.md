# GDExtension conversion fixtures

End-to-end checks for the script → generated GDExtension class path. Each
fixture is a small Godot project that the driver copies to a temporary
directory, converts with the staged editor and runs headless:

| fixture | what it covers |
|---|---|
| `plain` | a class with no coroutines: properties, a signal, loops, a helper method |
| `await` | `await` on signals, timers and frames; loops and nested loops across suspensions; concurrent coroutines; two waiters on one signal; script signals with one and two arguments; awaiting a GDScript coroutine; `await` on a non-signal value; a plain method in the same class |
| `free` | a node freed while its coroutine is suspended (`--quit-after`, the state must not resume) |
| `cast` | `is`/`as` for builtins, native classes, typed arrays and typed dictionaries; `assert`; built-in statics (`Color.from_hsv`, `Vector2.from_angle`); `super` calls |

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

- Conversions run with a throwaway `HOME` so the editor dumps its extension API
  inside the temporary directory. Building the extension needs a godot-cpp
  checkout the process may write to.
- The fixtures are copied to a temp directory because converting rewrites the
  scene, archives the script as `.gde` and writes `gdext/`.
- Converting on one host writes only that host's library entries into the
  descriptor. To run the same converted project on another platform, add the
  matching `platform.debug`/`platform.release` lines and build the generated
  sources where that toolchain lives (the sources themselves are portable).
