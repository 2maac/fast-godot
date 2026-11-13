# Third-party notices

## Godot Engine

Copyright (c) 2014-present Godot Engine contributors.
Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.
Licensed under the MIT License. See `LICENSE.txt`.

## Grit (`modules/grit`)

Ahead-of-time GDScript compiler for Godot.
Upstream: https://github.com/DasDarki/grit — MIT License.

Modifications in this repository: none beyond the engine patch in `patches/`.

## CFFI (`modules/cffi`)

C foreign-function interface for GDScript.
Derived from https://github.com/agorangetek/cffi-gdextension — The Unlicense.

Modifications: converted from a GDExtension into an engine module (engine class registration,
plain filesystem library paths instead of a custom `.ffilibrary` resource format, system libffi
instead of a vendored build).
