---
title: CFFIPointer
---

# CFFIPointer

A pointer into C memory. Returned by functions with pointer return types, by `CFFIType.alloc`, by
`CFFISpan.get_data` and `CFFISpan.get_pointer`, and by `CFFI.get_pointer`.

A `CFFIPointer` does not own memory unless it is a [CFFIOwnedValue](#cffiownedvalue) — the two
subclasses that own memory are `CFFIOwnedValue` (one value) and
[CFFIOwnedArray](span.md#cffiownedarray) (an array).

## Reading and writing

```
Variant get_value(int index = 0)
void    set_value(Variant value, int index = 0)
```

`index` counts elements of the pointer's element type, not bytes.

```gdscript
var t := CFFI.find_type("int32_t")
var p := t.alloc_array(3)
p.set_value(10, 0)
p.set_value(20, 2)
print(p.get_value(0), " ", p.get_value(2))   # 10 20
```

## Address and element type

```
int     get_address()
CFFIType get_element_type()
```

`get_address` is the raw address as an integer, which is what you pass to `CFFI.get_function_at`.

```gdscript
print(p.get_address())          # e.g. 140234567890176
print(p.get_element_type().get_name())   # sint32
```

## Pointer arithmetic

```
CFFIPointer offset_by(int64_t offset)
CFFIPointer cast_elements(Variant element_type)
```

`offset_by` counts elements of the current type. `cast_elements` reinterprets the same memory with a
different element type — useful for treating a buffer as bytes or as a struct.

```gdscript
var bytes := CFFI.find_type("uint8").alloc_array(16).get_data()
var ints := bytes.cast_elements("int32_t")
print(ints.get_value(0))
```

## Copying

```
CFFIOwnedValue duplicate()
CFFIOwnedArray duplicate_array(int64_t size)
```

`duplicate` copies a single element into newly owned memory. `duplicate_array` copies `size`
elements and returns an owning span, which is how you keep data that C only borrowed.

```gdscript
var copy := p.duplicate_array(3)    # independent of p
copy.set_value(99, 0)
print(p.get_value(0))               # unchanged
```

## Strings

```
String get_string_from_ascii(int length = -1)
String get_string_from_utf8(int length = -1)
String get_string_from_utf16(int length = -1)
String get_string_from_utf32(int length = -1)
String get_string_from_wchar(int length = -1)
```

Decodes the pointed-to memory as text. With the default `length = -1` it reads until the terminating
zero; pass a length to read a fixed number of code units.

```gdscript
var getenv: CFFIFunction = libc.get_function("getenv", "char*", ["char*"])
var value: CFFIPointer = getenv.invokev([CFFI.null_terminated_utf8_buffer("HOME")])
if value != null:
    print(value.get_string_from_utf8())
```

## Arrays and dictionaries

```
Array to_array(int64_t length)
PackedByteArray      to_byte_array(int64_t length)
PackedInt32Array     to_int32_array(int64_t length)
PackedInt64Array     to_int64_array(int64_t length)
PackedFloat32Array   to_float32_array(int64_t length)
PackedFloat64Array   to_float64_array(int64_t length)
PackedVector2Array   to_vector2_array(int64_t length)
PackedVector3Array   to_vector3_array(int64_t length)
PackedVector4Array   to_vector4_array(int64_t length)
PackedColorArray     to_color_array(int64_t length)
Dictionary           to_dictionary()
```

The typed conversions copy `length` elements into the matching Godot type, which is the fast way to
get C data into the engine. `to_dictionary` reads a struct's fields by name and returns them as a
`Dictionary`.

```gdscript
var samples := CFFI.find_type("float").alloc_array(256)
# ... fill one entry per sample ...
var packed: PackedFloat32Array = samples.to_float32_array(256)
```

## Struct fields

```
CFFIPointer get_field(String field_name)
```

Returns a pointer to a field inside a struct instance, offset according to the struct's layout.

```gdscript
var v := CFFI.define_struct("vec2", {"x": "float", "y": "float"}).alloc()
v.get_field("x").set_value(1.5)
v.get_field("y").set_value(-1.5)
print(v.to_dictionary())    # {x: 1.5, y: -1.5}
```

# CFFIOwnedValue

A `CFFIPointer` that owns the single value it refers to and frees it when the last reference is
released. This is what `CFFIType.alloc` and `CFFIPointer.duplicate` return. It adds no methods; the
point is lifetime.
