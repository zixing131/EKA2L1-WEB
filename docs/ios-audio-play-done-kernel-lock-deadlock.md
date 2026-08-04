# The play-done callback blocks on the kernel lock and deadlocks a `Play()`

## Symptom

TestFlight build 260793 (commit `f774cb08`, exact dSYM UUID match
`778B9C1C-…`) was killed by iOS with a `0x8BADF00D` **scene-update watchdog**
transgression: "exhausted real (wall clock) time allowance of 10.00 seconds".
The process had been alive for eight minutes and the watchdog CPU statistics
read `Elapsed application CPU time (seconds): 0.061, 0% CPU` — the app was not
busy, it was stuck.

The `.ips` is a four-thread pile-up, and unlike the earlier
[leaked-lock hang](./ios-mmf-callback-kernel-lock-leak.md) the owner of the
contended mutex is right there in the report:

| Thread | State |
|---|---|
| main | `-[UIWindow _sendTouchesForEvent:]` → `submitPointerEventAtX:…` → `window_server::handle_input_from_driver` → `std::mutex::lock()` |
| Timing | `ntimer::advance` → `animation_scheduler::scan_for_redraw` → `std::mutex::lock()` |
| Symbian OS | `InterpreterMainLoop` → `lib_manager::call_svc` → `dispatcher::resolve` → `eaudio_player_play` → `player_shared::play()` → `audiounit_ios_output_stream::stop()` → **`AURemoteIO::Stop()`** (`mach_msg`) |
| `AURemoteIO::IOThread` | `output_render_cb` → `player_shared::data_supply_callback` → the `eaudio_player_notify_any_done` lambda → `std::mutex::lock()` |

## Root cause

A textbook lock inversion, spelled out by those two bottom rows:

* `hle::lib_manager::call_svc` takes `kernel_system::kern_lock_` around the
  **whole** dispatch call ("Lock the kernel so SVC call can operate in safety"),
  so every `eaudio_*` dispatch function runs with the kernel lock held.
* `player_shared::play()` stops the previous stream before starting a new one,
  and `AudioOutputUnitStop()` is synchronous: it does not return until the
  render callback in flight has finished.
* That in-flight callback is the guest's play-done notification, and it called
  `kern->lock()` (blocking) to complete the request.

So the guest thread waits for the render thread while holding the lock the
render thread is waiting for. Everything else that later wants the kernel lock —
touch delivery on the main thread, the animation scheduler on the timing thread —
piles up behind it, the scene update never completes, and FrontBoard kills the
app after 10 s.

Nothing here is iOS-specific in principle; iOS just makes it fatal, because a
desktop host would only hang while CoreAudio's synchronous stop turns the hang
into a watchdog kill.

## Why the neighbouring code did not have this bug

The invariant *"the audio render thread must never block on the kernel lock"* was
already established in this tree, twice:

* the MMF `more_buffer` callback (`services/audio/mmf/dev.cpp`) uses
  `std::unique_lock<kernel_system>(*kern, std::try_to_lock)` and returns `false`
  (retry later) when it cannot get the lock;
* the dispatch DSP-stream `more_buffer` callback (`dispatch/src/audio.cpp`) does
  the same with a bare `kern->try_lock()`.

`dispatcher::on_process_exit` even carries a comment explaining exactly this
hazard for the teardown path, which is why orphaned mediums are destroyed from
`flush_pending_teardown()` instead of inline. The dispatch **player**'s
play-done callback was the one place still taking the lock unconditionally, and
it is on the hottest path there is: a game restarting a sound effect.

## Fix

`eaudio_player_notify_any_done`'s callback now tries the kernel lock, and when it
is busy hands the notification to the emulation thread instead of waiting:

* `dispatcher::defer_player_notify(handle)` records the *player handle* — not a
  copy of the `notify_info`.
* `dispatcher::complete_deferred_player_notifies_locked()` completes whatever
  request the player has armed at that moment (validating the requester against
  the live thread list, as the render thread did) and clears the notify slot.

Deferring the **handle** rather than the notification is what keeps the guest
semantics identical. A player has a single notify slot; had the render thread
been able to take the lock, it would have completed whatever sat in that slot and
cleared it. Completing a *copy* later would instead signal a request the guest
may have re-armed on the same `TRequestStatus` in the meantime — one completion
too many, i.e. a `E32USER-CBase 46` stray-signal panic waiting to happen. Looking
up the player at delivery time also makes a destroyed player or a cancelled
request a no-op for free.

Delivery happens at two points, both *before* any further guest-visible action:

* the top of `dispatcher::resolve()` — already under the kernel lock, and ahead
  of the dispatch call that may be the guest arming or cancelling its next
  request;
* `flush_pending_teardown()` on the emulation thread, which runs before each CPU
  slice and covers the case where the lock was held by some other thread.

Note the deferral is not a rare path: during any dispatch call (GLES included)
the guest thread holds the kernel lock, so a callback landing there always
defers. The delay is bounded by the next SVC or the next loop iteration.

## Verification

Standard regression 12/12 and the Angry Birds touch-guest suite (a title that
restarts sound effects constantly) on the Release simulator build.

Two things worth knowing for next time:

* While reproducing, `xcrun simctl io … screenshot` wedged and stayed wedged,
  which made the Angry Birds suite look hung. `sample`-ing the app showed every
  emulator thread healthy (interpreter running, GL commands flowing, audio
  rendering) — the wedge was in CoreSimulator, not the emulator. Killing the app
  process released it. Sample first, do not assume the app is the hung one.
* The suite's carousel-swipe assertion failed while the swipe visibly worked
  (episode 1 → 3), on this change *and* on a stashed HEAD build: it reused
  `AB_DIFF_MIN` (150000), a threshold meant for full-band transitions, while a
  carousel page change repaints only the cards and dots. Measured on the
  iPhone 16 Pro simulator: a settled episode screen with no input scores exactly
  0 differing pixels, real swipes 29k–63k, full transitions 110k–250k. The check
  now has its own `AB_SWIPE_DIFF_MIN` (8000). When a visual assertion fails,
  compare the saved screenshots before believing it.
