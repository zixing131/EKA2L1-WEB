# N-Gage Tetris freezes: the stray-signal filter was eating real completions

## Symptom

5320 (rm-409) → N-Gage launcher (`ngiinstaller.exe`, UID `0x20007B38`) → *Start Game* → Tetris
(`tetris.exe`, UID `0x2000AFDB`).

The game boots, but nothing ever moves on its own:

- the N-Gage splash and the in-game board render exactly **one frame per key press**;
- the FPS counter stays at **0**;
- in gameplay the tetromino never falls — pressing a key advances the world by one frame and it
  stops again.

The launcher itself, and every other title on the same ROM, behaved normally.

## Narrowing down

`log-svc: true` + `extensive-logging: true` in `Documents/data/config.yml` gives a full SVC trace
without a rebuild. Sampling the log while the game looked frozen showed the emulator was *not*
idle: one Tetris thread was spinning at ~200 Hz through

```
mutex_wait / mutex_signal ×2 → timer_after(5000us) → wait_for_any_request
```

so the process was alive and its 5 ms tick was firing. The game's `Main` thread, though, ended its
trace at a `wait_for_any_request` and never woke again.

Adding the process/thread name and `r0/r1/r2/lr` to the SVC trace line
(`lib_manager::call_svc`) made the transition obvious — right where `Main` went quiet:

```
[Tetris[2000afdb]0001::Main] Calling SVC 0x800000 wait_for_any_request r0=0x0 lr=0x801a6383
Absorbed stray request signal #1 on thread Main (pc=0x8019DB4C)
```

That log line comes from `thread::wait_for_any_request()`'s stray-signal filter
(see [`stray-signal-accounting-followup.md`](./stray-signal-accounting-followup.md)): when the
guest wakes at a *direct* `User::WaitForAnyRequest` and the active scheduler has no ready active
object, EKA2L1 swallows the signal instead of letting euser panic `E32USER-CBase 46`.

### Proving the filter was the cause

The quickest decisive experiment was to make the absorb branch `break` instead of swallowing
(behind an env var) and re-run the same flow. Tetris went straight to a playable board at 7 FPS
with blocks falling — 13 signals that *would* have been absorbed were delivered to the guest and
its euser did not panic. So the filter, not the guest, was the problem here.

### What was actually being eaten

Instrumenting `thread::signal_request()` with a per-thread ring of `backtrace()` frames plus the
completed request-status address, dumped at the absorb point, named the victims:

- `window_server_client::get_ready()` → `base_fifo::set_listener()` → immediate `complete(0)`
  (the ws event queue was non-empty, so the `EventReady` request completes inside the IPC);
- `kernel::timer::fire()` — the game's own 5 ms tick;
- `fs_server_client::file_read()` completions from synchronous `SendReceive`.

All three are legitimate completions, not over-signalling. A full signal/wait ledger for the
Tetris `Main` thread (15672 signals) balanced perfectly against completions — `message_complete`,
`request_signal`, `thread_request_signal` and the notify/IPC paths accounted for every one.

### Why the scheduler saw a signal with no ready AO

Disassembling this ROM's `User::WaitForRequest(TRequestStatus&)` (euser at `0x8019AE68`; the
wrapper is Thumb at `0x801A6384`) shows the classic Symbian shape:

```
n = -1
loop: n++
      User::WaitForAnyRequest()      ; SVC stub at 0x8019DB48
      if (*aStatus == KRequestPending) goto loop
      if (n != 0) RThread::RequestSignal(n)   ; SVC 0x3B — hand back what wasn't mine
```

The request semaphore is a plain counter: `WaitForRequest` consumes one signal per turn and
*re-signals* the ones that belonged to somebody else. Tetris drives its frame loop with
`RTimer::After()` + `User::WaitForRequest()` on a bare `TRequestStatus` while ws events and
synchronous file IPC complete in the background, so signals routinely get borrowed and handed
back. When the borrowed-and-returned signal is finally picked up by the scheduler's
`WaitForAnyRequest`, the active object it "belonged" to has already been dispatched and re-armed
(`Active, Pending` in the AO dump) — the filter sees no ready AO and eats it.

One eaten signal is fatal: the next `User::WaitForRequest` on an already-completed status blocks
forever, because the completion has *already* happened and nothing will signal again. That is the
freeze. Key presses still got through because a ws key event completes a request the scheduler
*can* match, which buys exactly one more frame.

## Fix

`thread::wait_for_any_request()` already had a shortcut for the mirror-image case: a *direct*
`WaitForAnyRequest` that finds an empty semaphore but a ready active object returns without
consuming, because the signal was lost. The wrapper form needed the same treatment.

The wrapper's do-while always consumes one signal per turn, so a target status that is already
complete *must* have its signal in the semaphore. Arriving there with an empty semaphore is proof
the signal went missing. When that happens and the filter has absorbed at least one signal on this
thread, the wait is handed straight back and the debt is paid from `stray_absorbed_refund_` — the
budget only ever holds what the filter took, so no signal is invented.

This keeps the filter (Snakes/Final Battle still need it) while making a wrong guess
self-healing.

## Dead ends worth skipping next time

- **Chasing an over-signalling HLE path.** The follow-up doc points at
  `hle-ipc-complete` as the residual imbalance source, so a lot of effort went into per-status
  signal/wait ledgers, high-water-mark tracking and a "leaked completion" deque. For Tetris there
  is no imbalance at all — every signal has a matching completion. The bug is *identity*, not
  *count*: the filter cannot tell which request a counted signal belongs to.
- **`-LaunchAppUID 0x20007B38`.** Launching the N-Gage launcher directly by UID leaves a black
  screen and a fully idle emulator; go through the app list entry (labelled *Games*) instead.
- **`property_reference::~property_reference()` → `property::cancel()`.** It shows up prominently
  in signal backtraces during startup and looks like a double completion, but the subscription is
  erased from the queue on completion so it cannot fire twice.
- **Stale `elementRef`s.** `xcodebuildmcp ui-automation` refs must be re-snapshotted after every
  navigation; a stale ref silently taps nothing and looks exactly like a hang.

## Verification

- Tetris reaches a playable board with blocks falling, on both `dyncom` and `dynarmic`
  (`ios-use-jit: true`), with the filter still active (absorbed 1–4 per session, all refunded).
- `scripts/ios_regression_test.sh` 12/12 on both backends; `angrybirds` suite 5/5.
- Snakes (the title the filter was introduced for) still reaches 3D gameplay at 40–42 FPS with
  `absorbed=0` and no `E32USER-CBase 46`.
