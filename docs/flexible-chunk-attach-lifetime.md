# Attaching to a flexible chunk was mistaken for owning it

## Symptom

TestFlight build 26.7.0 (260861) on an iPhone 17 Pro (`iPhone18,4`, iOS 26.5.2) produced two
`EXC_BAD_ACCESS (SIGSEGV) at 0x20` reports a minute apart, both inside
`mem::flexible::memory_object::detach_mapping()` with `this == nullptr`:

```
# report 1 -- emulation thread, guest closing a handle
0  memory_object::detach_mapping(mapping*) + 32
1  flexible_mem_model_process::detach_chunk(mem_model_chunk*) + 108
2  flexible_mem_model_process::delete_chunk(mem_model_chunk*) + 32
3  kernel::chunk::destroy() + 108
4  kernel_system::destroy(kernel::kernel_obj*)
5  kernel::kernel_obj::decrease_access_count()
6  kernel::object_ix::close(unsigned int)
7  kernel_system::close(unsigned int)          <- SVC

# report 2 -- device switch, on the dispatch queue
0  memory_object::detach_mapping(mapping*) + 32
1  flexible_mem_model_process::~flexible_mem_model_process() + 60
2  kernel::process::kill(...)
3  kernel::process::destroy()
4  kernel_system::wipeout()
5  system_impl::~system_impl()
6  -[EKA2L1Emulator bootDeviceAtIndex:]
```

The flexible model only runs on `epocver >= epoc95`, so this is a Symbian^3/Belle device
(X7, rm-707).

## Narrowing it down

`x0` was `0` at the fault and the faulting address was `0x20`, which is exactly
`memory_object::mappings_` (after `data_`, `page_occupied_`, `control_`, `external_`). So the
callee was reached with a null `this`: `fl_chunk->mem_obj_` read as null.

A live chunk cannot have a null `mem_obj_`. `do_create()` unconditionally
`make_unique<memory_object>()`s it, and it runs before the chunk is ever handed out — since
`51bbb0f`, `create_chunk()` destroys the struct and leaves the out pointer untouched when
`do_create()` fails. So the pointer being dereferenced was not a live chunk, it was a freed one
whose memory had been reused and zeroed.

That freed struct was still reachable, in both reports, from a `flexible_mem_model_process`
attach list: report 1 found it with the `std::find_if` in `detach_chunk()`, report 2 walked it
straight out of `attachs_`. A plain double-`delete_chunk` would not do this — the second call's
`find_if` fails and returns before touching `mem_obj_`. Something else had freed a chunk struct
that a process still had attached.

## Root cause

`flexible_mem_model_chunk` structs live in one global `chunk_manager` on `control_flexible`, but
their lifetime was driven from two places that both assumed "attached" means "owned":

```cpp
flexible_mem_model_process::~flexible_mem_model_process() {
    for (auto &attach: attachs_) {
        attach.chunk_->mem_obj_->detach_mapping(attach.map_.get());
        fl_control->chunk_mngr_->destroy(attach.chunk_);   // every attached chunk, owned or not
    }
}
```

Attaching is not ownership. `kernel_system::open_handle*()` and `mirror()` call
`kernel_obj::open_to(process)`, and `kernel::chunk::open_to()` maps the chunk into *that*
process — so any process that opens a handle to a global chunk (`FbsSharedChunk`,
`FbsLargeChunk`, `WsGlobalMemChunk`, the skin chunk, ...) ends up with an `attachs_` entry
pointing at a struct created by, and owned by, someone else. Both reports follow from that:

- **Report 1.** Process B opens a chunk created by process A, then exits. B's destructor frees
  A's chunk struct. The kernel object is still alive (A holds a handle), so when A finally closes
  it, `chunk::destroy() -> A->delete_chunk()` finds the entry in A's `attachs_` and dereferences
  the freed struct.
- **Report 2.** Two processes are attached to the same global chunk at wipeout. The first one
  destroyed during `kernel_system::wipeout()` frees the struct; the second walks its own
  `attachs_` entry into freed memory.

The mirror image of the same hole existed independently of the destructor: whenever a chunk
struct dies — `delete_chunk()` from the owner, or `mmc_impl_unq_.reset()` for the kernel-created
standalone chunks that have no owning process at all — every *other* process that opened it kept
its attach info. Nothing ever told them.

## Fix

Make the chunk track its attachers and drive the invalidation from the chunk side:

- `flexible_mem_model_chunk::attachers_` records every process holding an attachment;
  `attach_chunk()`/`detach_chunk()` maintain it.
- `~flexible_mem_model_chunk()` force-detaches whoever is left, while its mapping list and memory
  object are still alive. This covers every way a struct can die, including the standalone
  `mmc_impl_unq_` chunks the kernel creates with no owning process.
- `~flexible_mem_model_process()` detaches its mapping and drops itself from `attachers_`, but
  frees the struct only when nobody else has it mapped. If the creator exits while others are
  still attached, ownership moves to a surviving attacher, so the last one out still frees it
  instead of leaking. Freeing it any earlier would also unmap a chunk out from under a live
  process, which is not what Symbian does — a global chunk outlives its creator as long as a
  handle exists.

`is_addr_shared_` was also left uninitialised by the constructor until `do_create()` ran; it is
now `false` from the start.

The multiple/moving model is not affected: its `attach_chunk()` has an unconditional early
return, so `attached_` is never populated and chunk structs only ever live in their owner's
`chunks_` array.
