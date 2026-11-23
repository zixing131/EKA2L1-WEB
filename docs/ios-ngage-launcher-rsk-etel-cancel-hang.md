# N-Gage Launcher freezes while exiting: incomplete ETel notification cancels

## Symptom

On the 5320 (RM-409), pressing the right soft key on the N-Gage Launcher home
screen starts normal shutdown work but never closes the application. The last
frame remains visible at 0 FPS. Using the native **Exit Game** action after that
could expose a separate host-side window teardown deadlock; that deadlock did
not explain why the guest's own Exit command froze first.

## Narrowing down

The full SVC trace showed that the right soft key was delivered. The Launcher
closed its helper servers and sessions, then its main thread stopped after:

```
session_send_sync(function=0x5075, status=0x404ae8)
wait_for_any_request
wait_for_any_request
```

The target status remained `KRequestPending` and the thread slept on its request
semaphore. This was a guest wait, not a host mutex deadlock.

An active-scheduler dump initially made the state look like a lost request
signal: an unrelated active object was ready while the thread slept. That is
legal inside `User::WaitForRequest(TRequestStatus&)`. The wrapper may consume
signals belonging to other requests until its target completes, then return
those borrowed signals. The real invariant violation was simpler: the target
sync IPC was never completed.

The Nokia SDK's `etelmmcs.h` identifies decimal 20597 (`0x5075`) as
`EMobilePhoneNotifySignalStrengthChangeCancel`. EKA2L1 already implemented
`NotifySignalStrengthChange` and stored its request status, but did not dispatch
the matching cancel. The default phone opcode branch only logged an error; it
did not complete either the original notification or the synchronous cancel
IPC, so `WaitForRequest` could never return.

Adding that cancel exposed the next teardown request, 26501:
`EMobilePhoneNotifyCurrentNetworkChangeCancel`. Adding it exposed 20592:
`EMobilePhoneNotifyNetworkRegistrationStatusChangeCancel`. All three follow
ETel's documented rule that a mobile cancel opcode is the original async opcode
plus `EMobileCancelOffset` (500). The Launcher maintains all three notifications
and cancels them in sequence during shutdown, so each missing handler masked the
next.

## Fix

The phone subsession now dispatches all three notification cancel opcodes. Each
handler follows the existing battery and indicator notification contract:

1. complete the stored asynchronous notification with `KErrCancel`;
2. complete the synchronous cancel IPC with `KErrNone`.

This is shared ETel behavior, not an N-Gage-specific bypass. Unknown phone
opcodes still remain visible instead of being globally treated as successful.

## Dead ends worth avoiding

- `thread::is_suspended()` also reports ordinary wait states, so it is not
  evidence that the guest explicitly called `RThread::Suspend()`.
- A ready active object beside a `WaitForRequest` loop does not prove a lost
  signal; the loop is allowed to borrow unrelated request signals.
- The TestFlight watchdog stack in window focus teardown was a second bug
  reached only after forcing the already-frozen guest closed.

## Verification

- Debug and Release simulator builds, 5320/RM-409: launch N-Gage Launcher by UID
  `0x20003B78`, wait for the home screen, press RSK, and observe the Launcher
  finish teardown and return to the native application list.
- No unimplemented ETel phone cancel opcode, guest panic, access violation, or
  emulation halt appears in the exit log.
- The Release iOS regression suite passes 12/12 both with a fresh install and
  on the immediately repeated run.
