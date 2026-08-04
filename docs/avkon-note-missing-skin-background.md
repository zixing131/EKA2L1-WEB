# Avkon note renders without its skin background

## Symptom

On the X7 (rm-707, Symbian^3) dividing by zero in Calculator popped up an error
note that showed only its red octagon icon. No frame, no message text — the note
area was flat black, and boosting the screenshot's contrast 8× revealed nothing
hidden in it. The note did take up layout space: the key rows below it shifted
down while it was on screen. An S60v3 device showed its notes correctly.

## How it was narrowed down

Everything obvious checked out, which is what made this interesting:

- **Fonts and popups work.** The Options menu on the same screen renders its
  items fine.
- **The text is submitted.** Logging `do_command_draw_text` gives
  `win=18 'Unable to divide by zero' box=(19,44)-(271,26) pen=0x00000000` — the
  string resolves, the font binds, the command reaches the window.
- **Geometry is right.** The note is window 18, 360×134 at (0,253), visible.
- **The icon draws.** Its `gdi_blt_masked` resolves both handles (51×68).
- **The transparent background is deliberate.** The note sets its window
  background to `0x00FFFFFF` with the alpha channel enabled, and the fill runs
  with `w=0` — exactly what a control does when it expects a skin frame to cover
  the area.

The complete GC command stream for the note window was:

```
set_clipping_region, cancel_clipping_region, reset ×7, use_font,
set_brush_color, draw_box_text_optimised1, discard_font, set_opaque,
reset ×2, gdi_blt_masked, deactive
```

Text, then icon, and **no frame at all** — while Calculator's own window paints
skin frames as the usual nine blits (7×7 corners, 71×7 and 7×41 edges, 73×43
centre). So 9-patch frames work in general; this one was being skipped whole.

Reading Avkon rather than guessing explains why. `CAknNoteControl::Draw` calls
`AknsDrawUtils::Background` with a frame background context, which ends in
`CheckAndDrawFrame`:

```cpp
if( data->NumberOfImages() != EAknsFrameElementsN )   // 9
    return EFalse;
```

One wrong count and the entire frame is silently dropped — no blit, no fallback.
Dumping the parsed skin tables showed `QsnFrPopup` holding **18** images: the
nine frame elements listed twice, in order.

The raw chunk bytes said the file itself is fine — `count = 9`, nine 8-byte
entries. The same chunk was simply parsed *twice*, and the parser did:

```cpp
skn_image_table &tab_ref_ = img_tabs_.emplace(tab_.id_hash, std::move(tab_)).first->second;
for (i = 0; i < count; i++) tab_ref_.images.push_back(img_hash);
```

## Root cause

`std::map::emplace` does not overwrite: on the second definition it keeps the
existing entry and returns it, so the nine new entries were appended to the nine
already there. A skin legitimately redefines an item (a later definition
overrides an earlier one), so this is normal input, not a corrupt file.

S60v3 skins evidently do not redefine the popup frame, which is why only
Symbian^3 was affected.

## Fix

Assign instead of emplacing, in all four item parsers
(`src/emu/services/src/ui/skin/skn.cpp`): image tables and colour tables were
accumulating duplicates, while bitmaps and animations were silently keeping the
*first* definition and ignoring the override.

Fixing the colour tables turned out to matter too — with them correct, the note
text comes out white on the dark frame instead of the black it was resolving to
before.

## Notes

- The 2-pixel white dash above each disabled Calculator key is unrelated; see
  [NVG extended bitmaps a guest blits itself](./nvg-extended-bitmap-guest-blit.md).
- Worth remembering: Avkon drops a whole frame on an element-count mismatch
  without logging anything. A skin item that is subtly wrong shows up as "nothing
  is drawn", not as a broken image.
