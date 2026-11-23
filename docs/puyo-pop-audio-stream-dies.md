# Puyo Pop went silent a minute in and never made a sound again

## Symptom

Puyo Pop (N-Gage, `nem-4`) starts with music over the intro. Somewhere during the
attract sequence the sound stops, and nothing brings it back: the menus, the scenario
cut-scenes and the match itself are all silent for the rest of the session. The game
keeps running normally otherwise.

## Narrowing it down

The title imports exactly one audio DLL, `MEDIACLIENTAUDIOSTREAM`, so every sound it
makes goes through a single `CMdaAudioOutputStream`. That lands in
`src/patch/mediaclientaudiostream` (the guest-side patch DLL) on top of the
`eaudio_dsp_stream_*` dispatch calls, so tracing both ends of that pipe was enough to
see the whole conversation. Temporary probes on the dispatch entry points, on
`dsp_output_stream_shared`'s render callback, and on the EKA1 `RTimer::After` SVC
(filtered to the 100 ms and 10 ms delays the patch uses) told the story; timestamps in
the log pattern were what made it readable.

Two independent defects were behind it, and the first one hides the second.

### 1. A fabricated underflow killed the stream

Healthy streaming looked like this, one cycle per buffer:

```
notify_register → write(3200 bytes) → cb_request_more → more_buffer (completed)
```

The game writes 3200 bytes at 8 kHz mono — 200 ms of audio per buffer — and the patch
keeps exactly one buffer in flight: `CMMFMdaOutputBufferQueue::WriteAndWait()` hands us
a buffer and re-arms the buffer-ready request, and only the completion of that request
makes the game produce the next one.

The host asked for that next buffer far too late. `internal_decode_running_out()` fired
the request when the ring held `avg_frame_count_ * channels * 4` samples — four render
callbacks, about 46 ms. So a guest that had chosen 200 ms buffers was given 46 ms of
wall-clock to answer. Real MMF does the opposite: the client's buffer is copied to the
sound device on arrival and `MaoscBufferCopied` comes back immediately, so the client
always has a whole buffer period.

Miss that window once and the patch declares the stream dead:
`WriteAndWait()` finds an empty queue → `HandleBufferInsufficient()` starts a 100 ms
`CPeriodic` → `DataWaitTimeout()` sets `EMdaStateStop` and calls
`MaoscPlayComplete(KErrUnderflow)`. Puyo Pop never streams again after that. The trace
caught the exact moment:

```
14:05:27.016  more_buffer completed (ring 280 samples left)
14:05:27.062  timer_after us=100000  thread=Puyo Pop     <- queue was empty
14:05:27.164  timer_after us=100000  thread=Puyo Pop     <- CPeriodic re-arm, then underflow
   (silence for the rest of the session)
```

Note what is *not* in that trace: no `stream_stop`, no `stream_destroy`. `DataWaitTimeout`
only flips the guest-side state, so the hardware stream keeps being pulled and renders
silence forever — a good fingerprint for this failure, as opposed to a game that
deliberately stopped its music.

### 2. `Position()` reported the hardware's free-running clock

With the buffer budget fixed the intro survived, but the game still went quiet the
moment it switched music. This time the trace showed the *intended* teardown —
`notify_cancel`, `reset_stat`, `stream_stop` — followed by the game reading
`CMdaAudioOutputStream::Position()` and getting **98 seconds**, the entire previous
playback, instead of 0.

`eaudio_dsp_stream_position` returned `real_time_position()`, which was
`stream_->current_frame_position()` — the backing hardware stream's lifetime frame
counter. That counter ignores `reset_stat()`, keeps ticking through a virtual stop, and
keeps ticking through the silence we pad an empty ring with. The patch calls
`EAudioDspStreamResetStat` immediately before `EAudioDspStreamStop` precisely so the
next playback starts from zero, and that call did nothing.

It matters twice over, because Puyo Pop paces its writes off the position: its
`MaoscBufferCopied` handler reads `Position()`, mixes, and writes one buffer. A position
that runs ahead of the audio actually played makes the game see a multi-buffer jump and
skip a round — which is exactly how the second death started, right after a buffer-ready
notification had to be deferred by 40 ms because the render thread lost the race for the
kernel lock.

