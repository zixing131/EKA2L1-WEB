# Killer Virus counts time in frames, not in seconds

> **One-line conclusion**: *Attack of the Killer Virus* (N70/rm-84, `uid3=0x629E1011`)
> never reads a clock. It advances its state by a fixed 125 ms per rendered frame — an
> 8 fps design target — and EKA2L1 runs that loop at about 16 fps, so the on-screen
> TIME counter runs roughly twice real time.

## Symptom

The in-game `TIME mm:ss` readout advances far faster than the wall clock, and a round
ends after a few seconds of real time. The game is an augmented-reality shooter: it
draws sprites over the camera viewfinder, so on the iOS simulator the background is the
mock camera's test pattern.

## Narrowing it down

**The game's time API surface.** `killervirus.app` is an uncompressed E32 image
(`compression_type == 0`), so its import section can be read directly: 11 DLLs, 45
EUSER ordinals. Because EKA1 export ordinals cannot be symbolised against any EKA2 SDK,
the ordinals were resolved through the ROM instead — `z:\rm-84\system\libs\euser.dll`
is a `TRomImageHeader` image whose export table maps ordinal to code address, and each
exported function's first reachable `SVC` immediate names the executive call. The
mapping from file offset to address is `vaddr - (iCodeAddress - 100)`; the dumped file
holds header + text + a truncated export table, and the entry point sits at offset 100.
That gave `RTimer::After` (`0xC00048`), `RTimer::Cancel` (`0xC00047`), `TTime` /
`User::UTCTime` (`0x80006E`), the active-scheduler and trap machinery — and no
`User::TickCount`, no `NTickCount`, no fast counter.

**What the loop actually executes.** A temporary aggregating probe in
`lib_manager::call_svc` (count SVCs per thread, dump once a second) is far cheaper than
`log-svc` and gives the complete picture. During gameplay, per second:

```
timer_after_eka1 x15   session_send_sync_eka1 x120   session_send_eka1 x30
wait_for_any_request x201   active_scheduler x76   clear_inactivity_time x15 ...
```

Fifteen loop iterations per second, and **not a single time-related executive call**.
`time_now` appears only in the menu (3/s) and never once the round starts. A game that
never reads a clock cannot be measuring elapsed time; it can only add a constant per
iteration.

**The constant.** The interval argument of every gameplay `timer_after_eka1` is
`100` microseconds — the game asks for the shortest possible delay and paces itself on
whatever throttles the loop. Varying the simulator's mock camera rate
(`VIEWFINDER_FPS` in `camera_simulator.mm`) moves the loop rate and the clock rate
together:

| mock camera | guest loop | TIME rate | implied step |
| --- | --- | --- | --- |
| 5 fps | 5.0 /s | 0.63x | 0.127 s |
| 15 fps (default) | 15 /s | 1.98x | 0.132 s |
| 30 fps | 16 /s (host-bound) | ~2.1x | — |

The step is a constant 0.125 s per frame: the game was written for 8 fps. At 30 fps the
loop stops following the camera and settles at 16 — that is this host's ceiling for
dyncom plus the simulator's software GLES, and it is about twice what an N70 achieved.

**Confirmation.** EKA2L1 already has a per-application frame limit
(`compat/<UID>.yml`, key `fps`, surfaced in the iOS and Qt settings). With `fps: 8` the
loop runs at 8/s and the clock tracks real time:

| sample | TIME | wall clock |
| --- | --- | --- |
| 1 | 00:03 | 0.00 s |
| 2 | 00:09 | +5.54 s → 1.08x |
| 3 | 00:14 | +11.11 s → 0.99x |

So there is no timing defect to fix in the kernel: the engine ties simulated time to
its own frame rate, and the emulator is simply faster than the phone. The per-app frame
limit is the remedy, the same class of problem and remedy as Ferrari GT's fixed 40 ms
step.

## A second, independent observation

The cadence the guest sees from the legacy Nokia CameraServer is the *host* camera's
cadence. `GetImage` in this protocol is a per-poll still capture, not a streaming
viewfinder; EKA2L1 answers it from a free-running host feed, so the guest's frame rate
follows whatever the host produces — 15 fps from the iOS simulator's mock camera, the
session default (typically 30) from `camera_ios.mm`, and the native rate on the Qt and
Android backends, none of which set a frame duration. The same guest therefore runs at
different speeds on different hosts. For this game the host work ceiling dominates, so
capping the feed would not have been the whole story, but the host dependence is worth
removing on its own.

## Dead ends worth skipping

- Suspecting the tick-queue rounding work: `RTimer::After(100us)` already rounds up to
  the 64 Hz grid, which can only make the loop *slower*. Unrelated.
- Suspecting `User::After`, `TTime`/`User::UTCTime` quantisation on EKA1, or the EKA1
  `TICK_MASK` in `tick_count`: the game reaches none of them during a round.
- Suspecting camera frame-rate negotiation: a probe on `camera_session::fetch` shows
  the game sends only opcodes 0 (turn on), 2 (lighting), 3 (quality) and 4 (get image).
  The protocol has no rate field at all, so nothing is being dropped by the
  `default: error_not_supported` arm.
- Reading the E32 import block ordinals as code offsets: that is the post-EKA2 ELF
  form. Old petran images store the ordinals inline.
- Walking an EUSER export until a return instruction: EKA1 executive stubs are bare,
  consecutive `SVC` instructions, so a naive walk falls into the stub table and reports
  sixty unrelated executive calls. Stop at the first `SVC` reached through a call.
