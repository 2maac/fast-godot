---
title: CFFIFunction
---

# CFFIFunction

A C function bound to a type signature. Returned by `CFFILibraryHandle.get_function`,
`CFFI.get_function_at`, or built from a `Callable` as a
[CFFICallableFunction](callable-function.md).

## invokev

```
Variant invokev(Array arguments)
```

Calls the function. Arguments are converted from GDScript values to the declared C parameter types,
and the C return value is converted back to a `Variant`.

```gdscript
var libm := CFFI.open("libm.so.6")
var pow := libm.get_function("pow", "double", ["double", "double"])
print(pow.invokev([2.0, 10.0]))    # 1024.0

var abs := libm.get_function("fabs", "double", ["double"])
print(abs.invokev([-7.5]))         # 7.5
```

Return values follow the declared type: `void` returns `null`, integers return `int`, `float` and
`double` return `float`, `bool` returns `bool`, and pointer types return a
[CFFIPointer](pointer.md).

```gdscript
var strdup: CFFIFunction = libc.get_function("strdup", "char*", ["char*"])
var copy: CFFIPointer = strdup.invokev([CFFI.null_terminated_utf8_buffer("text")])
print(copy.get_string_from_utf8())    # text
copy.duplicate()                      # only frees if you also bind and call free()
```

## Arity and types are checked

Passing the wrong number of arguments, or a GDScript value that does not convert to the declared C
type, is an error rather than undefined behaviour. That check is the main reason to declare the
signature accurately instead of using `"pointer"` everywhere.
