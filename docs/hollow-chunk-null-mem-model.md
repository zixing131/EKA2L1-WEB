# A failed chunk allocation still handed the guest a usable handle

## Symptom

TestFlight build 26.7.0 (260847) on an iPhone 17 Pro (`iPhone18,4`, iOS 26.5.2) crashed with
`EXC_BAD_ACCESS (SIGSEGV)` at address `0x0`, about six minutes into a session. The faulting
thread was the emulation thread, inside an SVC:

```
0  eka2l1::kernel::chunk::max_size() const + 4
1  eka2l1::epoc::chunk_max_size(kernel_system*, unsigned int) + 60
2  hle::bridge<kernel_system, int, unsigned int> lambda
3  eka2l1::hle::lib_manager::call_svc(unsigned int) + 856
4  ARMul_State::RaiseSystemCall(unsigned int) + 52
5  InterpreterMainLoop(ARMul_State*, unsigned int&) + 10528
```

The other threads were idle or blocked in normal waits; nothing pointed at a teardown race.

## Narrowing it down

The obvious reading — "`kern->get<kernel::chunk>(h)` returned null and the SVC dereferenced it" —
is wrong: `chunk_max_size` has had an explicit `if (!chunk) return epoc::error_bad_handle;`
since 2019. `git log -L` on that function confirms the check was in the shipped source.

The dSYM from the CI run for `9271a2319` (`dwarfdump --uuid` matching
`3609a56ad50a3fe48e43e6c1e2b57a86` exactly) confirmed the frames were symbolicated correctly,
so the crash really was two instructions into `chunk::max_size()`.

That function is a one-liner:

```cpp
const std::size_t chunk::max_size() const {
    return mmc_impl_->max();
}
```

The register state fits an `ldr x0, [x0, #off]` (load `mmc_impl_`) followed by a faulting vtable
load: `x0` was `0` at the fault, and `x1`/`x19` still held the guest handle `0x0036002f`. The
crash report's "byte read" annotation is a red herring — the ESR was `0x92000006`, i.e. `ISV=0`,
so the access-size field is not valid and Apple's decoder falls back to "byte".

So `this` was a live chunk object and **`mmc_impl_` was null**.

## Root cause

`kernel::chunk`'s constructor treats a memory-model failure as a warning, not an error:

```cpp
if (err != mem::MEM_MODEL_CHUNK_ERR_OK) {
    LOG_ERROR(KERNEL, "Failed to allocate mem model chunk: {}, error: {}", obj_name, err);
} else {
    ...
}
```

`flexible_mem_model_process::create_chunk()` leaves its `mem_model_chunk *&` out-parameter
untouched on every failure path, and `multiple_mem_model_process::create_chunk()` returns
`MEM_MODEL_CHUNK_ERR_MAXIMUM_CHUNK_OVERFLOW` without setting it once a process reaches
`MAX_CHUNK_ALLOW_PER_PROCESS` (1024). Either way `mmc_impl_` stays `nullptr` and the constructor
returns a fully registered kernel object anyway.

`chunk_new` (SVC 0x00) then only checked the *handle*:

```cpp
const kernel::handle h = kern->create_and_add<kernel::chunk>(...).first;
if (h == kernel::INVALID_HANDLE) {
    return epoc::error_no_memory;
}
return h;
```

The handle is always valid — it is allocated from the handle table, which knows nothing about the
memory model. So a guest that runs the emulated process out of address space or chunk slots gets
a positive handle back from `RChunk::CreateLocal()`, concludes the chunk exists, and the very next
call (`MaxSize()`, `Base()`, `Adjust()`, ...) dereferences a null `mmc_impl_` and takes the host
process down. Every other `chunk::` accessor had the same shape; `max_size()` just happens to be
what this guest called first.

`multiple_mem_model_process::create_chunk()` had a second, related defect: when the slot was
allocated but `do_create()` failed, it still published the half-initialised struct through the out
parameter *and* kept the slot. The caller's code-chunk retry path
(`force_clean()` then create again) therefore leaked one slot per failure, pushing the process
closer to the 1024 limit that causes the failure in the first place.

Two smaller latent bugs in the same file: `chunk::destroy()` dereferenced `own` one line before
null-checking it, and the deserialisation constructor `chunk(kernel_system *, memory_system *)`
left `mmc_impl_` uninitialised rather than null.

## Fix

- `multiple_mem_model_process::create_chunk()` releases the chunk slot and leaves the out pointer
  untouched when `do_create()` fails, matching the flexible model's contract.
- `kernel::chunk` gained `valid()`, and every `mmc_impl_` dereference is now guarded so a hollow
  chunk degrades (0 / `false` / `KErrNoMemory`) instead of segfaulting. `destroy()` clears
  `mmc_impl_` and checks `own` before using it; `mmc_impl_` is default-initialised to `nullptr`.
- `chunk_new` and `chunk_create_eka1` close the handle and return `KErrNoMemory` when the created
  chunk is not `valid()`, so the guest sees the failure Symbian would have reported.

The guard alone would have converted the crash into silent wrong behaviour; reporting
`KErrNoMemory` from the creation SVCs is what actually matches the platform, and the guards exist
so the kernel-internal creation sites (FBS shared/large chunks, window server chunks, skin chunk)
that also ignore construction failure cannot turn a resource shortage into a host crash.
