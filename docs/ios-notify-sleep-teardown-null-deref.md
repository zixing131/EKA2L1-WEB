# `User::After` completion null-deref on thread teardown

## Symptom

TestFlight build 26.7.0 (260760) crashed with `EXC_BAD_ACCESS (SIGSEGV)`, a
write translation fault at address `0x0`. The crashing thread was the emulator's
timing thread:

```
Thread 9 Crashed:
0  EKA2L1  eka2l1::kernel::thread::notify_sleep(int) + 108
1  EKA2L1  eka2l1::ntimer::advance() + 336
2  EKA2L1  eka2l1::ntimer::loop() + 160
```

Register state: `x0 = 0` (the store target), `x1 = 0x0040fd94` — a low guest
address that looks like a `TRequestStatus*` sitting in process/thread memory.
The ESR reported a byte-write translation fault, but `ISV=0`, so the access-size
bits are meaningless; the meaningful fact is a **write to a null pointer**.

## Narrowing it down

`notify_sleep` is the completion path for `User::After`. `svc.cpp`'s `after`
bridge calls `thread::sleep_nof(status, micro_secs)`, which stores the guest
`TRequestStatus*` in `sleep_nof_sts` and schedules the shared
`SchedulerWakeUpThread` timer event. When the event fires, `ntimer::advance()`
runs the callback, which looks the thread up by uid and calls
`notify_sleep(0)`. The offending line was:

```cpp
(sleep_nof_sts.get(owning_process()))->set(errcode, kern->is_eka1());
```

`ptr::get()` translates the guest address through the owning process's address
space (`get_ptr_on_addr_space`). It returns `nullptr` when the page is no longer
mapped. The unconditional `->set()` then wrote through null → SIGSEGV. The
faulting guest address in `x1` matched `sleep_nof_sts`, confirming the
translation returned null.

Why was the page unmapped? `ntimer::advance()` runs each event callback **outside
the timing lock** (it `unq.unlock()`s around the call). `scheduler::stop()`
cancels a thread's queued wakeup via `unschedule_event`, but it cannot cancel an
event that has already been popped and is mid-callback. So there is a window:

1. `advance()` pops the wakeup event for thread T and releases the lock.
2. T (or its process) is torn down — T's stack / data chunk is unmapped
   (`thread::destroy()` → `kern->destroy(stack_chunk)`), while the kernel thread
   object itself is still reachable via `get_by_id`.
3. The callback runs `notify_sleep`; `sleep_nof_sts` is still set, but its page
   is gone, so `get()` returns null and the write faults.

The sibling completion path already handles exactly this: `notify_info::complete()`
null-checks `sts.get(...)` before calling `set()`. `notify_sleep()` was simply
missing the same guard — this is a general kernel race, not title-specific.

## Fix

Guard the translated host pointer in `notify_sleep`, mirroring
`notify_info::complete()`:

```cpp
epoc::request_status *sts_real = sleep_nof_sts.get(owning_process());
if (sts_real) {
    sts_real->set(errcode, kern->is_eka1());
}
```

If the sleeping thread has already been torn down there is nothing meaningful to
complete, so skipping the write (while still clearing `sleep_nof_sts` and
signalling the request) is safe. Verified with the standard iOS regression suite
(11/11 pass, including the 90s Final Battle in-game dwell that exercises timer
completions).
