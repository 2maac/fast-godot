---
title: CFFI
---

# CFFI

CFFI is an engine module that lets GDScript call C functions in shared libraries directly, and
call back from C into GDScript. It wraps libffi, so the calling convention, struct layout and
variadic arguments are handled for you.

It is always available: no autoload, no plugin, no `preload`. `CFFI` is an engine singleton, and
`CFFILibraryHandle`, `CFFIFunction`, `CFFIType`, `CFFIPointer` and the rest are engine classes.

## Quick start

```gdscript
var lib := CFFI.open("libc.so.6")                       # Linux
# var lib := CFFI.open("/usr/lib/libSystem.B.dylib")     # macOS
# var lib := CFFI.open("msvcrt.dll")                     # Windows

var strlen: CFFIFunction = lib.get_function("strlen", "size_t", ["char*"])
var text := CFFI.null_terminated_utf8_buffer("hello")

print(strlen.invokev([text]))   # 5
```

## Pages

| page | covers |
|---|---|
| [Grit](grit.md) | the export-and-rebuild workflow: generated C++, `grit_generated`, fallback |
| [CFFI](cffi.md) | opening libraries, finding types, structs, callbacks, memory helpers |
| [Type strings](type-strings.md) | how types are written: `"int"`, `"char*"`, `"float[3]"`, `"const char**"` |
| [CFFILibraryHandle](library-handle.md) | `get_function`, `get_global` |
| [CFFIFunction](function.md) | `invokev` |
| [CFFIType](types.md) | type objects, `alloc`, structs, arrays, pointer types |
| [CFFIPointer](pointer.md) | reading and writing memory, strings, field access |
| [CFFISpan](span.md) | bounded views over memory |
| [CFFICallableFunction](callable-function.md) | passing a GDScript Callable to C |
| [StreamPeerCFFIPointer](stream-peer.md) | reading and writing through `StreamPeer` |
| [Examples](examples.md) | complete worked examples |

## Types

Arguments to `invokev` and values returned from it are converted between GDScript `Variant`s and C
types according to the declared type strings. Passing the wrong GDScript type for a declared C type
is a runtime error rather than a silent truncation.
