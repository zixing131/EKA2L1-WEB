# A binary 8bpp AIF mask rendered the icon inside out, so the app list showed nothing

## Symptom

Nokia 6680 (rm-36) app list. *Karapuzzz Tetris 3D* has no icon at all — an empty cell where every
other installed title draws one. The app itself launches fine, and its caption is correct.

## Narrowing down

The icon comes from `tetris3d.aif`. Its UID1 is `0x101FB032`, which
`get_aif_version_from_uids()` classifies as AIF v2, so the icons are an MBM embedded in the file
rather than a direct file store. Probing `read_icon_data_aif()` showed that path working: the
embedded MBM starts at offset 0x40, holds four bitmaps, and all four `create_bitmap()` calls
succeed.

```
[0] 44x44 bpp=8 color=1 comp=1   <- icon
[1] 44x44 bpp=8 color=1 comp=1   <- mask
[2] 42x29 bpp=8 color=1 comp=1
[3] 42x29 bpp=8 color=1 comp=1
```

Decoding the two bitmaps by hand (byte RLE, then the EColor256 palette) makes it obvious. The mask
holds exactly two levels, 0 and 255, with the **cube shape in black and the backdrop in white** —
the opposite of what the icon decoder assumes.

`mask_is_soft()` classified it by colour depth, and 8bpp landed in the soft/alpha family, whose
rule is "luminance is the alpha, white is opaque". Applied to this mask that makes the shape fully
transparent and the backdrop fully opaque: an inside-out icon that reads as blank.

## The contract

BITGDI decides between the two mask families itself, in
`graphicsdeviceinterface/bitgdi/sbit/BITBLT.CPP`:

```cpp
if (aMaskBitmap->DisplayMode() == EGray256)
    DoBitBltAlpha(..., EFalse);        // alpha blend, aInvertMask ignored
...
const TDrawMode drawMode = aInvertMask ? EDrawModeAND : EDrawModeANDNOT;
```

So only an **EGray256** mask is a real alpha mask. Every other display mode is a binary stencil
whose polarity is the caller's `aInvertMask`, and Avkon passes `ETrue` almost everywhere
(`EIKCLBD.CPP`, `EIKMENUB.CPP`, `Aknscind.cpp`, …), which ANDs the mask in as-is — white keeps the
destination, so **white is transparent**. Tetris 3D's mask is EColor256, so it is a stencil.

## Why not just key off the display mode

Because a previous fix in this port went the other way for a reason: some S60v2 icons carry
gray-valued opacity in a mask whose header flags it as colour, and treating those as stencils
inverts them. Surveying every AIF mask in the 6680 ROM settles which cases actually exist:

| mask | count | border | correct polarity |
| --- | --- | --- | --- |
| 8bpp gray256, 9-118 levels | 55 | all black | soft |
| 8bpp gray256, 2 levels | 1 | all black | soft |
| 8bpp **color256, 2 levels** | 2 | **all white** | colour-key |
| 1bpp gray2 (all installed v1 AIF apps) | — | — | colour-key (already) |

The two colour-key ones (`appinst.aif`, `photoring.aif`) have exactly Tetris 3D's shape: binary,
white border. So the depth rule stays as the baseline, with one content-based override on top.

## Fix

`epoc::apply_icon_mask_alpha()` in `services/fbs/impls/bitmap.cpp`. A mask that the depth rule calls
soft is re-read as colour-key when its content can only be a stencil: it holds white plus at most
one other level, its border is solid white, and some interior pixel is not. An icon's outer frame
is transparent by definition, so that combination is inside out under the soft reading and cannot
be anything else.

Masks with a real alpha ramp, with an already-transparent black border, or fully opaque ones (a
full-bleed icon, which must stay opaque) are untouched, so nothing that renders correctly today
changes.

The helper also absorbs the mask compositing that previously lived in the iOS bridge: deciding how
to read a Symbian mask belongs next to the bitmap conversion that produced it, not in a UI layer,
and Qt and Android — which each roll their own and have no soft-mask support at all — can adopt it.

## Dead end worth skipping

The icon's colours looked wrong at first (yellow and magenta where the game's own in-game logo is
red and green), which suggested an R/B swap: the `color256` branch of `convert_to_rgba8888()` writes
the palette entry's bytes low-to-high, while every neighbouring branch writes R, G, B.

It is not a bug. `TColor256Util::Color256()` builds its `TRgb` with the deprecated
`TRgb(TUint32 aValue)` constructor, documented in `gdi.inl` as taking **`0xaabbggrr`** — red in the
low byte. The palette table is stored in that order, so writing low-to-high *is* R, G, B. Other
color256 icons on the same device (dragonworld, Asphalt 2) confirm it visually. The AIF artwork
simply uses a different palette from the in-game logo.
