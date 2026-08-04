# MMF `more_buffer` callback leaks the kernel lock on a throw

## Symptom

TestFlight build 260779 was killed by iOS with a `0x8BADF00D` **scene-update
watchdog** transgression (10 s wall-clock allowance) while the user was playing a
game — a hang ("卡死"), not a segfault. The `.ips` shows `EXC_CRASH / SIGKILL`
from `FRONTBOARD` with `ProcessVisibility: Background`, and the device was under
thermal pressure (`Thermal Level: 6, serious`).

The report is a deadlock, not a crash. Three threads are all blocked in
`_pthread_mutex_firstfit_lock_wait` → `std::mutex::lock()`, each on the **same**
`kernel_system::kern_lock_`:

- **main thread** — a keypad tap: `HoldableRawKey.press` → `submitRawKey:` →
  `window_server::queue_input_from_driver` → `handle_input_from_driver` →
  `window_key_shipper::start_shipping()` → `kern->lock()`
- **Symbian OS thread** — `system_impl::loop()` → `kernel_system::reschedule()`
  → `lock()`
- **Timing thread** — `ntimer::advance()` → `timer_callback` → `kern->lock()`

No *live* thread holds `kern_lock_`. A mutex with waiters but no owner means it was
locked and never unlocked — a **leaked lock**.

## How it was narrowed down

Symbolicated against the exact CI dSYM (UUID `3FD98A64-…`, run 29916896964). The
three EKA2L1 stacks above pinned the contended mutex to `kern_lock_`. Ruling out
the usual suspects took most of the effort:

- **Not a re-entrant self-deadlock.** `submitRawKey` does not pre-lock the kernel;
  `start_shipping` takes and releases `kern_lock_` in tight, balanced pairs.
- **Not an AB-BA with `screen_mutex`.** The crash is a *key* event, which never
  touches `screen_mutex`. Both redraw paths (`animation_scheduler::invoke_due_animation`
  on the timing thread and the iOS `kick_screen_redraw` on a dispatch queue) take
  `kern_lock_` → `screen_mutex` in the *same* order — no inversion.
- **Not an AB-BA with the ntimer `lock_` or the system `mut`.** `ntimer::advance`
  releases `lock_` before invoking the callback that takes `kern_lock_`; the OS
  thread holds `mut`/`loop_mutex` and *then* wants `kern_lock_`, and every bridge
  op that mutates kernel state acquires `loop_mutex` first, so those orderings are
  consistent.

That left a leak. The only threads that touch `kern_lock_` during gameplay are the
three deadlocked ones plus one more that the crash snapshot does not foreground:
CoreAudio's **realtime render thread**. Commit `22fc419d1` had just wrapped that
thread's guest data path in `try { … } catch (...) {}` to stop an exception from
unwinding into `std::terminate`. That fix is correct, but it turned a *crash* into a
*swallowed exception* — and the swallowed path leaks a lock.

## Root cause

`mmf_dev_server_session::init_stream_through_state` registers the DSP
`more_buffer` notification callback (`services/audio/mmf/dev.cpp`). It ran on the
audio render thread and unlocked the kernel **manually**:

```cpp
if (!kern->try_lock()) return false;
{
    const std::lock_guard<std::mutex> guard(dev_access_lock_);
    if (last_buffer_) complete_play(epoc::error_underflow);
    else              do_report_buffer_to_be_filled();   // completes guest IPC / writes descriptors / enqueues events
}
kern->unlock();     // ← skipped if the body throws
return true;
```

`complete_play` / `do_report_buffer_to_be_filled` complete guest requests, write
into guest descriptors, and enqueue window/message-queue events — all of which can
throw (guest-memory faults surfaced as C++ exceptions, `bad_alloc` under the memory
pressure that accompanies the observed thermal throttling). When the body throws,
the RAII `dev_access_lock_` guard releases but the manual `kern->unlock()` at the
bottom is skipped, so `kern_lock_` stays locked. The exception then propagates up
`data_callback` → `call_callback` and is caught by `output_render_cb`'s
`catch (...)`, so the render thread simply falls back to silence and keeps
running. The kernel lock is now held by nobody, and the OS/timing/input threads
block on it until the watchdog kills the app.

Before `22fc419d1` this same throw would have `std::terminate`d immediately; the
noexcept boundary converted that crash into this hang. The real defect — a
non-exception-safe manual `kern->unlock()` on a host realtime thread — predates it.

## Fix

Make the kernel unlock RAII in both the playback and record `more_buffer`
callbacks, so the lock is released on every exit path including an exception:

```cpp
std::unique_lock<kernel_system> kern_guard(*kern, std::try_to_lock);
if (!kern_guard.owns_lock()) return false;
{ const std::lock_guard<std::mutex> guard(dev_access_lock_); … }
return true;   // kern_guard releases on scope exit, even while unwinding
```

`kernel_system` already exposes `lock()/try_lock()/unlock()`, so it satisfies the
`Lockable` requirement for `std::unique_lock`. These two callbacks were the only
places that manually unlocked the kernel from inside a host realtime callback.

## Takeaway

Any lock a host realtime/audio/dispatch callback takes must be RAII-scoped: those
callbacks sit behind a `noexcept` boundary that swallows exceptions, so a manual
unlock skipped by an in-flight throw silently leaks the lock and wedges the whole
emulator instead of crashing loudly.
