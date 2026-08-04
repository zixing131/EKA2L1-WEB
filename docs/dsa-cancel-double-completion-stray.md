# The stray signal behind `E32USER-CBase 46` was a DSA cancel completed twice

## Symptom

Snakes (`0x2000730F`) on N95 (rm-320) panics `E32USER-CBase 46` — the guest's
`CActiveScheduler` woke from `WaitForAnyRequest` and found no ready active object. The
emulator survived only because `thread::wait_for_any_request()` carries a stray-signal
absorber (see [`stray-signal-accounting-followup.md`](./stray-signal-accounting-followup.md)):
disable it and the panic reproduces 4/4 within ~25 s of boot.

The same request-semaphore imbalance is what the absorber has been papering over since it
was introduced, for Snakes, Final Battle and others.

## Root cause

`RDirectScreenAccess::Cancel()` is a *client-completed* request. ws32's
`CDirectScreenAccess::DoCancel()` sends `EWsDirectOpCancel` to wserv and then completes its
own `iStatus` locally:

```
CDirectScreenAccess::StartL()            (Ws32.dll ord 234)
  CActive::Cancel()                      (EUser  ord 1088)
    RDirectScreenAccess::Cancel()        (Ws32.dll ord 236)
      User::RequestComplete(iStatus, KErrCancel)   (EUser ord 617)
```

`CActive::Cancel()` then consumes exactly that one signal with `User::WaitForRequest`.

EKA2L1's `dsa::do_cancel()` also completed the outstanding "must stop" notification with
`epoc::error_cancel`, so a client-initiated cancel produced **two** signals for **one**
wait. The surplus signal stays in the request semaphore indefinitely; it only becomes fatal
much later, when the stream of real completions dries up and the scheduler happens to claim
it with no ready active object.

That matches every measurement:

- signal and wait totals balance exactly over a whole boot (1308/1308) — the extra signal is
  a real, well-formed completion, not an over-signal from a broken HLE path;
- no completion ever targets an already-complete request status (a source-side check over a
  full boot: 0 hits), so it is not a double *completion of one request* in the usual sense —
  the two completions come from opposite sides of the client/server boundary;
- at the guest scheduler's first loop head the semaphore count is exactly one greater than
  the number of ready active objects, and it stays exactly one greater for the rest of the
  run.

## How it was narrowed down

The invariant that made this tractable: **at a direct `User::WaitForAnyRequest` with an
active scheduler installed, `request_sema->count()` must equal the number of ready active
objects.** Checking that at the scheduler loop head gives a continuous detector that does not
depend on the panic firing — the panic is timing-dependent and disappears as soon as probes
slow the emulator down.

With the detector in place:

1. Recording every `TRequestStatus` the guest completes itself (via the `Exec::ThreadRequestSignal`
   exec that `User::RequestComplete` ends in — the status write happens in user space, the
   exec only signals) and re-reading those addresses at the surplus point: four of five had
   become ready active objects, one had not (`status = -3`, flags `0`).
2. Walking the guest stack for that call and resolving addresses to export ordinals named it
   as the `CDirectScreenAccess` cancel path.
3. A/B: suppressing the server-side completion in `dsa::do_cancel()` made the surplus never
   appear and, with the absorber disabled, Snakes booted cleanly 3/3.

Resolving guest addresses to symbols was the step that broke the deadlock. ROM DLLs are XIP,
so the image's `TRomImageHeader` (at `rom_entry::address_lin`) gives `export_dir_address` /
`export_dir_count`; the nearest export at or below the address yields an ordinal, and the
SDK's `epoc32/release/armv5/lib/<dll>.dso` maps ordinals to mangled names (each export is a
`.dynsym` entry whose value points into the `ER_RO` section, one 4-byte slot per ordinal).
Ordinals are stable across Symbian releases, so a Belle SDK `.dso` resolves an S60 3.1 ROM.

## Fix

`dsa::do_cancel()` takes a `complete_request` flag:

- `dsa::cancel()` (client-initiated `EWsDirectOpCancel`) passes `false` — the client already
  completed the request, so the server only drops it (`dsa_must_stop_notify_.sts = 0`, so a
  later abort or destruction cannot complete it either);
- `dsa::abort()` (server-initiated: screen mode change, another DSA taking over, window gone)
  and the destructor pass `true` — there the client has a genuinely outstanding request.

