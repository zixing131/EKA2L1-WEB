# The N-Gage Radio icon, and why mask polarity is the display mode after all

## Symptom

Nokia N-Gage (`nem-4`) app list. Every icon is stretched vertically, and *Radio* renders as a
magenta block with a pale smear in the middle instead of the cyan radio set.

Two unrelated defects.

## 1. Stretched icons

`encode_rgba_to_png()` in the iOS bridge scaled every decoded icon into a square
`side x side` canvas with a plain `CGContextDrawImage` over the full rect. S60 AIF list icons are
**42x29** — `applist_server::get_list_icon()` picks that size on purpose — so the whole legacy icon
set came out 1.45x too tall. S60v3+ MIF/SVG icons are square, which is why only the old devices
showed it. Fixed by scaling to fit and centring; Qt's `applist_widget` had the same bug in
`drawPixmap(QRect(0, 0, 64, 64), ...)` and got the same fix.

## 2. Radio's mask read inside out

`fmradio.aif` pairs a 42x29 EColor256 icon (magenta colour-key backdrop) with a 42x29 **EColor256**
mask holding three palette indices: `0xFF` white, `0xE1` and `0x00`. On the pre-9.5 palette `0xE1`
is `0x111111`.

The icon decoder classified masks by colour depth: 2-8bpp was "soft", meaning luminance is the
alpha and white is opaque. Under that reading the artwork (luminance 17) became 93% transparent
while the white backdrop stayed fully opaque — the magenta colour key survived and the radio
disappeared.

A content check sat on top of the depth rule for exactly this failure mode, but it only fired for
masks with white plus *one* other level; this one has two, so it did not.

## The contract

BITGDI picks between the two mask families on the display mode alone, in
`graphicsdeviceinterface/bitgdi/sbit/BITBLT.CPP`:

```cpp
if (aMaskBitmap->DisplayMode() == EGray256)
    DoBitBltAlpha(..., EFalse);        // alpha blend, aInvertMask ignored
...
const TDrawMode drawMode = aInvertMask ? EDrawModeAND : EDrawModeANDNOT;
```

Only **EGray256** is an alpha mask. Everything else is a binary stencil whose polarity is the
caller's `aInvertMask`, and Avkon passes `ETrue` almost everywhere (`EIKCLBD.CPP`, `EIKMENUB.CPP`,
`Aknscind.cpp`), which ANDs the mask in as-is: white keeps the destination, so **white is
transparent**. The same file states the implementation never inspects mask pixels, and that a
stencil whose pixels are neither black nor white blits to "unpredictable discoloration" — so a
content-based classifier is modelling something BITGDI does not do.

## Why the depth rule existed, and why it can go

`fedc6bc` had moved off the display mode, on the grounds that S60v2 ROMs store gray-valued opacity
in masks whose header flags them as colour. Two independent pieces of evidence say that was a
misattribution:

- The full survey of 6680 AIF masks recorded in
  [S60v2 binary colour-key icon mask](./s60v2-binary-colour-key-icon-mask.md) lists the 56
  multi-level (soft) masks as **gray256** with an all-black border, and only the 2 binary stencils
  as color256. No color256 mask on that ROM carries gray opacity.
- Instrumenting `apply_icon_mask_alpha()` and walking the whole app list on all five ROMs available
  here (nem-4, rm-84, rm-409, rm-320, rm-707) logged 55 masks. Every mask classified as soft is
  EGray256; no EColor256 mask is ever soft. Classifying by display mode alone produces an identical
  result to the depth rule plus its content override on every one of them.

What `fedc6bc` actually changed alongside the classifier is the soft path itself: `118dcfa8` wrote
`mask_rgba[i * 4 + 3]` — the *binary* "is this pixel pure white" flag `make_standard_mask` leaves in
the alpha channel — where the luminance belongs. An anti-aliased EGray256 mask has few exactly-white
pixels, so that reading erased most of the foreground. That is the defect the commit fixed; the
depth rule rode along with it.

Two more data points on the S60v1 side, which the depth rule got wrong:

- `nem-4` ships no `aknicon.dll`. Series 60 1st edition has no icon framework of its own, so app
  icons go straight through `BitBltMasked` — the contract above is the whole story there.
- All 86 AIF masks in that ROM frame the artwork in white, whether stored as EGray2 (84),
  EColor256 (fmradio) or EColor4K (realplayer). The polarity is uniform across display modes; only
  the depth rule made fmradio special.

## Fix

`epoc::apply_icon_mask_alpha()` now takes the mask's display mode and applies BITGDI's rule
directly: EGray256 is alpha, everything else is a stencil with white transparent. The depth
classifier and the content override are gone.

## The 6680 regression, and where the display mode really lives

Installing the 6680 (rm-36) ROM afterwards showed every app-list icon as an opaque
block with the artwork punched out — the inverse of a correct icon. Instrumenting
`apply_icon_mask_alpha()` on that ROM logged its masks as **EColor256**, which would
mean the contract above is wrong and `fedc6bc` was right after all.

Parsing the ROM's AIF files directly says otherwise. Of the 35 masks in the 19 AIFs
that embed their icons:

| mask mode | border | levels | count | polarity |
| --- | --- | --- | --- | --- |
| EGray256 | black | multi | 33 | alpha |
| EColor256 | white | binary | 2 (`appinst.aif`) | stencil |

Zero exceptions to BITGDI's rule. The masks logged as EColor256 are the same
EGray256 masks — the display mode was being *derived* rather than read:

