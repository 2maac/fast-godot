---
title: CFFIType
---

# CFFIType

A resolved C type. Get one from [CFFI.find_type](cffi#find_type), or from
[CFFI.define_struct](cffi#define_struct). It knows its size, its alignment, and how to allocate
memory for itself.

```gdscript
var int_type := CFFI.find_type("int")
print(int_type.get_name())        # int
print(int_type.get_size())        # 4
print(int_type.get_alignment())   # 4
```

## Methods

```
String  get_name()
int64_t get_size()
int64_t get_alignment()
CFFIOwnedValue alloc(bool initialize_with_zeros = true)
CFFIOwnedArray alloc_array(int64_t length, bool initialize_with_zeros = true)
```

`alloc` returns a pointer to one newly allocated value; `alloc_array` returns a span over `length`
values. Both own their memory and free it when the last reference goes away, so you do not call
`free` yourself.

```gdscript
var t := CFFI.find_type("double")
var one := t.alloc()
one.set_value(3.5)
print(one.get_value())        # 3.5

var many := t.alloc_array(4)  # zeroed
many.set_value(1, 0)
print(many.get_length())      # 4
print(many.to_array())        # [1.0, 0.0, 0.0, 0.0]
```

# CFFIStructType

A struct type. `define_struct` returns one.

```
CFFIType type_of(String field_name)
int64_t  offset_of(String field_name)
```

```gdscript
var vec3 := CFFI.define_struct("vec3", {"x": "float", "y": "float", "z": "float"})
print(vec3.get_size())          # 12
print(vec3.type_of("y").get_name())   # float
print(vec3.offset_of("z"))      # 8

var v := vec3.alloc()
v.get_field("x").set_value(1.0)
v.get_field("z").set_value(-2.0)
print(v.get_field("z").get_value())   # -2.0
```

Layout follows C rules for the target platform, including padding, so a struct defined here matches
the same struct in C.

# CFFIArrayType

An array type, produced by a type string such as `"int[4]"`.

```
CFFIType get_element_type()
int64_t  get_length()
static CFFIArrayType from(Variant type, int64_t length)
```

```gdscript
var arr := CFFIArrayType.from("float", 3)
print(arr.get_length())                  # 3
print(arr.get_element_type().get_name()) # float
print(arr.get_size())                    # 12
```

# CFFIPointerType

A pointer type, produced by a type string such as `"char*"`.

```
CFFIType get_element_type()
static CFFIPointerType from(Variant type)
```

```gdscript
var p := CFFIPointerType.from("char")
print(p.get_size())                      # 8 on 64-bit
print(p.get_element_type().get_name())   # char
```
