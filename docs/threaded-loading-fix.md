---
title: Threaded loading fix
---

# Threaded loading fix

This build carries one engine fix on top of Godot 4.7.2. It is small, and it is
the reason games stop crashing while a level loads.

Line numbers below refer to this tree.

## The crash

`ResourceLoader::_run_load_task()` — [`core/io/resource_loader.cpp:337`](https://github.com/2maac/fast-godot/blob/main/core/io/resource_loader.cpp#L337) —
ended by releasing the load task's token. In the tree where the crash was caught
that call was at line 675; after the fix it sits at 681.

```cpp
load_task.load_token->unreference();
```

That pointer can be **null** by then. The task is reached through a reference
into `thread_load_tasks`, a `HashMap` that other threads rehash whenever a nested
load inserts an entry, so the field can read back as garbage when the load
finishes. The result is a worker-thread crash:

```
EXC_BAD_ACCESS (code=1, address=0x158)
  RefCounted::unreference()            core/object/ref_counted.cpp:92
  ResourceLoader::_run_load_task()     core/io/resource_loader.cpp:675   (pre-fix tree)
  WorkerThreadPool::_process_task()    core/object/worker_thread_pool.cpp:142
  WorkerThreadPool::_thread_function() core/object/worker_thread_pool.cpp:220
```

It only happens with `ResourceLoader.load_threaded_request(path, "", true)` — the
sub-threaded variant — and only on scenes large enough to trigger nested loads.
A trivial project never reproduces it.

## The fix

Two sites, both null-guarded:

| file | line | path through the function |
|---|---|---|
| [`core/io/resource_loader.cpp`](https://github.com/2maac/fast-godot/blob/main/core/io/resource_loader.cpp#L511) | 511 | early return, task already completed by another thread |
| [`core/io/resource_loader.cpp`](https://github.com/2maac/fast-godot/blob/main/core/io/resource_loader.cpp#L681) | 681 | normal completion, end of the function |

```cpp
if (load_task.load_token) {
    load_task.load_token->unreference();
}
```

In the tree where the fix was first written these call sites were at lines 509
and 675; they shifted by two and six lines as the surrounding code changed.

## How it was verified

A reproducer that mimics a Play button (`load_threaded_request` of the level with
sub-threads enabled) was run against several engines:

| build | crashed |
|---|---|
| official Godot 4.7.2 | 7 / 8 runs |
| stock 4.8 beta | 8 / 8 runs |
| this fork, before the fix | 3 / 8 runs |
| this fork, after the fix | 0 / 32 runs |

Because the same crash reproduces on official 4.7.2, it is an upstream defect
rather than something introduced by Grit.

## What is still open

The guard stops the crash; it does not repair the lifetime problem underneath it.
Several paths keep a `ThreadLoadTask &` into a `HashMap` that other threads can
rehash — for example
[`core/io/resource_loader.cpp:694`](https://github.com/2maac/fast-godot/blob/main/core/io/resource_loader.cpp#L694)
still reads `load_task.local_path` through that reference after the lock is
released. A complete fix needs stable storage for those entries — pointers with
ownership, or copying the fields that must outlive the lock.

Until then, the workaround is available if a future engine version regresses:

```gdscript
# Sub-threads off: one loader thread instead of the pool spawning per resource.
ResourceLoader.load_threaded_request(path, "", false)
```

The original change is commit `3f773be` in the development history; the fresh
repository's history starts from a snapshot, so the diff above is the record of
it.
