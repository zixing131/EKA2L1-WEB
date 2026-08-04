# Dragon.World (6680) flickers black on every server recomposite

## Symptom

Dragon.World (`0x102735C5`, S60v2 / Nokia 6680, EKA1) flickers hard on a real
device: the guest picture drops to solid black for a frame at a time, several
times a second, from the title screen onwards. Every other 6680 title on the
same ROM renders fine, and the same build in the iOS Simulator looks perfectly
stable.

## Narrowing it down

The simulator/device split was the useful lever. Recording the simulator with
`simctl io recordVideo` and scoring per-frame mean luminance found **zero** dark
frames over 8s and 20s captures — the two black frames each capture contained
sat at the very start and end and are recorder artifacts, not emulator output.
So the bug was real but hidden by something the simulator does differently.

`EmulatorViewController.renderScale` caps the simulator's GL surface at 1.5x
(software GLES makes the present blit the frame bottleneck), while a device
renders at the native 3.0x. Removing that cap reproduced the flicker
immediately: 160 black frames out of 584. That made it a local, fully
scriptable repro, and it also ruled out "the device is faster" as the cause —
the trigger is the surface size shifting the timing of the compositing passes,
not raw CPU speed.

From there each layer was eliminated by making the emulator paint a distinctive
colour and checking what the flicker frame actually contained:

- Present clear colour → red. The flicker frames stayed **black**, and looking
  at the *whole* screenshot instead of the cropped guest area showed the red
  letterbox, the keypad and the FPS badge all still rendering normally. Only
  the guest picture rectangle went black, so `submit_screen_frame` was fine and
  `screen_texture` itself was black.
- `screen::redraw` clear colour → green. The flicker frames turned solid green.
  That pinned it exactly: the `FLAG_SERVER_REDRAW_PENDING` path clears the
  screen bitmap and then replays the window tree, and the replay was putting
  nothing back.
- Dumping the redraw store on every server pass gave the answer. In steady
  state the window held exactly one segment, of type `redraw`, containing one
  command: opcode 6, `gdi_store_command_set_clip_rect_single`.

Dead ends worth not repeating:

- **EAGL buffer rotation.** `kEAGLDrawablePropertyRetainedBacking` is `@NO`, so
  a present that isn't paired with a full redraw would show an undrawn buffer.
  Flipping it to `@YES` changed nothing (127/466 black frames, same ratio).
- **Renderbuffer being recreated.** `bind_swapchain_framebuf` re-runs
  `renderbufferStorage:fromDrawable:` whenever a new surface size is pending,
  which would wipe the backing store. It fires once per session, not per frame.
- **GL-side loss.** A `glReadPixels` right before `swap_buffers` read
  `255,0,0,255` on *every* present — the drawing always landed in the
  colorbuffer correctly.
- **Segment aging.** `clean_old_nonredraw_segments` erases old non-redraw
  segments and invalidates the window, which looked like a plausible source of
  half-drawn frames. It never ran (0 hits over 40s).

## Root cause

The game only sets a clipping rectangle between `BeginRedraw` and `EndRedraw`;
it paints its actual picture through the non-redraw path. `add_draw_command`
marked the window as `content_changed` for *any* stored command, including that
lone clipping opcode, so `end_redraw` requested a full server recomposite.

That recomposite clears the screen bitmap and rebuilds it purely from the redraw
store — and a store whose only command is "set clip rect" draws nothing. The
screen bitmap stayed at the clear colour until the game's next non-redraw paint
put the picture back, which is the black flicker. On a 1.5x surface the timing
happened to land the next paint before the frame was presented; at 3.0x it did
not.

## Fix

Treat only commands that actually put pixels down (`draw_rect`, `draw_line`,
`draw_polygon`, `draw_bitmap`, `draw_text`, `update_texture`) as content
changes. A clipping opcode on its own leaves the window exactly as it was, so it
must not trigger a recomposite that the store cannot satisfy.

`gdi_store_command_draws_pixels()` in `gstore.h` carries the predicate;
`redraw_msg_canvas::add_draw_command` uses it to gate `content_changed(true)`.

Verified on the reproduction build (3.0x simulator surface): 0 dark frames in
526, against 125/468 before. The standard Release regression suite passes.

An adjacent hardening — having `promote_last_segment` discard a redraw segment
that drew nothing, instead of letting its region eliminate the stored non-redraw
commands that still hold the window's pixels — was implemented and measured
separately. It did not change the outcome on its own and was dropped to keep the
fix minimal.
