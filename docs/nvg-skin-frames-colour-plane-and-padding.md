# Skin frames drawn by an app: the colour plane and the scanline padding

## Symptom

On the X7 (rm-707, Symbian^3), X-plore's bottom softkey bar was a band of coloured
static. The "Menu"/"Exit" labels and the two toolbar icons sat on top of it and were
readable, but everything behind them was noise. The selected row of the drive list had
the same look: the right shape, filled with garbage.

The noise was structured, stable across runs, and confined to exactly those two
regions — everything X-plore draws itself (text, the drive icons, the list) was fine.

## How it was narrowed down

X-plore's `LibSrc/Symbian/Skins.cpp` gives the regions away. Both are
`AknsDrawUtils::DrawFrame()` calls into `igraph->GetBitmapGc()` — an **off-screen
`CFbsBitGc`**, not a window GC:

- `DrawSelection()` → `KAknsIIDQsnFrList`
- `DrawSoftButtonRectangle()` → `KAknsIIDQgnFrSctrlSkButton`

So the pixels are produced by guest-side BitGDI, blitting the skin's frame graphics
into X-plore's own bitmap, and only then handed to the window server. That rules out
our own drawing code and points at the source bitmaps.

The X7 skin (`z:\resource\skins\101f84b9\series60skin.mif`, 7 MB) is NVG, and this is
the same family as [NVG extended bitmaps a guest blits
itself](./nvg-extended-bitmap-guest-blit.md). A probe in `fbscli::create_bitmap`
logging every extended bitmap showed why only *part* of the frame worked:

```
NVGPROBE create ext bitmap 11x22 dpm=4 rasterizable=true    ← mask, converted
NVGPROBE create ext bitmap 11x22 dpm=7 rasterizable=false   ← colour, left as vectors
```

Every skin graphic arrives as a **pair** — `AknNvgFormatHandler::RenderPreparedIconL()`
creates the colour bitmap and the mask from the *same* NVG data, differing only in
display mode and the `is_mask` flag in the prepended `akn_icon_header`. Our
`is_nvg_bitmap_rasterizable()` accepted only mono display modes, so the mask became
pixels and the colour plane stayed compressed vector bytes. Hence the shapes were
right and the colours were noise. A second bound, `NVG_RASTERIZE_MAX_SIDE = 128`,
additionally excluded the wider parts (350×17 separators, 360×44 title bar, the
640×640 background).

Dropping the mono restriction fixed the bulk of it and left a much smaller artefact: a
**one-pixel column of coloured speckle** at the edge of each frame piece. A probe
pairing create/duplicate by bitmap id showed 112 extended bitmaps created and 112
rasterised — nothing was being missed, so the leftover had to be inside a converted
bitmap. It was: Symbian word-aligns scanlines, so an 11-pixel-wide EColor64K bitmap
has a byte width of 24 — twelve pixels. `rasterize_nvg_bitmap()` wrote pixels
`0..width-1` and left the padding pixel holding its original NVG byte, which BitGDI
happily blitted.

## Root cause

Two separate gaps in the stand-in rasteriser:

1. `is_nvg_bitmap_rasterizable()` only accepted mono (mask) bitmaps up to 128×128. The
   colour plane of every skin graphic, and every part wider than 128 px, kept its
   vector data and was blitted as pixels.
2. `rasterize_nvg_bitmap()` never wrote the word-alignment padding at the end of each
   scanline, leaving vector bytes in the pixels past `width`.

## Fix

`is_nvg_bitmap_rasterizable()` now gates on what our rasteriser can actually do rather
than on a guessed subset of bitmaps:

- whole-byte pixel formats (8/16/24/32 bpp), because the write-back stores one pixel at
  a time and has no sub-byte packing;
- a byte budget on the raster form, because we expand the bitmap in place and therefore
  have to reserve room for the pixels at creation time.

`rasterize_nvg_bitmap()` clears the raster region before writing pixels into it.

This matches the platform contract rather than approximating it. On a device the pixels
come from the licensee's `CFbsRasterizer` plugin, which BitGDI consults for *any*
extended bitmap it has to read; `TBitmapDesc` documents `iSizeInPixels` and `iDispMode`
as "the width and height / display mode to rasterize into", so there is no notion of
some extended bitmaps being readable and others not. The only size limit Symbian itself
imposes is `KMaxPixelSize = KMaxTInt / 4` (an overflow guard), which is why the old
128-px constant had nothing behind it. Overwriting the vector data in place is safe
because `CreateExtendedBitmap()` documents extended bitmaps as immutable — modifying
their data is undefined behaviour — so nothing re-reads it, and a different size means
a different extended bitmap.

## Notes

- The earlier scope-narrowing was justified partly by banding it introduced in
  Calculator's history pane. That banding is real but is just RGB565 quantisation of
  the bitmap's own EColor64K display mode: sampled values step by 8 across the
  gradient. A device rasterises into the same mode, so this is the faithful result —
  it only looked wrong next to the window server's path, which decodes NVG straight
  into an RGBA texture and is *smoother* than hardware.
- The same change fixes the X7 Calculator's C / √ / % / ± keys, which had been left
  open in [NVG extended bitmaps a guest blits
  itself](./nvg-extended-bitmap-guest-blit.md). Their colour plane was the missing
  half.
- Dead end worth skipping: rasterising earlier (queueing freshly created extended
  bitmaps and flushing them at the top of every FBS request, so an owner that never
  duplicates the bitmap still gets pixels) changed nothing. Every extended bitmap in
  this workload is duplicated before it is used, and the duplicate hook already covers
  it.
