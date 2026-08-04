# Audio keeps playing after "Exit Game"

## Symptom

N97 (RM-507) ROM, Galaxy on Fire, sound enabled. Opening the iOS overlay menu and tapping
**Exit Game** returns to the app list — but the game's music keeps playing forever. Only
killing the whole iOS app (or launching another guest app, which reboots the session) stops
it.

## Narrowing it down

The screen and process teardown clearly worked (the app list came back), so the question was
which host object still owned a running audio stream.

`sample <pid>` on the simulator process was enough to answer that without touching the code:
an `AURemoteIO::IOThread` exists and shows `AURemoteIO::PerformIO` → `RenderBus` frames only
while an output AudioUnit is *started*. Those frames were present before the exit and, at the
time, still present several seconds after it. That ruled out "the sound is a stale buffer" and
pinned the leak on a live `audiounit_ios_output_stream`.

Where that stream comes from: `data/patch/mediaclientaudio*.dll` is patched, so the game's
`CMdaAudioPlayerUtility` calls land in the dispatcher's `eaudio_player_*` bridge rather than
in a Symbian server. Those create `dsp_epoc_player`/`dsp_epoc_stream` objects inside
`dispatch::dsp_manager`, which is a **process-agnostic global container** owned by the
dispatcher — the guest is expected to release them from `CMdaAudioPlayerUtility`'s destructor
(`eaudio_player_destroy`).

That is exactly what a killed process never does. `process::kill()` closes handles and thus
tears down HLE *server sessions* (the MMF DevSound path frees its stream in
`~mmf_dev_server_session`), but the dispatcher objects are not kernel objects and nothing
referenced them anymore. The player kept decoding and the RemoteIO unit kept pulling.

A temporary log in the exit path confirmed the scale: Galaxy on Fire leaves **32** live
mediums behind at exit.

## Two hazards that shape the fix

1. **The kernel lock.** Destroying a medium stops the host stream, and `AudioOutputUnitStop()`
   waits out the render callback in flight. That callback takes `kernel_system`'s lock to
   complete guest notifications (`complete_audio_notify_if_alive`). The iOS frontend kills the
   process while *holding* that lock (`closeRunningApp`), so destroying the objects inline in
   `process::kill()` would be a lock-order inversion — a real hang, not a theoretical one.
   Teardown must therefore be deferred out of the killing context.

2. **The parked emulation loop.** The natural deferred-destroy point is the emulation thread
   (`system_impl::loop`, which holds no kernel lock). But once the app list is back, iOS parks
   the OS thread: `state->paused || !state->mounted` makes it sleep in 16 ms ticks and never
   call `loop()` again. A first version of the fix orphaned the mediums correctly and still
   played music forever, because the flush queue was never drained. This was visible in the
   log as the "process exit" trace with no matching "flush" trace.

## Fix

- `kernel_system` gains a `process_exit_callback`, invoked from `process::kill()` (the single
  funnel for normal exit, host kill and panic) whenever the kernel is not wiping out.
- Every `dsp_medium` records the unique ID of the process that created it. On process exit the
  dispatcher *detaches* — moves out, does not destroy — every medium of that process into a
  pending list. Detaching only takes the manager's own small mutex, so it is safe under the
  kernel lock.
- `dispatcher::flush_pending_teardown()` destroys the pending mediums. It is called from
  `system_impl::loop()` for guest-side deaths (a panicking app, an app that exits while the
  loop runs) and, on iOS, from `closeRunningApp` right after the kernel lock is released — the
  only safe point that is reached when the loop is about to be parked.

The result is the general contract one expects: HLE state a process owns dies with the
process, however it died, instead of relying on the guest's destructors running.

## Verification

`AURemoteIO` frames present at the game menu, zero frames a few seconds after Exit Game; the
flush trace showed all 32 mediums destroyed. Full iOS regression suite re-run afterwards.

## Not covered

`video_player_container_` and `cameras_` in the dispatcher have the same ownership shape and
are still only released on `dispatcher::shutdown()`. No reproduction was available for those,
so they were left alone; the process-exit hook is the place to extend if one shows up.
