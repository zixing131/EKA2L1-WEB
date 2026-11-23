# Quitting a game left the Qt frontend on a black screen, and closing it hung the process

## Symptom

On the Linux desktop build, quitting a guest game from inside the game (Snakes →
Quit → Yes on a 5320/RM-409) did not bring the application list back. The window
kept the emulated screen — now black, since the app that owned it was gone — and
went on drawing it at full frame rate. Closing the window from there did not end
the process either; it stayed alive with no window until it was killed.

## Narrowing it down

The frame counter kept ticking after the game was gone, so neither the render
thread nor the emulator loop had stopped. That ruled out the freeze the frontend
would show if the graphics driver had wedged, and pointed at the frontend simply
never being told the app had exited.

The emulator log said so directly, in a line that is easy to read past because it
comes from Qt rather than from the emulator's own logger:

```
qt.core.qobject.connect: QObject::connect: Cannot queue arguments of type 'eka2l1::kernel::process*'
(Make sure 'eka2l1::kernel::process*' is registered using qRegisterMetaType().)
```

It appears exactly once per game exit, right after the window server tears the
app's view down.

`main_window` learns about a guest process exiting through a `kernel::process`
logon callback, which runs on the emulator thread inside `process::kill`. It
hands that off to the GUI thread as `emit app_exited(proc)` over a
`Qt::QueuedConnection`. Queueing a call means copying its arguments, and Qt can
only do that for types it has a metatype for. `eka2l1::kernel::process *` has
none, so Qt discarded the call instead of delivering it. `on_app_exited` — the
only thing that calls `on_restart_requested()`, which resets the system and
rebuilds the application list — therefore never ran.

Registering the pointer type would have been the small fix, and the wrong one.
The callback fires while the process is being killed and the object is freed
shortly after the callback returns, so by the time a queued slot ran on the GUI
thread the pointer would have been stale — `on_app_exited` reads the exit type,
reason and category straight off it. The bug had been hiding a use-after-free.

The second half of the report — the window that would not close — is unrelated
to the first and was visible in a stack dump of the stuck process:

```
TID …: os_thread → ~system → ~system_impl → ~scripts
        → ~directory_watcher_impl → std::thread::join()
TID …: read()            ← the watch thread, blocked on inotify
```

`directory_watcher_impl`'s destructor removed its watches, set `should_stop`,
and joined the watch thread. That thread spends its life blocked in `read()` on
the inotify fd and only looks at `should_stop` between reads, so the shutdown
depended entirely on something happening to wake the read.

Something usually does, which is why this only bites sometimes: `inotify_rm_watch`
queues an `IN_IGNORED` event, and that is what the destructor's first loop
delivers. But it delivers it *before* `should_stop = true`. Win the race and the
thread wakes, sees the flag, and leaves. Lose it — the thread drains `IN_IGNORED`
and re-enters `read()` before the store lands — and the join waits for a file
event that will never come. The main thread was in turn joining the OS thread,
so the whole process hung. Closing the window 5 times on the unfixed build hung
once; two earlier hangs were caught the same way.

The header already included `<sys/eventfd.h>`; the wake-up it was meant for had
never been written.

## Fix

Snapshot the exit details — type, reason, category — into `int`/`int`/`QString`
at callback time and emit those. Qt queues them without any registration, and
nothing dereferences the process after it is gone.

Give the watcher an eventfd, `poll()` both it and the inotify fd, and have the
destructor write to it before joining. Two smaller things in the same loop: a
failed `read()` used to leave `length` at `-1`, which the `std::size_t`
comparison in the parse loop promoted to `SIZE_MAX` and walked off the end of
the event buffer, and the destructor dereferenced `wait_thread_` without
checking that construction had got far enough to create it.

## Verified

Against the same Debian/xrdp box, quitting Snakes on a 5320 now lands back on the
application list, and the Qt warning is gone from the log. Closing the window
exited cleanly 21 times out of 22; the single hang had no watcher or join frame
in its stack and is most likely the unrelated X round-trip deadlock this host is
already known for.

## Still open

Before the fix, with the emulator left sitting on the dead app for about a
minute, the process segfaulted in `kernel::thread::end_timeout_early()`, reached
from
`semaphore::signal()` at `sema.cpp:53`. It did not come back once the
frontend started restarting promptly, but the defect is still there:
`thread::kill` stops a thread and
detaches it from the scheduler, but never removes it from the wait queue of the
object it was blocked on, so a semaphore that outlives the process can pop a
freed thread pointer out of `waits` and dereference it. `semaphore::timeouted`
already does exactly the removal that is missing, but a general fix has to cover
mutexes, condvars and the EKA1 variants as well, so it is deliberately not part
of this change.
