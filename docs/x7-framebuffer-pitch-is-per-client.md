# One framebuffer, two row pitches

With the IPC deadlock out of the way, Alien Pinball ran at 60 FPS on the X7 but
every frame was sheared into diagonal bands. Sky Force Reloaded, on the same
phone, had shown exactly that symptom before and was fixed by giving 32-bit
ScreenPlay framebuffers a 64-byte-aligned row pitch. Alien Pinball wanted the
opposite.

## Measuring the shear instead of guessing

Correlating a screenshot row against rows further down gives the shift per row
directly: about 8 display pixels per display row, and the display-to-guest scale
cancels out, so 8 guest pixels per guest row on a 360-wide screen. At 32 bits per
pixel that is 32 bytes, which is exactly `align64(360 * 4) - 360 * 4` — the
padding the ScreenPlay pitch adds. The sign said the reader was advancing faster
than the writer, so the emulator was reading padded rows out of a tightly packed
frame.

Building with the padded branch forced off made Alien Pinball render perfectly
and put Sky Force back into bands, so the two games really did write the same
buffer with different row layouts.

## Neither the call site nor the DSA count separates them

Both games post their frames from their own process, through the same
`update_screen` dispatch, with a direct screen access session open. The previous
heuristic — use the padded pitch whenever a 32-bit DSA is active on a ScreenPlay
screen — therefore had to be wrong for one of them, and a `RDirectScreenAccess`
client normally draws with `CFbsBitGc` over a `CFbsScreenDevice`, which is the
common case it got wrong.

Logging every HAL call per process showed what actually differs:

* Sky Force reads the display HAL for the framebuffer (`TScreenInfoV01` every
  frame, `TVideoInfoV01` at startup) and writes the panel itself.
* Alien Pinball never touches the display HAL at all. Every pixel it posts comes
  out of `CFbsScreenDevice`, which on EKA2L1 is the `scdv` patch DLL.

And `scdv` was the odd one out: `InstantiateNewScreenDevice` built every screen
draw device with a data stride of `-1`, so the 32-bit device set
`iScanLineWords = iSize.iWidth` — tightly packed, whatever the framebuffer's real
pitch is. Two writers, two layouts, one buffer.

Reporting a tight pitch to everyone is not an option either: with the display HAL
answering 1440, Sky Force still laid its rows out at 1472, so it derives the
aligned pitch itself rather than taking the one it is given.

## Fix: make the layout single-valued

`screen::screen_buffer_byte_width()` is the one definition of the layout, and a
new dispatch (id 6, `GetScreenFramebufferRowBytes`) hands it to the guest. The
`scdv` screen draw device asks for it and passes it as its data stride instead of
`-1`, for the 32-bit device family — the only depth that is ever padded, and the
narrower devices reject a stride that is not a multiple of their pixel group
anyway. Every writer of the buffer now agrees with the texture upload and with
the window server's write-back, so the upload no longer consults the DSA count,
the screen architecture or the buffer's contents.

Two details came with it:

* The pitch is now derived from the *current* mode width rather than the panel's
  native one. EKA2L1's screen buffer is laid out for the mode being displayed —
  `sync_screen_buffer_data` writes `current_mode()` rows — so a rotated mode
  needs its own row length. This is the root of the 5320 landscape regression
  that the previous fix had to special-case around.
* The upload only takes the reported pitch over the tightly packed one at 32
  bits per pixel; the narrower framebuffers keep the layout the upload already
  assumed.

`scdv_general.dll` and `scdv_v81a.dll` are rebuilt from these sources. A guest
does not have to cooperate for this to hold: a client that writes the panel
itself gets the same pitch through the display HAL, which is where Symbian says
it comes from.

Verified on the X7 with Alien Pinball (menu and table) and Sky Force Reloaded
(language screen), on the 5320 with Brothers in Arms for the rotated
pre-ScreenPlay path, and with the regression suite (Final Battle, Calculator, N95
Calculator, Angry Birds).
