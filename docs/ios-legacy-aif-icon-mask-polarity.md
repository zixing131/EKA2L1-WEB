# Legacy AIF icon mask polarity

> **Superseded.** The three-family depth split described here is gone. The S60v2 masks this
> doc calls color256 are EGray256 — the colour flag came from the emulator's own bitmap
> rebuild, not from the ROM; see
> [mask polarity is the display mode](./icon-mask-polarity-is-the-display-mode.md).

## Symptom

After mask compositing was added to the iOS app list, Asphalt 2 installed on the
Nokia 6680 (`rm-36`) appeared in the list but its icon cell was blank. The 6680
ROM's system icons were correct, as were the newer 5320 icons that motivated the
original compositing change.

## Narrowing it down

The SIS installs `asphalt2.aif`, so this icon goes through the applist server's
legacy AIF bitmap pair rather than the standalone MBM or MIF decoders. The AIF
contains two icon sizes. Its first pair is a 44x44, 8bpp colour bitmap followed
by a 44x44, **1bpp gray2 mask**. Applying that mask as a soft alpha image made
the white backdrop opaque and the black icon foreground transparent. Against
the white iOS app-list background, the surviving backdrop looked completely
blank.

Two tempting classifications were ruled out:

- The SBM `color` flag is not sufficient. The 6680 ROM uses 8bpp `color256`
  masks that still contain soft grayscale opacity, so treating every coloured
  mask as a colour key breaks the system icons.
- Treating every mask up to 8bpp as soft alpha fixes those ROM masks and the
  5320 Final Battle mask, but incorrectly includes legacy binary masks such as
  Asphalt 2's.

## Fix

Mask polarity is now split into three format families:

- 1bpp gray2: binary legacy mask, white backdrop is transparent, so invert;
- 2-8bpp: multi-level soft opacity, use luminance directly;
- 12bpp and above: colour-key bitmap, invert the existing pure-white test.

This keeps the special case tied to a general Symbian bitmap representation,
not an application UID. It preserves anti-aliased soft masks while restoring
the conventional legacy AIF binary-mask polarity.

## Verification

On `rm-36`, Asphalt 2's 3D car icon renders again and the ROM system icons keep
their colour and transparency. On `rm-409`, Brothers in Arms, Final Battle, and
天地道 remain correct. The Release regression suite passed 11/11 both after
installing the build and in a later no-reinstall run; logs contained no guest
panic, access violation, graphics halt, or temporary icon diagnostics.
