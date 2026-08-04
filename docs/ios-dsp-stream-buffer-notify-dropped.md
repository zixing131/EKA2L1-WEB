# Stuttering streamed audio: dropped DSP buffer-ready notifications

## Symptom

Asphalt 2 (Urban GT 2) on the N-Gage (NEM-4, EKA1) device, Release build: the race
runs at a full 60 FPS but the in-race audio stutters continuously — short dropouts
roughly once or twice a second. The title screen and menus, including the streamed
"Moby - Lift Me Up" music, sound clean; only entering a race breaks the audio.

## How it was narrowed down

The menu/race split was the first useful clue. `asphalt2.app` imports both
`MEDIACLIENTAUDIO` and `MEDIACLIENTAUDIOSTREAM`, and instrumenting the two host
paths separately showed they are used in different places:

* menus → `CMdaAudioPlayerUtility` → `dsp_epoc_player` → one `audio_output_stream`,
  created once and left running,
* race → `CMdaAudioOutputStream` → `dsp_epoc_stream` → `dsp_output_stream_shared`,
  a guest-fed ring buffer.

So the problem had to be in the streaming path, not in the AudioUnit backend. A
temporary probe in `dsp_output_stream_shared::data_callback()` counting callbacks,
underruns, buffer-ready requests, guest writes and the ring-buffer low-water mark,
plus timers around `create_unit`/`start_unit`/`stop_unit`/`dispose_unit`, gave this
per-second picture while racing:

```
dsp freq=16000 ch=1 avgframes=185 cb=87 underrun=0 notify=54 fail=23 writes=31 wsamples=15872 ringmin=9
dsp freq=16000 ch=1 avgframes=185 cb=87 underrun=0 notify=51 fail=20 writes=32 wsamples=16384 ringmin=13
```

Two numbers stand out. `fail` is the number of buffer-ready notifications the driver
tried to deliver and could not — **29% of all requests over a 97-second run**. And
`ringmin`, the smallest ring level observed in a second, repeatedly fell to 9–20
samples, i.e. well under a millisecond of audio, against a design target of ~46 ms.
The stream was permanently running on fumes, and every dip to zero is an audible gap.

The failure path is `eaudio_dsp_stream_create_impl()`'s more-buffer callback:

```cpp
if (!kern->try_lock()) {
    return false;          // dropped
}
```

`try_lock` rather than `lock` is correct and deliberate — this callback runs on the
CoreAudio render thread, and blocking it on the kernel lock deadlocks against a guest
thread that stops or destroys a stream from a dispatch call (see
`ios-audio-play-done-kernel-lock-deadlock.md`). But returning `false` makes
`dsp_output_stream_shared::data_callback()` clear `more_requested` and simply try
again on the *next* render callback, one full hardware buffer later (11.6 ms here).

That retry is far too coarse for how often the lock is actually taken.
`dispatcher::resolve()` holds the kernel lock for the whole of every HLE dispatch
call, and the window server's redraw runs under it on the ntimer thread — during a
race both are busy. With a ~30% miss rate, a run of four consecutive misses (≈0.7%,
so several times a second at 87 callbacks/s) is enough to drain the ring: the
low-water mark is only `avg_frame_count * channels * 4` ≈ 740 samples, and the game
writes 512-sample (32 ms) buffers, one at a time, because the
`mediaclientaudiostream` patch keeps exactly one buffer in flight.

Dead ends worth skipping:

* The AudioUnit lifetime cost is real but not the cause here. The very first
  `create_unit` + `start_unit` costs ~76 ms, but Asphalt 2 mixes its own effects into
  the single stream — the whole race produced only two unit creations. (A title that
  calls `CMdaAudioPlayerUtility::Play()` per sound effect *would* pay it every time:
  `player_shared::play()` always tears down and recreates its output stream.)
* Emulation speed is not the cause either — the guest wrote 15.9–16.4 k samples per
  second against the stream's 16 kHz, i.e. exactly real time.

## Fix

Do not drop the notification. When the render thread loses the race for the kernel
lock, hand the stream's buffer-ready notification to the emulation thread, which
delivers it under a proper `lock()`. This mirrors what the player path already does
with `dispatcher::defer_player_notify()`; the stream path had simply never been given
the same treatment.

`dispatcher::defer_stream_buffer_notify()` queues the stream handle;
`complete_deferred_stream_notifies_locked()` completes it from
`dispatcher::resolve()` (already under the kernel lock, before the dispatch call, so
a guest that re-arms or cancels in that very call sees the completion first) and from
`flush_pending_teardown()` on the emulation loop. Both run far more often than the
audio callback, so the notification now lands in microseconds rather than being lost.
A handle that no longer resolves, or whose requester thread has died, is dropped the
same way the player path drops it.

The callback registration moved to after `manager.add_object()` so it can name its
own stream by handle; nothing can fire it in between, because the stream has not been
started yet.

Same probe, same race, after the fix:

```
dsp freq=16000 ch=1 avgframes=185 cb=87 underrun=0 notify=32 fail=0 writes=32 wsamples=16384 ringmin=559
```

Over 97 seconds: 0 failed notifications (was 1273 of 4335), `notify` now equals
`writes` one-for-one instead of burning retries, and the ring-buffer floor never fell
below 100 samples — it sits at 378–572, the level the low-water design intends.
