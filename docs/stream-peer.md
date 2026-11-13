---
title: StreamPeerCFFIPointer
---

# StreamPeerCFFIPointer

A `StreamPeer` over a region of C memory, so Godot's own binary readers and writers can work directly
on a C buffer.

```gdscript
var peer := StreamPeerCFFIPointer.new()
peer.set_pointer(some_c_pointer)
peer.set_size_in_bytes(1024)

peer.seek(0)
var value := peer.get_32()      # inherited from StreamPeer
```

## Methods

```
CFFIPointer get_pointer()
void        set_pointer(CFFIPointer pointer)
int64_t     get_size_in_bytes()
void        set_size_in_bytes(int64_t size_in_bytes)
int64_t     get_size()
void        set_size(int64_t size)
void        seek(int64_t position)
int64_t     get_position()
void        clear()
```

`get_size`/`set_size` report the size in elements of the pointer's type; `get_size_in_bytes`/
`set_size_in_bytes` report it in bytes. `clear` empties the region so it reads as zeros.

Because it inherits `StreamPeer`, everything defined there works: `get_8`, `get_u16`, `get_float`,
`get_data`, `put_*`, and so on.

```gdscript
peer.seek(0)
for i in 4:
    print(peer.get_float())
```

The pointer does not own the memory unless it is a `CFFIOwnedArray` or `CFFIOwnedValue`; a stream
over a borrowed buffer must not outlive the buffer.
