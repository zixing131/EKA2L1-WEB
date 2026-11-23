# First ThreadSanitizer pass over the iOS build

Recent crash triage rounds kept landing on the same shape of bug: an object that was
still alive but touched from two threads at once (audio render vs. destructor, timer
callback vs. teardown, play-done notification vs. the kernel lock). Every one of those
was found *after* the fact, from a TestFlight `.ips` or a watchdog report, because the
races are timing-dependent and essentially never reproduce locally.

ThreadSanitizer inverts that: it reports a race whenever two threads access the same
address without synchronisation, whether or not the bad interleaving actually happened
on that run. This is the first pass of it over the iOS port.

## Running it

`EKA2L1_SANITIZER=address|thread` on `scripts/build_ios.sh` (or `-DEKA2L1_SANITIZER=`
directly). On the Xcode generator it sets `ENABLE_THREAD_SANITIZER` / 
`ENABLE_ADDRESS_SANITIZER` rather than hand-rolling `-fsanitize=` flags — the build is
mixed C++/Obj-C++/Swift, and swiftc spells the flag `-sanitize=`, so letting Xcode
instrument and link the runtime in one go is the only thing that gets all four right.

The JIT is forced off whenever a sanitizer is on: TSan cannot see into dynarmic's
emitted code and reports its code buffers as races. Runs go through dyncom, which is
fully instrumented. Reports land wherever `TSAN_OPTIONS=log_path=` points; the app
container's `Documents/` is convenient, and `halt_on_error=0` collects every race
instead of stopping at the first.

Expect roughly a 10x slowdown. Boot takes about a minute, and a static guest screen
reporting 0 FPS is normal rather than a hang.

## What it found

**206 races on boot, all one root cause.** `common/src/cvt.cpp` held five
process-global `std::wstring_convert` objects. That class carries mutable conversion
state — an `mbstate_t` plus a converted-character count — which every `to_bytes` /
`from_bytes` call writes to, so a shared instance is not safe to call from more than
one thread. Behind those five objects sit ~330 call sites: Symbian filename and
descriptor conversion runs through them from the UI thread, the CPU thread, timer
threads and every service thread. On boot the main thread's device setup and
`applist_server`'s registry rescan collide constantly.

Only non-Windows platforms were exposed; the Win32 branch of those helpers uses
`WideCharToMultiByte`, which is thread-safe. Making the converters `thread_local`
took the count to 1. A function-local instance per call would also be correct but
allocates a codecvt facet every time, on a path this hot.

**A timer thread reading its clock before it was started.** `ntimer::reset()` spawned
the timer thread and only then called `teletimer_->start()`. The loop calls
`advance()` → `microseconds()` immediately, so it could read a `start_` left over from
the previous run — a reset-time clock glitch that could make the timer catch up a
large fake interval. Starting the teletimer before the thread that reads it took boot
to zero races.

**A half-constructed `std::function` called through its vptr.** With a game running,
TSan flagged `data race on vptr (ctor/dtor vs virtual call)` on
`emu_window_ios::surface_change_hook`. The graphics thread published
`state->graphics_driver` inside `layer_mutex` and then assigned the hook *outside* the
lock, while `attachLayer:` on the main thread read the driver with no lock at all and
treated "driver is non-null" as "the hook is ready". Between those two steps the main
thread could see a `std::function` mid-construction and dispatch through its vptr —
the same failure shape as the Angry Birds crash that called a non-function pointer.

Fixing only the writer is not enough: without the reader taking the lock there is no
happens-before edge, so the hook's construction is not guaranteed visible. The hook is
now installed inside the same critical section and *before* the driver is published,
and `attachLayer:` samples the driver inside the critical section it already had.

**An audio flag that could silence a stream permanently.** `dsp_output_stream_shared`
guards its "already asked the guest for more data" state with a plain `bool` written
by both the guest thread (`write()`) and the host render thread (`data_callback()`):

```
render thread                             guest thread
─────────────────────────────────────────────────────────
if (!more_requested)  → decide to ask
  more_buffer_callback_(...)  ──────────→ woken, supplies data
                                          write() → more_requested = false
more_requested = true    ← overwrites the guest's clear
```

The data arrived, but the flag stayed `true`, so the next time the buffer ran low no
request was made and the stream starved — silence until something else called `write()`
or `stop()`. The window is not a few instructions; it spans the whole callback, which
is precisely the call that wakes the other thread.

The flag is now `std::atomic<bool>` and the slot is claimed with `exchange(true)`
*before* invoking the callback, so a clear that arrives during the call survives. The
original semantics are preserved: no callback still leaves it set, a callback returning
true leaves it set, and one returning false releases it for a retry.

## What is left

Four races remain during gameplay, all `dyncom_core::stop()` against
`InterpreterMainLoop`. That is the deliberate asynchronous-stop mechanism — another
thread pokes a field to make the interpreter leave its loop. It is a real race by the
memory model, but the field sits in the interpreter's hottest loop, so making it atomic
needs its own performance measurement rather than a drive-by fix.

`screen::flags_` also races between `restore_from_config` on the window-server thread
and `set_native_scale_factor` on the render thread, each holding a *different* lock.
The consequence looks mild (an occasionally wrong scale factor), so it is recorded
rather than fixed here.

## Performance

Sampling the process for 20s during real gameplay (1977 frames) shows zero hits in
`ucs2_to_utf8`, `utf8_to_ucs2`, `wstring_convert` or `tlv_get_addr` — the
`thread_local` access stub never appears. Those conversions live on load-time paths,
not per-frame ones. The audio `exchange` runs once per buffer-low callback, next to a
PCM memcpy. Final Battle, Calculator and N95 Calculator regressions pass 12/12.

## Worth remembering

TSan's value here was not that it found races nobody could have found by reading the
code — the `wstring_convert` globals are obvious in hindsight. It is that it found them
*without needing the bad interleaving to occur*, and it ranked them by exposure: one
root cause accounted for 206 of 206 boot reports.

Note also that none of the four fixes were "a pointer to a dead object". Every one was
"a live object touched from two threads". A handle/uid scheme would not have caught any
of them, which is a useful data point when weighing lifetime-tracking work against
synchronisation work.