- `read_icon_data_aif()` rebuilds an AIF v2 icon through `fbs_server::create_bitmap()`.
- `create_bitmap()` fills `sbm_header::color` from `get_bitmap_color_type_from_display_mode()`,
  which maps only EGray2 to "monochrome" and calls every other gray mode a colour bitmap.
- Deriving the mode back from that header (bpp 8, colour 1) yields EColor256.

Symbian never derives it that way. `CFbsBitmap::DisplayMode()` returns
`CBitwiseBitmap::DisplayMode()`, which reports what the bitmap was built with; the
static `CBitwiseBitmap::DisplayMode(TInt aBpp, TInt aColor)` is used only where a
bitmap is read in from a stream (`BMPASTR.CPP`, `FBSBMP.CPP`). So the applist icon
path now asks the bitmap: `bitwise_bitmap::current_display_mode()`, falling back to the
initial mode for EKA1 ROM bitmaps, whose current byte reads ENone (the N-Gage AIF
icons are all of that kind).

Two supporting observations from the same session, both first-hand rather than
inferred:

- Instrumenting the window server's `gdi_blt_masked` while a 6680 app runs shows Avkon
  blitting a 176x15 EGray256 mask and a 44x44 AIF mask, both with `aInvertMask=ETrue` —
  exactly the two families the contract describes, with the caller polarity it predicts.
- `nem-4` ships no `aknicon.dll`; the S60v2 ROMs do. It turned out not to matter — the
  display mode alone settles every mask on both — but it is why the S60v1 side was
  suspected of needing a rule of its own.

## The header field that started it

The mode was derivable-but-wrong because of a second defect, fixed alongside:
`get_bitmap_color_type_from_display_mode()` mapped only EGray2 to "monochrome" and
called every other gray mode a colour bitmap, so any bitmap this emulator created in
EGray4/EGray16/EGray256 serialised with the colour flag set.

`bmconv` writes the field the other way round
(`graphicstools/gdi_tools/bmconv/PBMCOMP.CPP`):

```cpp
case 8:
    if (color == EMonochromeBitmap)
        bmp->iDispMode=4;      // EGray256
    else
        bmp->iDispMode=6;      // EColor256
```

4bpp splits into EGray16 / EColor16 the same way, and `SEpocBitmapHeader::TColor` names
0 `ENoColor`. That is the table `get_display_mode_from_bpp()` already reads, so the two
must agree for grayscale and colour modes; a round-trip test covers those supported
by the existing depth/boolean-colour decoder. The decoder does not distinguish the
three 32-bit modes, whose header colour values are checked separately. Only bitmaps
the emulator creates were affected — one loaded from an MBM keeps the file's header.

## Ordinary grayscale pixels remain opaque

Correcting the header also makes emulator-created EGray256 main images reach the
RGBA decoder's grayscale branch. That branch wrote the sample into all four
channels, even when `make_standard_mask` was false. A black main-image pixel thus
became transparent, and intermediate grays became translucent.

The official graphics source establishes the distinction independently of ROM
icon appearance. At SymbianSource `oss.FCL.sf.os.graphics` revision
`ff133bc50e6158bfb08cc093b0f0055321dcde99`:

- [`TRgb::Gray256()` in RGB.CPP](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/graphicsdeviceinterface/gdi/sgdi/RGB.CPP#L103)
  constructs `TRgb(sample, sample, sample)`.
- [The three-component constructor in GDI.INL](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/graphicsdeviceinterface/gdi/inc/GDI.INL#L68)
  explicitly ORs in `0xff000000`, making every grayscale colour opaque.
- [`CBitwiseBitmap::IsColor()` in BITBMP.CPP](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/fbs/fontandbitmapserver/sfbs/BITBMP.CPP#L2814)
  returns `ENoColor` for all four grayscale modes, supporting the header correction.
- [BITBLT.CPP](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/graphicsdeviceinterface/bitgdi/sbit/BITBLT.CPP#L842)
  uses EGray256 samples as opacity specifically when the bitmap is the mask.

The decoder now writes alpha 255 for ordinary EGray256 images and retains sample
opacity for mask conversion. The regression test follows the real header/decoder
path with black, intermediate gray, near-white and white samples across two padded
rows, then composites the decoded mask. Before the fix it fails on the first black
ordinary pixel (`alpha 0`, expected `255`).

## Previous verification

Unit tests in `src/tests/epoc/services/fbs/bitmap.cpp` cover both families plus the EColor256
stencil this started from: `ekatests` 270 cases / 27368 assertions pass. On the simulator, nem-4
renders Radio and all 32 other icons correctly, and rm-36 / rm-409 / rm-320 / rm-707 / rm-84 are
correct too. Release regression suite 12/12.

On the 6680 (rm-36), all 39 icons render with their anti-aliased soft masks.

## Review-fix verification (2026-09-19)

The real EGray256 decoder test first failed on ordinary black (`0 != 255` alpha).
After the fix, the complete Release host suite passed 273 cases / 27,477 assertions.
The final Release simulator build passed the default regression suite, 12/12,
with screenshots inspected and no guest panics, access violations or graphics
halts in the checked logs. Fresh app-list spot checks showed nem-4 Radio without
the magenta backdrop and visible rm-36 soft-mask icons without inverted masks.
These checks cover the final source content before squashing; the upstream PR
reuses those results for the same changed hunks.
