---
title: CFFICallableFunction
---

# CFFICallableFunction

A function pointer backed by a GDScript `Callable`, created with
[CFFI.create_function](cffi#create_function). Pass it to C where a callback is expected.

```gdscript
var on_event := CFFI.create_function(
    func(code: int) -> void:
        print("event ", code),
    "void",
    ["int"]
)
```

## Methods

```
Callable get_callable()
void     set_callable(Callable value)
```

`set_callable` swaps the GDScript callable behind the existing pointer, which avoids re-creating the
trampoline when the handler changes.

```gdscript
on_event.set_callable(func(code: int) -> void: print("changed ", code))
```

## Callback parameters must be untyped

A `Callable` with typed parameters receives `Nil` for its arguments:

```gdscript
# Wrong: pa and pb arrive as Nil.
CFFI.create_function(func(pa: CFFIPointer, pb: CFFIPointer) -> int: ..., "int", ["void*", "void*"])

# Right: leave them untyped and cast inside.
CFFI.create_function(func(pa, pb) -> int:
    return pa.cast_elements("int32_t").get_value() - pb.cast_elements("int32_t").get_value(),
    "int", ["void*", "void*"])
```

A `void*` has no element type, so `get_value()` returns `Nil` until you `cast_elements` to something.

## Lifetime

The trampoline is owned by the `CFFICallableFunction` object. **Keep a reference** for as long as C
can call it: if the object is freed, C is left holding a pointer to a released trampoline.

```gdscript
var handler := CFFI.create_function(func() -> void: print("tick"), "void", [])
self.handler = handler      # keep it on the object that outlives the registration
register_callback(handler)  # C stores the pointer
```

The same caution applies to a lambda that captures local state: the captured variables live as long
as the `Callable` does, so the reference you keep is what keeps them valid.
