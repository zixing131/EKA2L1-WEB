# Stale CPU TLB entry after cross-process chunk decommit (TestFlight SIGSEGV)

## Symptom

TestFlight build 260763 (commit `8c4e881fa`), iPhone Air, iOS 26.5.2: hard crash
(`EXC_BAD_ACCESS / KERN_INVALID_ADDRESS`) on the "Symbian OS thread" about a
minute into a foreground session. The faulting frame symbolicated to
`InterpreterMainLoop` (dyncom), and the faulting instruction was the TLB-hit
fast path of `ARMul_State::ReadMemory32`:

- `LDR W0, [X9, X8]` with `X9 = 0x127230000` (cached host page base) and
  `X8 = 0xF0` (guest page offset).
- The report's `vmRegionInfo` shows `0x1272300f0` is **not in any region** — the
  host allocation ended exactly at `0x127230000` and the next 16K was an
  unmapped gap. So the read went through a cached guest→host translation whose
  backing host memory had been `munmap`ed.
- Reconstructed guest address: `0x23A600F0`, which under the **old (EKA1)
  memory map** falls in the shared/global chunk section
  (`0x10000000–0x30000000`).

## Diagnosis

The dyncom core keeps a small direct-mapped TLB (`cpu/12l1r/tlb.h`) that caches
guest page → host pointer translations. It is invalidated two ways:

- full flush on **process switch** (`thread_scheduler::switch_to`), and
- per-page `dirty_tlb_page` via `mmu_base::unmap_from_cpu` when a chunk
  decommits pages.

The hole: `multiple_mem_model_chunk::decommit()` only called
`unmap_from_cpu` when the chunk owner's address space equalled the MMU's
*current* address space:

```cpp
if (!own_process_ || mul_process->addr_space_id_ == mm->current_addr_space()) {
    mm->unmap_from_cpu(...);
    break;
}
```

That gate is fine for **local** chunks (their vaddr range is per-process, and
the TLB is flushed on every process switch, so no foreign entries can exist).
It is wrong for **globally-visible** pages — shared/global chunks (the entire
user area on the EKA1 map, where servers routinely dereference client heap
pointers directly), and RAM code ranges. Those are mapped at the same virtual
address in every address space, so the *current* process can hold TLB entries
for a chunk owned by *another* process.

Failure sequence:

1. Process A creates a global chunk; process B (current) reads it — the MMU
   read handler seeds the CPU TLB with the host pointer.
2. The chunk is destroyed while B is still current (B closes the last handle,
   or A's kernel objects are released from B's context). The owner-vs-current
   check fails, so the TLB is **not** dirtied.
3. `~multiple_mem_model_chunk` then `munmap`s the host backing.
4. B keeps running (no process switch, hence no full flush) and touches the
   vaddr again → the interpreter's TLB-hit fast path dereferences the freed
   host pointer → host SIGSEGV instead of a contained guest fault.

The flexible model had the same gate in `memory_object::decommit()`. There the
per-process mappings are handled correctly (each mapping's owner is compared),
but the **kernel fixed mapping** used for code/ROM/kernel regions — visible
from every address space — never matches an application's address space, so
decommitted code pages (e.g. DLL unload) could likewise leave stale TLB
entries. This became more reachable after `read_code` started seeding the TLB
on instruction fetch.

Dead end worth noting: `common::decommit` on iOS only `mprotect(PROT_NONE)`s
(region stays mapped), which would fault as `KERN_PROTECTION_FAILURE`. The
report's `KERN_INVALID_ADDRESS` + "not in any region" is what pinned the freed
memory to chunk *destruction* (`unmap_memory`), not a plain decommit.

## Fix

Dirtying a TLB entry is a pure invalidation — evicting a valid or foreign
entry is always safe (the next access re-resolves through the slow path), and
`dirty_tlb_page` is a trivial indexed tag compare. So `decommit` now calls
`unmap_from_cpu` unconditionally for all MMUs, in both the multiple model
(`mem/src/model/multiple/chunk.cpp`) and the flexible model
(`mem/src/model/flexible/memobj.cpp`, for every mapping of the memory object).
After this, a guest touching a vanished page falls into the slow path and gets
the normal unmapped-memory handling instead of crashing the host.