`thread::wait_for_any_request()`'s stray-signal absorber goes away with it, along with the
`stray_absorbed_refund_` budget that existed only to undo the absorber's own wrong guesses.
No diagnostic counter is left behind either: whether a wait is a direct
`User::WaitForAnyRequest` or an iteration of the `User::WaitForRequest` wrapper is decided
from r0 at a shared exec stub, so any "stray" tally built on it fires on perfectly healthy
guests (N-Gage Tetris produced ~10 per session while playing fine) and would send the next
investigation down a false trail.

The mirror-image shortcut — a direct `WaitForAnyRequest` that finds an empty semaphore but a
ready active object returns without consuming, because the signal went missing — is kept.
With the absorber gone, `wait_for_any_request()` is straight-line code again: identify the
stub, take that one shortcut, wait.

## Why the 5320 ROM never needed any of this

`dsa_must_stop_notify_` is only ever armed on the **old, sync-thread DSA architecture**. The
`dsa` constructor creates the sync thread only when `client_version().build <= WS_NEWARCH_VER`
**and** `epoc_version <= epoc93fp1`, and `request_access()` only stores the "must stop"
request status when that sync thread exists. Measured on the two ROMs:

| ROM | `epocver` | ws build | sync thread | `dsa_must_stop_notify_` at cancel |
|---|---|---|---|---|
| N95 rm-320 | 8 (`epoc93fp1`) | 151 | yes | **armed** |
| 5320 rm-409 | 9 (`epoc93fp2`) | 151 | no | not armed |

On 5320 and newer the client learns about aborts through the `dsa_must_abort_queue_` message
queue instead, so `dsa::do_cancel()`'s completion was already a no-op there — no surplus
signal, hence no stray and no need for the absorber. The bug only ever bit ROMs at or below
S60 3rd FP1 (N95, 6680).

## Dead ends worth skipping next time

- **`Absorbed stray` is invisible on iOS.** It is `LOG_INFO(KERNEL, ...)`, and the
  `BUILD_FOR_USER` default filter contains `Kernel:Warn`. Any "the counter is 0" conclusion
  drawn from grepping the log without raising the level is meaningless.
- **Chasing an over-signalling HLE path.** The follow-up doc pointed at `hle-ipc-complete`.
  A source-side check (does the completed status still carry `pending` before `set()`?) shows
  zero violations across a boot, and the `session::detatch` re-completion path — which looks
  exactly like the culprit, since an HLE server neither sets `msg_status = completed` nor
  unlinks `in_progress_msgs_` — never fires either.
- **Counter-level A/B cannot say *which* signal is unpaired.** Skipping the 3rd, 4th or 5th
  `Exec::ThreadRequestSignal` of a boot all behave identically (surplus gone, no panic, 40 FPS),
  because the semaphore is a plain counter. Identity has to come from the completed status
  address, not from the count.
- **Tightening `identify_wait_request_stub`'s r0 heuristic.** `User::WaitForAnyRequest` and
  `User::WaitForRequest` reach the same fast-exec stub with the same return address, and
  inside the wrapper's loop r0 frequently holds `KRequestPending` (`0x80000001`), which maps
  into ROM and so reads as a valid `TRequestStatus*`. Requiring r0 to be writable user memory
  reclassifies those wrapper iterations as direct waits, the `pot <= 0 && has_ready_request`
  shortcut then returns without consuming, and the guest livelocks (3.1 M shortcut breaks,
  black screen at 0 FPS).
- **The first code-looking word on the stack is not the return address.** It led to blaming
  ws32's `EventReadyCancel`; adding a log to that handler showed it is never reached. Validate
  candidates by checking that the preceding instruction really is a `BL`/`BLX`.
- **Probes change the timing.** One extra `fmt::format` per signal is enough to turn a
  reproduce-every-time panic into "five runs, no repro". Keep diagnostic state POD and format
  only at dump time.

## Verification

Release simulator build.

- `scripts/ios_regression_test.sh` 12/12 on dyncom and on dynarmic (`ios-use-jit: true`);
  `angrybirds` suite 5/5.
- Snakes on N95 reaches 3D gameplay at 42 FPS (dyncom) / 40 FPS (dynarmic), and on 5320
  reaches the main menu — with the absorber counter at 0 in every case.
- With the absorber removed (the state this change ships), Snakes on N95 reaches 3D gameplay
  on both backends, 5320 Snakes reaches its menu, and N-Gage Tetris (the title that used to be
  frozen *by* the absorber) plays with the tetromino falling on its own at 27 FPS.
- DSA control: Sky Force Reloaded on X7 renders its language menu at 32 FPS. Sky Force and
  Snakes on the 6680 ROM are black both before and after this change — pre-existing, not a
  regression.
