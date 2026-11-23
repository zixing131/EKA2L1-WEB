# The dialogue boxes a fresh bitmap's fill colour made disappear

## Symptom

Metal Bluster 2 (`mb2`, UID `0x10206BB9`) on the N-Gage ROM (`nem-4`). The title screen,
mission select and briefing all render correctly. Once a mission starts, the cutscene
dialogue panels — two 176x38 bands at the top and bottom of the screen — come up as flat
white rectangles. The dialogue text is drawn, but in a pale cyan that is almost invisible
against the white, so the panel reads as "empty". On real hardware those bands are
semi-transparent blue panels with a character portrait and white text.

## Narrowing it down

The window server was a dead end, and usefully so. A probe in
`canvas_base::add_draw_command` showed the game issuing exactly one GDI command per
frame: a blit of a single 176x208 bitmap covering the whole screen, no mask. Everything on
screen, dialogue included, therefore comes out of one guest-owned `CFbsBitmap`; nothing
about the panel is decided on the host side of `ws32`.

Dumping that frame bitmap (12 bpp, `EColor4K`, 352-byte stride) showed the two bands filled
with `0xFFFF`, while the rest of the frame held ordinary 12-bit values with a zero top
nibble. `0xFFFF` is not a colour the game writes — it is a byte-wise `0xFF` fill, and the
only thing in the emulator that does that is `do_white_fill()`, called from
`bitwise_bitmap::construct()` when the client creates a bitmap.

Logging `fbs_bitmap_create` confirmed the game creates a 176x38 `EColor4K` bitmap twice per
frame during the cutscene (once per panel). Dumping those bitmaps at `free_bitmap()` time
showed they contain exactly two values: the `0xFFFF` fill and `0x0EFF`, the text. So the
game draws *only* the text into the panel bitmap and expects the fill to be handled for it.

Two plausible-looking explanations were ruled out before the answer showed up:

- `CWsScreenDevice::CopyScreenToBitmap` and `GetScanLine` are both stubbed in
  `scrdvc.cpp`, and both stubs log at `LOG_TRACE`, so they are invisible under the default
  `*:info` filter. A "semi-transparent panel" is exactly what a screen read-back would be
  for. Promoting the stub to `LOG_INFO` showed the game never calls either.
- Zero-filling new bitmaps instead of white-filling them (on the theory that hardware
  hands fbserv kernel-zeroed pages) makes the panel black with readable text. It looks
  like progress and is wrong: the portrait and the see-through background are still
  missing, because the fill value is not arbitrary.

What settled it was the guest's own call site. A probe walked the client thread's stack at
`fbs_bitmap_create` time, matching each word against the loaded code segments, which gave
a return address inside `mb2.app`. Both `mb2.app` and its helper `xutil.dll` are
uncompressed E32Images, so the import table maps thunks to `DLL#ordinal` directly and
capstone disassembles the Thumb code from the file.

The call site builds `TSize(176, 38)`, mode `10` (`EColor4K`), calls `XUTIL#43` (a thin
`new CFbsBitmap` + `Create()` wrapper that neither checks the result nor clears the
bitmap), wraps it in an object via `XUTIL#126`, appends the dialogue string, and finally
blits it to the frame at y=12 and y=158 — the two band positions. `XUTIL#126` stores its
third argument, a literal `0x0FFF`, at offset `0xC` of that object, and the blit
(`XUTIL#89`) reads it back as a **colour key**:

```
ldrh r1, [r0]        ; source pixel
cmp  r1, r4          ; r4 = key from object+0xC = 0x0FFF
beq  skip            ; transparent
strh r1, [r0]        ; else copy
```

## Root cause

`do_white_fill()` filled every byte with `0xFF` regardless of display mode. `EColor4K`
keeps 12 significant bits in a 16-bit pixel, so that yields `0xFFFF`, not white (`0x0FFF`).
The game colour-keys against white, `0xFFFF != 0x0FFF`, so every background pixel of the
panel was copied opaquely instead of being skipped — burying the panel art and portrait
the game had already drawn into the frame underneath.

The function had always taken a `display_mode` parameter and always ignored it. Its one
call site also passed `settings_.current_display_mode()`, which older FBS legacy levels
(EPOC6 among them) never store, so it reads back as `none` — the mode would not have been
usable even if the fill had consulted it.

Every other display mode's all-ones pattern really is white: `EGray*` and `EColor16`/
`EColor256` (palette index 255 is white in both of EKA2L1's 256-colour tables), and the
16/24/32-bit colour modes. `EColor4K` is the only one that needs a different pattern.

## Fix

`do_white_fill()` now writes `0x0FFF` per pixel for `EColor4K` and keeps the byte-wise
`0xFF` fill for everything else, and `construct()` passes the `disp_mode` argument it was
given rather than the not-always-stored current mode.
