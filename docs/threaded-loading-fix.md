---
title: Threaded loading fix
---

# Threaded loading fix

This build carries one engine fix on top of Godot 4.7.2. It is small, and it is
the reason games stop crashing while a level loads.

## The crash

`ResourceLoader::_run_load_task()` ended by releasing the load task's token:

```cpp
load_task.load_token->unreference();
```

That pointer can be **null** by then. The task is reached through a reference
into `thread_load_tasks`, a `HashMap` that other threads rehash whenever a nested
load inserts an entry, so the field can read back as garbage when the load
finishes. The result is a worker-thread crash:

```
EXC_BAD_ACCESS (code=1, address=0x158)
  RefCounted::unreference(this=0x0)
  ResourceLoader::_run_load_task()
  WorkerThreadPool::_process_task
```

It only happens with `ResourceLoader.load_threaded_request(path, "", true)` — the
sub-threaded variant — and only on scenes large enough to trigger nested loads.
A trivial project never reproduces it.

## The fix

Both release sites are null-guarded:

```cpp
if (load_task.load_token) {
    load_task.load_token->unreference();
}
```

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
rehash, and one of them still reads a field through that reference after the
lock is released. A complete fix needs stable storage for those entries —
pointers with ownership, or copying the fields that must outlive the lock.

Until then, the workaround is available if a future engine version regresses:

```gdscript
# Sub-threads off: one loader thread instead of the pool spawning per resource.
ResourceLoader.load_threaded_request(path, "", false)
```

The original change is commit `3f773be` in the development history; the fresh
repository's history starts from a snapshot, so the diff above is the record of
it.
