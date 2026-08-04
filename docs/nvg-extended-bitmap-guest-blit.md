# NVG extended bitmaps a guest blits itself

## Symptom

On the X7 (rm-707, Symbian^3) the Calculator's *disabled* keys rendered as noise:
at startup C, √, % and ± were blocks of grey static while every digit and
operator drew fine. Pressing a digit re-enabled C, √ and ± and they turned into
correct white glyphs; % stayed disabled and stayed noisy. The noise pattern was
stable across runs and looked like structured data rather than random memory.

This is separate from the NVG *decoder* bugs in
[NVG path decoding](./nvg-path-decoding-signed-coords-and-close-subpath.md) —
fixing those made the enabled keys correct and left the disabled ones broken.

## How it was narrowed down

The window server renders NVG bitmaps itself (`bitmap_cache.cpp` decodes them
with lunasvg), so the first question was whether the broken keys even go through
that path. Dumping the rendered RGBA of every 44×44 bitmap the cache uploaded
produced exactly 16 glyph masks — the enabled keys — and none of the four
disabled ones. So the noisy textures never reached the NVG decoder at all.

Dumping *every* small bitmap instead, tagged with its UID, found four extra
44×44 8bpp bitmaps carrying a plain (non-extended) UID and holding the noise.
Zeroing freshly allocated bitmap memory did not change their content, proving a
guest had written it. Logging the creator of every bitmap named the culprit:

```
proc=AknIconSrv  size=44x44 dpm=4 uid=0x39B9273E forcesz=896   ← NVG icon masks
proc=Calcsoft    size=44x44 dpm=4 uid=0x00009A2C forcesz=0     ← ×4, the noise
```

Calculator creates its own EGray256 masks and produces the dimmed appearance by
blitting the icon's mask into them.

## Root cause

Symbian stores an NVG icon as an **extended bitmap**: the shared data region holds
an `akn_icon_header` followed by compressed vector commands, and BitGDI defers to
the ROM's `CFbsRasterizer` plugin whenever a client blits one. The emulator has
no such plugin. Guest code that draws an extended bitmap on its own — Avkon
dimming a disabled icon, here — therefore copies the NVG bytes as if they were
pixels.

The window server never noticed because it decodes NVG itself before uploading a
texture; only the guest-side blit path was broken. That is also why the bug looks
state-dependent: an enabled key is drawn straight from the icon (server path),
while a disabled one goes through Calculator's own bitmap (guest path).

## Fix

`fbs_server::rasterize_nvg_bitmap()` renders the vector data into the shared
region in the bitmap's own display mode and clears the extended UID, so both the
guest and our own bitmap cache see ordinary pixels. It runs from
`duplicate_bitmap` — the moment a second client picks the bitmap up, by which
point the creator has finished writing the vector data.

Two details matter:

- The data region is sized for the raster form at creation time
  (`fbscli::create_bitmap`), because the compressed vectors are usually smaller
  than the pixels that replace them.
- A plain bitmap is identified in the shared structure by `BITWISE_BITMAP_UID`
  (0x10000040), *not* by the `NORMAL_BITMAP_UID_REV2` (0x9A2C) that the creation
  IPC speaks in. Clearing the UID to the latter leaves
  `get_suitable_bpp_for_bitmap` treating the bitmap as extended, so it uploads
  8bpp data as if it were RGBA — the whole screen turns to noise. That mistake is
  easy to make and looks nothing like a UID problem.

Scope is deliberately narrow: only mono (mask) bitmaps up to 128×128 are
converted. Widening it to every NVG bitmap gains nothing — the large colour
artwork is drawn by the window server, which decodes it correctly already — and
it costs shared-heap space and introduced banding in Calculator's history pane.

## Not fixed

- Disabled keys keep a 2-pixel white dash above the glyph. It comes from
  Calculator's own dimming pass, not from the rasterised mask, and is a large
  improvement on a full block of noise.
- The empty error note on divide-by-zero looked related but was a separate skin
  parsing bug; see
  [Avkon note renders without its skin background](./avkon-note-missing-skin-background.md).
