---
title: Grit
---

# Grit

Grit compiles GDScript to C++ **at export time** and links that C++ into your export template, so
scripts run as native code instead of going through the interpreter.

The consequence worth understanding before anything else: **there is no such thing as a Grit
template that ships with the engine**. The generated C++ is specific to your project, so a template
becomes Grit-optimised only when you build one with your project's generated code. The editor and
template downloads in the releases are ordinary builds — a Grit game exported on top of them runs in
the VM.

Grit's GDScript-level behaviour is documented upstream at
https://github.com/DasDarki/grit; this page covers only the workflow in Fast_Godot.

## Export modes

The export preset has a **Grit Export Mode** option with four choices, replacing the usual
debug/release export button:

| mode | what it does |
|---|---|
| **VM Debug** | stock export, stock debug template, no code generation |
| **VM Release** | stock export, stock release template, no code generation |
| **Compiled Debug** | generates C++, builds a debug template from it, re-runs the export with it |
| **Compiled Release** | same, for release |

The compiled modes need the engine source tree to build a template from. Set the project setting
`grit/export/engine_root` to the Fast_Godot checkout that contains the built engine objects, and make
sure the per-target manifest exists:

```sh
tools/grit_make_manifest.sh <engine_root> release   # after building template_release
tools/grit_make_manifest.sh <engine_root> debug     # after building template_debug
```

The mode then performs the whole flow in one action: generate the C++, compile just that code, relink
against the prebuilt engine objects, and re-run the export with the resulting template.

## Step 1 — export from the editor

Open your project in Fast_Godot and export it (Project → Export). Grit runs as part of the export
and writes the generated C++ to the directory in the project setting:

```
grit/export/generated_directory     default: res://.godot/grit/generated
```

The export log prints where it wrote, and what to do next:

```
Grit: generated C++ written to "<path>". Rebuild the export template with grit_generated pointing there.
```

Nothing is compiled at this point. The export has merely produced sources.

## Step 2 — build a template with that code

```sh
scons platform=macos    target=template_release grit_generated=<project>/.godot/grit/generated
scons platform=linuxbsd target=template_release grit_generated=<project>/.godot/grit/generated
scons platform=windows  target=template_release grit_generated=<project>/.godot/grit/generated
```

Point your export preset at the resulting binary through `custom_template/release` (and
`custom_template/debug` for a debug build), then export again — this time the export uses a template
that actually contains your compiled scripts.

For a debuggable build, use `target=template_debug`. Breakpoints and stepping in native code are
implemented for debug templates.

## The fallback

Without `grit_generated`, the build compiles `modules/grit/fallback/` instead: a VM-backed
implementation of the same entry points. That is what the prebuilt release templates contain, and it
is why they run Grit-exported projects correctly but no faster than stock Godot.

This is a deliberate design, not a silent degradation — you opt into native code by passing
`grit_generated`, and the fallback keeps the editor and templates usable for everyone else.

## If the directory is wrong

`grit_generated` must point at a directory containing `grit_registry.gen.cpp`. If it does not, the
build stops rather than falling back quietly:

```
Grit: "<directory>" does not contain generated code. Export the project first.
```

That message means step 1 hasn't been run, or the project setting points somewhere else — note that
the default lives inside `.godot/`, so deleting that folder discards the generated code and requires
a fresh export.

## Conformance flag

```
grit_conformance=yes
```

Always activates generated code regardless of how the build is invoked. It exists for running
Godot's own GDScript test suite natively, and is not needed for normal project builds.