### Dead ends

- The deferred-notification path (`defer_stream_buffer_notify`) looked like a good
  suspect for a lost wakeup. It is not: the notifications are drained from `resolve()`
  and from the emulation loop, and the trace shows them completing.
- `eaudio_dsp_stream_stop` never releases the audio-renderer semaphore on `epoc6`
  (it returns early with `1`, the "call `MaoscPlayComplete`" signal). That leak is real
  but harmless — the semaphore only gates teardown — and is not this bug.
- "The guest was starved of CPU" is tempting when the intro runs at 4 FPS, but the
  trace shows the guest answering every notification in a steady ~40 ms right up to the
  moment it stopped.

## Fix

Host side, `src/emu/drivers/src/audio/backend/dsp_shared.cpp` — this is what actually
brought the sound back:

- Ask for the next buffer while the ring still holds a whole guest buffer, instead of
  four render callbacks. `write()` records the size of the last buffer the guest handed
  over and `low_water_mark_samples()` uses it, floored at the old hardware-driven value
  (so titles with small buffers, like Asphalt 2's 32 ms writes, are unaffected) and
  capped at half a second of lookahead.
- Report `real_time_position()` from `samples_played_` — samples actually taken out of
  the ring — which is what a real device's position reflects, does not advance through
  padded silence or a virtual stop, and is cleared by `reset_stat()`.

Guest side, `src/patch/mediaclientaudiostream` — contract fixes found while reading the
same path, none of them required for Puyo Pop but all of them wrong as they stood:

- `KWaitBufferTimeInMicroseconds` 100 ms → 500 ms. It is the grace period between "the
  sound device ran dry" and "the client must be finished, report KErrUnderflow", and
  100 ms is far tighter than anything a real client would need. A client that announced
  the end of its stream (`KeepOpenAtEnd` + `RequestStop`) still gets the completion
  immediately, so this only relaxes the guess.
- `CMMFMdaOutputBufferQueue::CleanQueue()` now hands back *every* buffer the client
  still owns with `MaoscBufferCopied(KErrAbort, ...)`, not only the one in flight; the
  rest used to be deleted silently. It moves the queue into a local list first, because
  the callback is free to write again or stop the stream from inside it.
- `RunL()` clears `iCopied` after deleting the node. `WriteAndWait()` normally
  reassigns it, but it is skipped when the request was aborted, which left `CleanQueue()`
  a freed node to report.
- `CMdaAudioOutputStream::Open()` / `CMdaAudioInputStream::Open()` no longer drop
  `MaoscOpenComplete` when the stream is already open — a client waiting for it was
  stranded forever. An open that is genuinely still in flight is still ignored (a second
  `CTimer::After` on the same timer would panic).

Deliberately *not* changed: `Stop()` still delivers `MaoscPlayComplete` synchronously
from inside the client's own call, which Symbian never does. The surrounding comments
show the error code and its timing were tuned against KoF and Ashen, neither of which is
available here to re-test, so this stays as it is.

The one-buffer-in-flight design also stays. It is self-consistent: the buffer-ready
notification means "I am about to run dry", so an empty client queue at that moment is a
sound underflow test. Letting the client queue ahead the way real MMF does would need the
host to tell the patch how much unplayed audio it still holds, i.e. a new dispatch call.

## Verification

Puyo Pop on `nem-4` keeps streaming through the intro, the menus, the scenario cut-scene
and a full match, with the only gap being the ~2 s the game itself stops the stream for
while it switches tracks; a whole match and the way back out to the menu runs without a
panic. Asphalt 2 (same device, same `v81a` patch, 32 ms buffers) still plays its title
track and races at 60 FPS. The iOS regression suite passes 12/12, and the `angrybirds`
suite — which is the one title here that loads `mediaclientaudiostream` on an EKA2
device, so the `general` build of the patch — passes 5/5.

Both patch binaries were rebuilt from the changed source: `v81a` with S60 2nd FP3 under
`C:\Symbian\8.1a` (`armi urel`, the classic GCC), `general` with the S60 5th Edition SDK
(`gcce urel`, the SDK's own CSL toolchain). Leaving one of them stale would have shipped
two different implementations of the same file.
