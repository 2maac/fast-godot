---
title: CFFILibraryHandle
---

# CFFILibraryHandle

A loaded shared library. Returned by [CFFI.open](cffi.md#open); declares nothing by itself.

```gdscript
var libm := CFFI.open("libm.so.6")
var pow: CFFIFunction = libm.get_function("pow", "double", ["double", "double"])
```

## get_function

```
CFFIFunction get_function(String name, Variant return_type, Array argument_types = [], bool is_variadic = false)
```

Looks up an exported symbol and builds a callable from it. Returns `null` if the symbol does not
exist, which is the cheapest way to feature-detect at runtime.

- `name` — the exported symbol name, exactly as C exports it.
- `return_type` — a [type string](type-strings.md) or `CFFIType`.
- `argument_types` — one entry per parameter, in order.
- `is_variadic` — set `true` when the C function takes `...`; pass the fixed parameters in
  `argument_types` and the rest in the `invokev` argument list.

```gdscript
var printf := libc.get_function("printf", "int", ["char*"], true)
printf.invokev([CFFI.null_terminated_utf8_buffer("%d %s\n"), 42,
                CFFI.null_terminated_utf8_buffer("items")])
```

## get_global

```
CFFIPointer get_global(String name, Variant type)
```

Returns a pointer to an exported global variable, so you can read and write it.

```gdscript
var errno := libc.get_global("errno", "int")
if errno != null:
    print(errno.get_value())
    errno.set_value(0)
```

The pointer refers to the library's storage; it stays valid while the library is loaded.
