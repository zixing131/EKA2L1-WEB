# Installing a second device hangs until the app is backgrounded

## Symptom

On a physical iPhone, installing another ROM after a device was already booted
could leave the import sheet at **Processing...** indefinitely. Sending EKA2L1
to the background and returning to it immediately allowed the installation to
finish. Installing the first device did not have the same failure mode.

The install itself runs off the main queue, so the spinner was not a blocked
SwiftUI animation. Its completion never reached the main queue because the
worker had not returned from the bridge.

## Narrowing the hang

Device installation must exclude the running guest loop while it mutates the
device manager and drive tree. It first clears `mounted`, calls
`stop_cores_idling()`, then waits for `loop_mutex`. The OS thread holds that
mutex around `symsys->loop()`.

With CPU load saving enabled, an idle guest can sleep inside
`thread_scheduler::reschedule()` while the outer iOS loop still holds
`loop_mutex`. Waking the scheduler before taking the mutex is therefore
necessary. The lifecycle clue was decisive: backgrounding calls the same
`stop_cores_idling()` operation again. Audio, sensors, and SwiftUI refreshes
cannot release `loop_mutex`; the second scheduler wake can.

The first wake could be lost because `common::event` had two competing reset
contracts. On POSIX, `event_impl::wait()` consumed the signal by clearing
`is_set_` before it returned. The scheduler then called `idle_event.reset()`
again. A new `set()` arriving between those operations was erased by the second
reset. The OS thread could enter idle wait again while retaining `loop_mutex`,
leaving the installer blocked until a lifecycle pause supplied another wake.

Windows implemented the same class with a manual-reset event, so callers had
grown dependent on an explicit reset even though the POSIX implementation was
already auto-reset. That platform mismatch made fixing only the scheduler
unsafe.

## Fix

`common::event` now consistently has auto-reset semantics: one `set()` releases
one waiter, and that waiter consumes the signal atomically. Windows uses an
auto-reset event, and redundant post-wait resets were removed from both the
kernel scheduler and the Qt pause loop.

A common-layer regression test verifies that both indefinite and timed waits
consume exactly one signal. The iOS install protocol itself is unchanged: it
still pauses and drains the guest loop before touching emulator state, but its
wake can no longer be erased in the handoff.
