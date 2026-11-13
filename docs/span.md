---
title: CFFISpan
---

# CFFISpan

A pointer plus a length: a bounded view over C memory. Returned by `CFFIType.alloc_array`,
`CFFISpan.subspan`, `CFFISpan.duplicate` and `CFFIPointer.duplicate_array`.

Because it knows its length, the `to_*_array` conversions need no argument, and bounds are checked.

```gdscript
var t := CFFI.find_type("float")
var samples := t.alloc_array(8)      # CFFIOwnedArray, which is a span
print(samples.get_length())          # 8
print(samples.get_size_bytes())      # 32
print(samples.is_empty())            # false
```

## Methods

```
CFFIPointer get_data()
CFFIType    get_element_type()
int64_t     get_length()
int64_t     get_size_bytes()
bool        is_empty()
CFFISpan    subspan(int from, int count = -1)
CFFIPointer get_pointer(int index)
CFFIOwnedArray duplicate()
```

`subspan` and `get_pointer` are how you walk a buffer without recomputing offsets by hand — `count = -1`
means "to the end".

```gdscript
var head := samples.subspan(0, 4)
var tail := samples.subspan(4)
print(head.get_length(), " ", tail.get_length())   # 4 4

var third := samples.get_pointer(2)
third.set_value(0.5)
print(samples.get_value(2))    # 0.5
```

## Reading and writing

```
Variant get_value(int index)
void    set_value(int index, Variant value)
```

Note the argument order differs from [CFFIPointer](pointer.md): a span puts the index first.

```gdscript
samples.set_value(0, 1.0)
print(samples.get_value(0))    # 1.0
```

## Strings

```
String get_string_from_ascii()
String get_string_from_utf8()
String get_string_from_utf16()
String get_string_from_utf32()
String get_string_from_wchar()
```

Decode the whole span as text. A span that is not zero-terminated reads its full length, so use the
correct width for the data.

```gdscript
var bytes := CFFI.find_type("uint8").alloc_array(5)
for i in 5:
    bytes.set_value("hello".unicode_at(i), i)
print(bytes.get_string_from_utf8())    # hello
```

## Conversion

```
Array                to_array()
PackedByteArray      to_byte_array()
PackedInt32Array     to_int32_array()
PackedInt64Array     to_int64_array()
PackedFloat32Array   to_float32_array()
PackedFloat64Array   to_float64_array()
PackedVector2Array   to_vector2_array()
PackedVector3Array   to_vector3_array()
PackedVector4Array   to_vector4_array()
PackedColorArray     to_color_array()
```

Each copies the entire span into the matching Godot array type. Reading audio samples or vertex data
out of C is one call.

```gdscript
var interleaved := CFFI.find_type("float").alloc_array(6)
# ... fill 3 x (x, y) ...
var points: PackedVector2Array = interleaved.to_vector2_array()
```

## Creating one

```
static CFFISpan from(CFFIPointer pointer, int64_t count)
```

Wraps an existing pointer with a length, without copying. Use it to hand a C pointer and size to the
span-based helpers.

```gdscript
var buf: CFFIPointer = malloc.invokev([4096])
var view := CFFISpan.from(buf, 4096)
print(view.get_size_bytes())    # 4096
```

# CFFIOwnedArray

A `CFFISpan` that owns its memory and frees it when the last reference is released. This is what
`CFFIType.alloc_array`, `CFFISpan.duplicate` and `CFFIPointer.duplicate_array` return.

`CFFISpan.from` does **not** own anything: the memory it views must outlive the span.
