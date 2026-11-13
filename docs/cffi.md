---
title: CFFI
---

# CFFI

The `CFFI` singleton. It is the entry point: opens libraries, resolves type names, declares
structs, and creates functions that call back into GDScript.

## open

```
CFFILibraryHandle open(String name_or_path)
```

Loads a shared library and returns a handle, or `null` if it cannot be loaded. `name_or_path` may be
a full path or a bare library name, which is searched in the platform's usual locations and with the
platform's usual suffix (`.so` on Linux, `.dylib` on macOS, `.dll` on Windows).

```gdscript
var libc := CFFI.open("libc.so.6")                        # Linux
var libc := CFFI.open("/usr/lib/libSystem.B.dylib")       # macOS
var libc := CFFI.open("msvcrt.dll")                       # Windows

if libc == null:
    push_error("library not found")
```

See [CFFILibraryHandle](library-handle.md) for what you do with it.

## find_type

```
CFFIType find_type(String name)
```

Resolves a [type string](type-strings.md) or a previously defined struct name to a `CFFIType`. Returns
`null` for an unknown name.

```gdscript
var t := CFFI.find_type("int[4]")
print(t.get_name(), " ", t.get_size())   # int[4] 16
```

## define_struct

```
CFFIStructType define_struct(String name, Dictionary fields)
```

Declares a struct from a dictionary of `field name -> type`. Field order is the dictionary's
insertion order, which is the memory layout. The name becomes usable as a type string afterwards.

```gdscript
var vec3 := CFFI.define_struct("vec3", {"x": "float", "y": "float", "z": "float"})
print(vec3.get_size())          # 12
print(vec3.offset_of("z"))      # 8
```

## create_function

```
CFFICallableFunction create_function(Callable callable, Variant return_type, Array argument_types = [])
```

Wraps a GDScript `Callable` as a C function pointer, for passing into C APIs that take callbacks
(`qsort`, event hooks, completion handlers).

```gdscript
var compare := CFFI.create_function(
    func(a: int, b: int) -> int: return a - b,
    "int",
    ["void*", "void*"]
)
```

See [CFFICallableFunction](callable-function.md) for keeping it alive while C holds it.

## get_function_at

```
CFFIFunction get_function_at(int pointer, Variant return_type, Array argument_types = [], bool is_variadic = false)
```

Builds a callable from a raw function address, for functions that are returned by other functions
rather than exported by name.

```gdscript
var getter := lib.get_function("get_callback", "pointer", [])
var address: int = getter.invokev([])
var callback := CFFI.get_function_at(address, "void", ["int"])
```

## get_scope

```
CFFIScope get_scope()
```

Returns the global scope, the object that resolves type names and holds struct definitions. Useful
when you want to pass that scope around explicitly; `CFFI` itself already behaves as one.

## get_pointer

```
CFFIPointer get_pointer(Variant string_or_packed_array)
```

Returns a pointer to the contents of a `PackedByteArray`, or to a string. This does **not** copy: it
points at the Variant's own storage, so the Variant must outlive the pointer.

```gdscript
var bytes := PackedByteArray([1, 2, 3, 4])
var ptr := CFFI.get_pointer(bytes)
print(ptr.get_value(2))    # 3
```

For a copy that you own, use `CFFIType.alloc_array` or `CFFIPointer.duplicate_array` instead.

## Memory helpers

These mirror the C functions of the same name and take `CFFIPointer` arguments.

```
CFFIPointer memcpy(CFFIPointer dest, CFFIPointer src, int64_t size_bytes)
CFFIPointer memmove(CFFIPointer dest, CFFIPointer src, int64_t size_bytes)
CFFIPointer memset(CFFIPointer dest, int byte_value, int64_t size_bytes)
int         memcmp(CFFIPointer s1, CFFIPointer s2, int64_t size_bytes)
bool        memequal(CFFIPointer s1, CFFIPointer s2, int64_t size_bytes)
```

They take `CFFIPointer`, not `CFFISpan`, so pass `span.get_data()`:

```gdscript
var t := CFFI.find_type("uint8")
var a := t.alloc_array(4)
var b := t.alloc_array(4)
b.set_value(0, 7)

print(CFFI.memequal(a.get_data(), b.get_data(), 4))   # false
CFFI.memcpy(b.get_data(), a.get_data(), 4)
print(CFFI.memequal(a.get_data(), b.get_data(), 4))   # true
```

## Null-terminated buffers

```
static PackedByteArray null_terminated_ascii_buffer(String str)
static PackedByteArray null_terminated_utf8_buffer(String str)
static PackedByteArray null_terminated_utf16_buffer(String str)
static PackedByteArray null_terminated_utf32_buffer(String str)
static PackedByteArray null_terminated_wchar_buffer(String str)
```

Encode a GDScript string into a `PackedByteArray` with a terminating zero, ready to pass to a C
function that expects `char*` or `wchar_t*`.

```gdscript
var puts: CFFIFunction = libc.get_function("puts", "int", ["char*"])
puts.invokev([CFFI.null_terminated_utf8_buffer("hello from CFFI")])
```

Use the width that matches the callee: `utf8`/`ascii` for `char*`, `utf16`/`utf32`/`wchar` for wide
character APIs. On Windows `wchar_t` is 16 bits, on Linux and macOS it is 32.
