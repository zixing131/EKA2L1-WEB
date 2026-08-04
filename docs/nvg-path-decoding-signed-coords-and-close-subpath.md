# NVG path decoding: unsigned coordinates and a swallowed close-subpath

## Symptom

On Symbian^3 firmware (X7 / rm-707) a large share of the built-in application
icons rendered wrong in the iOS app list. Two distinct failure shapes:

- **Truncated icons.** Gallery showed only its top-left corner, Camera lost the
  right half of the lens, Clock/Files/Video services/FM radio were missing a
  chunk. The missing area was simply not painted — nothing was drawn over it.
- **Missing detail.** Calendar rendered "2" where the device shows "12"; Web
  had a blank globe with no continents; Device manager, Themes and Operator
  services were mostly empty shells.

Both only affected `.mif` icons that carry an **NVG** (Nokia Vector Graphics)
payload, i.e. Symbian^3-era firmware. S60v3/v5 devices (5320/rm-409) draw MBM
icons and were unaffected, which is why the regression suite never caught it.

## How it was narrowed down

The iOS icon path is `.mif → convert_mif_icon_to_svg → lunasvg → PNG`, and the
debinarised SVG is cached on disk at
`<data container>/Library/Caches/icons/<device>/debinarized_<UID>.svg`. That
cache is the cheap place to look: if the SVG is already wrong, lunasvg is
innocent.

Dumping every `<path>` of Gallery's SVG with its command set and bounding box
immediately showed the outlier:

```
a 11 5, 0, 0 0, 65514 0
                ^^^^^ x range [0, 65514] on a 96×96 viewBox
```

`65514` is `-22` read as unsigned: the raw S15.16 fixed-point value `0xFFEA0000`
is `-1441792`, and `-1441792 / 65536 = -22`, but interpreted as `uint32` it
becomes `4293787648 / 65536 = 65514`. A path whose bounds explode to 65514 units
is what mangles the rest of the icon.

The second bug did not show up as a bad number but as an *absent* one: the
Calendar SVG had a path for the "2" glyph and no path at all for the "1". Adding
a temporary dump of the `nvg_convert_error_description` vector (which
`convert_mif_icon_to_svg` collects and then throws away) proved the decoder
reported **no** errors — so nothing was failing, something was being skipped
deliberately. Re-reading the segment loop found the `break` on `VG_CLOSE_PATH`.

## Root cause

Both live in `nvg_generate_direction` in `src/emu/loader/src/nvg.cpp`:

1. The template was instantiated with `std::uint32_t` / `std::uint16_t`. OpenVG
   path coordinates are **signed** fixed-point (S15.16 for the 32-bit datatype,
   S11.4 for the 16-bit one). Every negative coordinate — any relative segment
   moving left or up — was turned into a huge positive one.

2. A `VG_CLOSE_PATH` segment `break`s out of the segment loop, dropping every
   segment after it. A single VGPath routinely holds several closed contours
   (glyph outlines, holes, multi-part shapes), so everything past the first
   contour was silently discarded. The function appended a single `Z` at the
   end, which made the truncated output still look like a valid path.

The two bugs interacted: an icon could lose its right half to bug 1 *and* its
inner detail to bug 2, which made the symptoms look app-specific rather than
like one broken decoder.

## Fix

- Decode with `std::int32_t` / `std::int16_t`.
- On `VG_CLOSE_PATH`, emit `Z` and continue instead of breaking.

## Notes / not fixed here

- `NVG_PATH_EIGHT_BIT_DECODING` still falls into the 16-bit branch (reads
  `int16_t`, 2-byte aligned, scale 0.5). That is almost certainly wrong too, but
  no icon in the ROMs on hand uses it, so it was left alone rather than changed
  blind.
- The arc rotation argument (`values[2]`) is emitted raw, without the
  fixed-point scale applied. Every arc encountered so far has a rotation of 0,
  so this is invisible today.
- The error vector filled in by the NVG/SVGB decoders is discarded by all three
  front-ends (iOS, Qt, Android). Wiring it to a log line would have made this
  investigation shorter — but in this case it would also have stayed silent,
  since neither bug records an error.
