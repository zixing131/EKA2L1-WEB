# AtomShift shows a level only after you touch the screen

## Symptom

On the X7 (rm-707), AtomShift's `Play → Basic Lab → Play` left the level-select
screen on the display. The level appeared only after touching the screen
anywhere inside the guest area — any touch, not a meaningful one. Two
screenshots taken seconds apart while stuck were byte-identical.

## The guest was running, the frame was not

`sample` on the stuck process ruled out a lost wakeup: the Symbian OS thread was
executing guest code (1741 of 1841 samples inside `InterpreterMainLoop`,
including `eaudio_player_get_position` dispatches), while the graphics thread sat
idle in `request_queue::pop` — no draw commands were being submitted at all. So
the guest was alive and the window server simply never composited.

A probe in `egl_swap_buffers_emu` printed the OpenVG coverage bookkeeping per
swap and named the mechanism immediately:

```
SWAP surface=1 cov=2 publish=true  stable=2     (level select)
SWAP surface=1 cov=1 publish=false stable=2     (level entry - withheld)
                                                (no further swap, ever)
SWAP surface=1 cov=1 publish=true  stable=1     (only after a touch)
```

The withheld swap came from the OpenVG "coverage drop" heuristic added for
Talking Tom (`07c0ed23c`): a swap that draws fewer full-window images than the
established baseline is treated as an intermediate construction frame and
deferred once, on the assumption that the *next* swap replaces it. AtomShift is
event-driven — it renders one frame per user action and then stops — so the next
swap never came and the deferred frame was withheld indefinitely. Touching the
screen made the guest redraw, and that swap published.

This never affected upstream: the heuristic exists only on this fork's branches.

## Why the heuristic existed, and why it is gone

The heuristic was compensating for a real black frame in Talking Tom, but not
for the reason its original write-up assumed, so the deeper question was whether
the emulator was losing a frame. It is not. Tracing the guest's OpenVG calls per
swap (`vgClear` / `vgDrawImage` / `vgImageSubData` / `vgDrawPath`, plus the
transformed coverage quad of every image draw) gives a fixed sequence per frame:

```
normal frame: 360x640 @ (0,0)-(360,640)   black backdrop, covers the surface
              320x480 @ (0,0)-(360,640)   the cat, uploaded fresh every frame
              7 small images              buttons and icons
black frame:  360x640 @ (0,0)-(360,640)
              (the cat is missing - not even uploaded)
              7 small images
```

There is no `vgClear` anywhere, and nothing on the host clears the EGL surface,
so this is not lost content: the guest's own opaque backdrop covers the previous
frame and it then skips the scene. The animation frames are JPEGs
(`!:\private\e0c58322\animationsbk`, 573 of them) decoded through the ROM's ICL
stack (`imageconversion.dll`, `iclextjpegapi.dll`, `jpegdecoder.dll`) — entirely
emulated ARM code. When decoding cannot keep up with the guest's own render
cadence, the guest draws a frame with no cat in it. The emulator reproduces that
faithfully.

The CPU backend confirms it. Same app, same actions, heuristic disabled:

| backend | dark frames | deferred swaps |
| --- | --- | --- |
| dyncom | 12 / 478 (old pacing), 4 / 416 (final code) | frequent |
| dynarmic (JIT) | 0 / 557 | 2 / 357 swaps |

Rendering FPS was identical (7-8 in the simulator, which is GL-bound), so the
difference is purely how fast the guest gets its next JPEG decoded. A host-side
heuristic that hides guest frames is therefore compensating for emulation speed,
at the cost of being wrong for any client that stops drawing — which is exactly
the AtomShift bug. It is removed, together with the OpenVG full-surface draw
counter and the per-surface coverage state that fed it.

## The composite pacing bug found along the way

Timestamped probes on swap, `try_update` and `animation_scheduler::
invoke_due_animation` showed a second, independent defect:

```
t=87551275 SWAP (construction frame; previous swap was 124ms earlier)
t=87551661 TRYUPD wait=0          <- "idle for more than a frame, composite now"
t=87552295 COMPOSITE              <- half-built frame on screen after 1.0ms
t=87553473 SWAP (completed frame, 1.2ms later)
t=87553834 TRYUPD wait=25996      <- but this one waits for the frame boundary
t=87580114 COMPOSITE              <- 27.8ms of black
```

`canvas_base::try_update` paced composites at the screen refresh rate, but only
when the window had drawn within the last frame interval; a window idle for
longer got `wait_time = 0` and was composited immediately. Real hardware does
the opposite: swaps landing between two vsyncs coalesce and only the last state
is scanned out. The fast path meant a client that pauses between scenes had its
first swap published on arrival, so a construction frame that its successor
replaced 1.2ms later was still shown for a full frame interval.

`try_update` now always schedules on the next frame boundary, taken on the
global clock so every window shares the same boundaries. Steady-state behavior
is unchanged (a client drawing every frame was already waiting out the interval);
only the first update after an idle gap is affected, and it now coalesces with
whatever follows it inside that interval.

## Result

AtomShift enters a level without any touch. Talking Tom still flashes its own
black frames under dyncom — 4 in 416 sampled frames across three action
transitions, down from 12 in 478 before the pacing fix — and effectively none
under the JIT. Standard regression suite 12/12, Angry Birds suite 5/5 with the
menu still at 60 FPS (the GLES path keeps its Window Server pacing and its
blocking drawer).

## Dead ends

- **Bounded deferral** — the obvious minimal fix: keep the heuristic, but
  composite the deferred frame anyway after a timeout. A 100ms bound on top of
  the old pacing measured *worse* than doing nothing (2 dark frames vs 0),
  because the pacing fast path published the construction frame before the
  timeout mattered. With the pacing fix and a 300ms bound it did work — 0 dark
  frames in 664 across five transitions, AtomShift entering its level on its
  own. It was dropped anyway, as a policy call rather than a measurement: it
  keeps the host guessing at guest intent, delays every genuine transition of an
  idle client, and hides a guest behavior instead of making the guest fast
  enough to avoid it.
- **Reading the probe log by swap boundaries.** The coverage verdict is computed
  *before* the draws of the following frame appear in the log, so grouping ops
  by "the swap line above them" attributes every frame's drawing to the wrong
  verdict — and makes deferred and normal frames look identical. Group by the
  verdict line instead; with the correct alignment the missing 320x480 image
  stands out immediately.
- Rebuilding `docs/ios-talking-tom-audio-and-frame-composition.md`'s conclusion
  from screenshots. Single-frame events need video: record with `simctl io
  recordVideo`, split at 20fps with ffmpeg, and score the guest region's mean
  luminance with ImageMagick.

## Where the real fix is

Guest-side JPEG decoding is the bottleneck behind the black frames. Options, in
increasing order of effort: keep the JIT (already opt-in, sideload only),
finish the Metal/ANGLE work so the simulator and device stop being GL-bound, or
HLE the ICL JPEG path — a patch DLL in `src/patch` that forwards decoding to a
host decoder, in the style of the existing `ecam` and `mediaclientaudio`
patches. Until then, a slow guest simply shows the frames it actually drew.
