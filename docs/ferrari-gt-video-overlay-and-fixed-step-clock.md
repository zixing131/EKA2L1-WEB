# Ferrari GT on the X7: a flickering intro movie and a race clock running 20% fast

Two separate complaints about the same title (Ferrari GT Evolution, UID `0x20024074`,
rm-707): the intro movie flashes white every couple of seconds, and once a Quick Race
starts the in-game clock runs visibly faster than real time. They turned out to have
nothing to do with each other.

## The intro movie flashes white

The later [surface-binding investigation](./window-surface-binding-and-video-composition.md)
revisits the overlay explanation below. ScreenPlay replaces a window's background
surface; the successful replay workaround does not establish a permanent video-over-EGL
priority. The measurements here remain the history of that workaround.

Recording the simulator and sampling the video strip made the flicker measurable:
roughly 14 frames out of a 25-second capture were a uniform white rectangle exactly
covering the guest screen, with normal video on either side.

The window that carries the movie (`win 9`) is a full-screen redraw canvas whose
background colour is the default `0xFFFFFFFF`, so "the whole guest screen went white"
and "the video window was cleared" are the same picture. That made the window server's
recomposite path the obvious suspect: `redraw_msg_canvas::draw` clears the background
and replays the stored GDI commands, and a video frame is not in that store — the
decode thread draws it straight into `driver_builder_`. Teaching the canvas to keep the
last posted frame and replay it there was the first fix.

It changed nothing. A probe on that branch showed why: over a 45-second run with the
movie playing, the server-recomposite branch never executed for that window. Neither
did any of the background-clear paths (three calls in a whole session). Nothing in the
window server was painting the screen white.

What *was* running, 490 times in those 45 seconds, was `eglSwapBuffers` on the very
same window. The game keeps its GL loop going while the movie plays, and the frames it
submits are empty — just the clear colour. EKA2L1 draws both the EGL surface and the
video frame into one window, so whichever arrives last wins; when a swap was the last
thing in a compositor interval, the screen showed the game's blank frame instead of the
movie. On the real device the posted video is an overlay above the window's own
content, not something the client's GL frames paint over.

Fix: the canvas now owns the last posted video frame (`set_posted_video_frame` /
`draw_posted_video_frame`), and `eglSwapBuffers` re-draws it after the EGL surface. The
replay in the server-recomposite branch is kept as well — it is the same invariant, and
a window whose background clear *does* fire needs it. White frames after the fix: zero.

Dead ends worth skipping: the `draw_rectangle(abs_rect)` in the client-redraw branch
(tinted magenta, never appeared), the screen-level colour clear in `screen::redraw`
(gated on the same server-redraw flag that never fired), and the iOS present path
(it clears to opaque black, not white).

## The race clock runs fast

Reading the in-race HUD (`Time: 00:xx.xxx`) against wall-clock time gave a steady
factor: 1.20 game seconds per real second, held to within a few percent over 20
seconds.

The game imports only four time-related euser entries — `User::TickCount` (ordinal
674), `TTime::HomeTime` (859), `User::After` (645), `CPeriodic::Start` (1381) — and no
`hal.dll`, so it cannot be asking the system what the tick period is. A probe on the
`tick_count` SVC recorded no calls at all. `time_now` runs about 120 times a second,
`User::After` is called once per frame with a fixed 1000 µs, and `eglSwapBuffers` runs
at 30 fps. So the game renders as fast as it can and sleeps a token millisecond per
frame; nothing in that loop paces it.

Capping the guest frame rate settled it. The start-line countdown (a fixed-length
animation) takes 0.875 s of real time at a 25 fps cap and 1.375 s at 15 fps — about 21
rendered frames either way, independent of how long those frames took. The engine
advances a fixed step per rendered frame, and the 1.20 factor measured at 30 fps puts
that step at 40 ms: the game is written for 25 fps, which is roughly what an X7
delivers. Anything faster and its whole world — clock, physics, animations — runs
proportionally fast.

This is not an emulator clock bug, so there is nothing to fix in the kernel. What was
missing was a way to ask for the rate the game wants: the in-game frame-limit picker
offered 15/30/60/unlimited, and 25 is now among them. With it selected the race clock
tracks real time.
