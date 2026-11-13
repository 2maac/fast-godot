---
title: Examples
---

# Examples

Every example below was run against a released Fast_Godot build on macOS; the values shown are the
ones it printed.

## Strings with libc

```gdscript
var libc := CFFI.open("/usr/lib/libSystem.B.dylib")   # macOS
# Linux: CFFI.open("libc.so.6")        Windows: CFFI.open("msvcrt.dll")

var strlen: CFFIFunction = libc.get_function("strlen", "size_t", ["char*"])
print(strlen.invokev([CFFI.null_terminated_utf8_buffer("hello")]))
```

```
5
```

Reading a `char*` back as a string:

```gdscript
var getenv := libc.get_function("getenv", "char*", ["char*"])
var home: CFFIPointer = getenv.invokev([CFFI.null_terminated_utf8_buffer("HOME")])
if home != null:
    print(home.get_string_from_utf8())
```

```
/tmp/fakehome
```

## Math

```gdscript
var pow := libc.get_function("pow", "double", ["double", "double"])
print(pow.invokev([2.0, 10.0]))
```

```
1024.0
```

`libm` symbols live in `libSystem` on macOS, `libm.so.6` on Linux and `msvcrt.dll` on Windows; `pow`
is in all three.

## Types

```gdscript
var t := CFFI.find_type("int32_t")
print(t.get_name(), " ", t.get_size(), " ", t.get_alignment())
```

```
sint32 4 4
```

The `_t` spelling resolves to the libffi canonical name, which is what `get_name` reports.

## Allocating an array

```gdscript
var t := CFFI.find_type("int32_t")
var arr := t.alloc_array(3)      # zeroed, owned
arr.set_value(0, 10)
arr.set_value(2, 20)

print(arr.get_value(0), " ", arr.get_value(2))
print(arr.get_length(), " ", arr.get_size_bytes())
print(arr.to_array())
```

```
10 20
3 12
[10, 0, 20]
```

Note the argument order on a span: `set_value(index, value)`. A single
[`CFFIPointer`](pointer.md) uses the opposite order, `set_value(value, index)`.

## Structs

```gdscript
var vec3 := CFFI.define_struct("vec3", {"x": "float", "y": "float", "z": "float"})
print(vec3.get_size(), " ", vec3.offset_of("z"), " ", vec3.type_of("y").get_name())

var v := vec3.alloc()
v.get_field("x").set_value(1.0)
v.get_field("z").set_value(-2.0)
print(v.get_field("z").get_value())
print(v.to_dictionary())
```

```
12 8 float
-2.0
{ &"x": 1.0, &"y": 0.0, &"z": -2.0 }
```

`to_dictionary` returns string-name keys, which is what the `&"x"` prefix in the print means. Index
it as `d["x"]` or `d[&"x"]`.

## Memory helpers

`memcpy`, `memmove`, `memset`, `memcmp` and `memequal` take `CFFIPointer`. A span is not a pointer,
so pass `get_data()`:

```gdscript
var t := CFFI.find_type("uint8")
var a := t.alloc_array(4)
var b := t.alloc_array(4)
b.set_value(0, 7)

print(CFFI.memequal(a.get_data(), b.get_data(), 4))
CFFI.memcpy(b.get_data(), a.get_data(), 4)
print(CFFI.memequal(a.get_data(), b.get_data(), 4))
```

```
false
true
```

## A callback into GDScript

Sorting integers with C's `qsort`, where the comparison function is GDScript:

```gdscript
var t := CFFI.find_type("int32_t")
var data := t.alloc_array(5)
for i in 5:
    data.set_value(i, [5, 3, 9, 1, 7][i])

var compare := CFFI.create_function(
    func(pa, pb) -> int:
        return pa.cast_elements("int32_t").get_value() - pb.cast_elements("int32_t").get_value(),
    "int",
    ["void*", "void*"]
)

var qsort := libc.get_function("qsort", "void", ["void*", "size_t", "size_t", "void*"])
qsort.invokev([data.get_data(), 5, 4, compare])

print(data.to_array())
```

```
[1, 3, 5, 7, 9]
```

Three details in that example are not obvious, and each of them is a real error if you get it wrong:

- **Declare C parameters as `"void*"`, not `"pointer"`.** The `pointer` base type cannot be
  converted from a `CFFIPointer` argument; you get `Pointer type is not supported`.
- **Leave the callback parameters untyped.** Writing `func(pa: CFFIPointer, pb: CFFIPointer)` makes
  the callback receive `Nil` for both arguments. Untyped parameters receive the `CFFIPointer`
  objects correctly.
- **Cast before reading.** `qsort` passes `void*`, which has no element type, so `get_value()`
  returns `Nil`; `cast_elements("int32_t")` gives the pointer a type first.

The `compare` object must stay referenced for as long as C can call it — here it lives for the scope
of the function, which is enough because `qsort` is synchronous. For a callback C stores and calls
later, keep it on a member, as described in
[CFFICallableFunction](callable-function.md#lifetime).

## Getting a pointer to Godot data

`CFFI.get_pointer` points at a `PackedByteArray`'s storage without copying, so C can write into it
and Godot sees the result:

```gdscript
var buffer := PackedByteArray()
buffer.resize(8)
var p := CFFI.get_pointer(buffer)
p.set_value(65, 0)
p.set_value(66, 1)
print(buffer.slice(0, 2).get_string_from_utf8())
```

```
AB
```

The Variant must outlive the pointer: `CFFI.get_pointer` borrows, it does not copy.
