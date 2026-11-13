---
title: Type strings
---

# Type strings

Wherever a type is expected you can pass a **string**, a `CFFIType` object, or (for structural
types) a `Dictionary` of fields. Type strings are parsed with the same rules every time.

## Base names

Built in, available without declaring anything:

| group | names |
|---|---|
| void | `void` |
| signed integers | `sint8` `sint16` `sint32` `sint64`, `int8_t` … `int64_t`, `char`, `short`, `int`, `long`, `long long` |
| unsigned integers | `uint8` `uint16` `uint32` `uint64`, `uint8_t` … `uint64_t`, `unsigned char`, `unsigned short`, `unsigned int` |
| floating point | `float`, `double`, `long double` |
| complex | `complex float`, `complex double`, `complex longdouble` |
| others | `bool`, `pointer`, `size_t`, `ssize_t`, `intptr_t`, `uintptr_t`, `char16_t`, `char32_t` |

The libffi spellings (`uint8`, `sint32`, …) and the `_t` spellings (`uint8_t`, …) are separate
names that resolve to the same type.

## Pointers and arrays

```
"int"        int
"int *"      pointer to int
"int **"     pointer to pointer to int
"int[4]"     array of 4 int
"int[3][4]"  array of 3 arrays of 4 int
"int[ 4 ]"   whitespace inside the brackets is fine
"int[]"      a POINTER, not a zero-length array
"int[0]"     a zero-length (flexible) array member
```

`int[]` meaning pointer is deliberate: it is how C function signatures spell an unsized array
parameter.

## Qualifiers

`const` and `volatile` are accepted as prefixes and ignored for layout:

```
"const int"
"volatile int *"
"const char **"
```

A trailing qualifier (`"int *const"`) does not parse. `const` and `volatile` are matched as bare
prefixes without a word boundary, so a name beginning with those letters can be misread — `"constant"`
parses as the identifier `ant`. Declare such names with a distinct spelling.

## Structs

Declare a struct once, then use its name:

```gdscript
var vec3 := CFFI.define_struct("vec3", {"x": "float", "y": "float", "z": "float"})
var length: CFFIType = CFFI.find_type("vec3")
```

Or pass a `Dictionary` where a type is accepted, which defines the struct inline.
