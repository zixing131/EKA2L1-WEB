# Flexible mapping detach left stale CPU TLB entries

## Symptom

TestFlight build 260790 (`78ff7124e`) crashed on the Symbian OS thread with
`EXC_BAD_ACCESS / KERN_INVALID_ADDRESS` in `InterpreterMainLoop`. The dSYM UUID
matched the report exactly. The fault was the inlined dyncom
`ARMul_State::ReadMemory8` TLB-hit path:

- guest address: `0x23A600F0`
- cached host page: `0x122200000`
- byte offset: `0xF0`
- fault address: `0x1222000F0`

The host pointer was no longer mapped. This looked identical to the earlier
cross-process decommit crash, even though build 260790 already contained the
fix that invalidates every MMU from both memory models' `decommit()` paths.

## Why the previous fix did not cover teardown

The flexible model stores every virtual view of a `memory_object` in its
`mappings_` list. `memory_object::decommit()` walks that list to clear page
tables and invalidate the CPU TLB.

Chunk teardown removes those views earlier:

1. `flexible_mem_model_process::detach_chunk()` calls
   `memory_object::detach_mapping()`.
2. Shared/fixed chunks also call `detach_mapping()` from
   `~flexible_mem_model_chunk()` to avoid walking a destroyed mapping later.
3. `detach_mapping()` only erased the pointer from `mappings_`.
4. The mapping destructor eventually cleared page-table entries, but never
   invalidated the CPU TLB.
5. When `~memory_object()` called `decommit()`, its mapping list was empty, so
   there was no virtual address from which to invalidate the cached
   guest-to-host translation.
6. The backing allocation was unmapped while dyncom still held its host
   pointer; the next guest access dereferenced freed host memory.

This is a lifecycle hole, not another owner/current-ASID selection bug.
Invalidating unconditionally inside `decommit()` cannot help after the
address-bearing mapping has already been removed.

## Fix

`memory_object::detach_mapping()` now performs the teardown while it still has
all required information:

1. clear the mapping's page-table entries, preventing a TLB miss from
   repopulating the translation;
2. invalidate the mapping's full virtual range on every MMU;
3. only then erase it from the memory object's list.

The later `mapping` destructor repeats the page-table unmap, which is
idempotent. Ordinary flexible-model decommit now follows the same safe order:
clear guest page tables and CPU TLB entries before making the host backing
inaccessible. This also removes the small interval in which another CPU access
could hit a still-valid translation after host decommit.
