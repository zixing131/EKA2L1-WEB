# `--app` deadlocked before the guest ran a single instruction

## Symptom

Launching the desktop frontend straight into a game — `EKA2L1 --app 0xA0002C40`
— produced an empty white window roughly four times out of five. The window
switched to game mode and the app list was gone, but nothing ever rendered. The
log stopped dead after the window server's three startup chunks
(`ScreenBuffer0`, `WsGlobalMemChunk`, `WsCodeChunk`) at around 318 lines, where
a healthy run reaches ~570. Launching the same game by clicking it in the app
list always worked.

## Diagnosis

Guessing at this was unproductive. Two wrong theories burned time first:

1. That the guest process never spawned. It sometimes did, sometimes did not —
   which is exactly what a race looks like, but it sent the investigation into
   `applist_server::launch_app` instead of the thread handshakes.
2. That the UI thread calling `get_current_active_screen()` was racing the
   emulator thread, because `get_screens()` lazily triggers `do_base_init()`
   and creates kernel chunks from the wrong thread. Plausible, consistent with
   the log's last lines, and wrong.

What settled it was `sample <pid> 3 -f out.txt` against a hung process. macOS
`sample` prints every thread's stack, which is the right tool for a deadlock
where the interesting fact is what each thread is *waiting on*:

```
main-thread      → QApplication::exec()                          (healthy)
Graphics thread  → ogl_graphics_driver::run() → request_queue::pop()
                                                                 (healthy, idle)
Symbian OS thread→ os_thread() thread.cpp:285 → common::event::wait()
```

`thread.cpp:285` is the *startup* `graphics_event.wait()`. The OS thread had
never left it, so `symsys->loop()` had never run and not one guest instruction
had executed. Everything in the log up to that point came from `stage_two()`
and from main-thread work.

The handshake:

```
graphics thread:  ... init ... → graphics_event.set()
                  → joystick_controller->start_polling()
                  → graphics_event.reset()          ← the bug
                  → graphics_driver->run()

OS thread:        stage_two() → init_done_event.set()
                  → graphics_event.wait()           ← parked here forever
```

`common::event` is auto-reset: `wait()` clears `is_set_` itself. `set()` takes
the lock, sets the flag and calls `notify_one()`; the waiter must then
re-acquire that same lock before it can re-evaluate its predicate. If the
graphics thread reaches `reset()` inside that window, the flag is cleared, the
waiter's predicate is false again, and the wake is gone. `start_polling()`
spawns an SDL thread, which is easily long enough for the waiter not to be
scheduled yet.

The deadlock is then mutual, because `graphics_event` carries *both*
directions: at shutdown `graphics_driver_thread_deinitialization` waits on the
same event for the OS thread to signal it. So a quit attempt also hung forever
in `os_thread_obj.join()` — one earlier sample caught exactly that state, with
the main thread already past `exec()`.

`git log -S` pinned the history. The `reset()` came from upstream `26c3d4bde`
(pent0, 2022), where it was correct — Win32 `CreateEvent(NULL, TRUE, ...)` is
manual-reset, so the signal latched and needed an explicit clear. But the POSIX
implementation has consumed on wait since `7ca4a5956` (2020), so on macOS and
Linux the race has been live upstream the whole time. `3d88ef0fa` later changed
Win32 to auto-reset to match, which fixed a real lost-wakeup elsewhere and, as
a side effect, exposed Windows to this one as well. `e14aa59e0` in this fork
removed the post-wait resets that change made redundant — but only the ones it
happened to touch. This site is a pre-wait reset on the *other* side of the
handshake, so it was missed.

Why the app-list path never hit it is timing, not logic: `--app` makes the main
thread load and spawn the guest before it starts the graphics thread, which
shifts when the two threads meet.

## Fix

Delete `state.graphics_event.reset()`. Under auto-reset semantics the waiter
already consumes the signal, so it is redundant in the good case and destructive
in the racy one. The two remaining post-wait resets on `init_event` and
`init_done_event` go too — same pattern, same failure mode — finishing what
`e14aa59e0` started. The explicit *pre*-wait `init_done_event.reset()` in
`mainwindow.cpp` stays: that one deliberately discards a stale signal before
raising a new request.

## Verification

`--app` went from 1 success in 5 to 20 in 20 across three batches. The single
failure inside those batches was a window the user closed by hand, not a hang.

Beyond the harness: a matrix over the CPU backends (`dyncom`, `dynarmic`,
`unicorn`, `r12l1`, and an unrecognised value) all boot and launch a game, and
Jelly Chase, Snakes and 7Days render identically on both real backends.

## Related

Reading the config's CPU backend again (`epoc.cpp` had it hardcoded to
dynarmic) turned out to need a guard of its own: `unicorn` and `r12l1` are
accepted by `string_to_arm_emulator_type` but have no core on a 64-bit desktop
host, and `create_core` answers null for them, which killed the emulator during
startup with nothing in the log at all. `resolve_emulator_type` now downgrades
them to dyncom with a reason, and `create_exclusive_monitor` resolves too so a
downgraded core cannot be paired with a monitor built for the backend that was
requested — that mismatch was reachable on iOS as well.

Upstreamed together as EKA2L1/EKA2L1#642.
