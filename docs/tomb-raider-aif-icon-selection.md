# Tomb Raider's invisible launcher icon: N-Gage ROM evidence

Tomb Raider appears without an icon in the host launcher on N-Gage. Inspection
of the installed AIF and the official nem-4 ROM identifies a selection mismatch:
the launcher takes the first bitmap pair, while the official menu requests the
42×29 list icon. The first pair in this AIF is a fully transparent 44×44 icon.

The initial investigation was performed on 2026-09-11 against fork revision
`e4934291d`. It established the file contents and official selection behavior by
static analysis. The subsequent implementation and validation are recorded below.

## What is in the AIF

The inspected `TOMBRAIDER.AIF` is 2,666 bytes and contains two bitmap/mask pairs:

| Pair index | Size | Image | Mask result |
|---|---|---|---|
| 0 | 44×44 | Uniform 12-bit color, value `0xD0D` | All 1,936 pixels transparent |
| 1 | 42×29 | Nonuniform image data | All 1,218 pixels opaque |

The first image starts at file offset `0x14`, with its mask at `0x144`.
The second starts at `0x248`, with its mask at `0x930`. Both images use 12-bit
RLE compression and both masks are 1-bit; the first mask uses byte RLE and the
second is uncompressed. The image and mask records are present, so this is not
a missing icon file.

The host bitmap-icon paths in `src/emu/ios/Bridge/IosEmulator.mm`,
`src/emu/android/app/src/main/cpp/src/launcher.cpp`, and
`src/emu/qt/src/applistwidget.cpp` all call `get_icon(..., 0)`. The implementation
in `src/emu/services/src/applist/applist.cpp` indexes the stored pairs directly.
It does not interpret zero as a request for the platform's list-icon size.
Consequently, this AIF supplies a valid but invisible bitmap to the launcher.
The same selection pattern exists on all three hosts; runtime reproduction on
Android and Qt was not part of this investigation.

## List icons and context icons

These are two uses of an application icon in the S60 UI:

- A **list icon** represents an application in the application menu, including
  a grid of applications. The legacy size observed here is 42×29.
- A **context icon** represents the current application or function in the
  status pane's context area, typically beside the title. It is not a context
  menu or right-click icon. The legacy size observed here is 44×44.

The SDK's `TAknsAppIconType` distinguishes `EAknsAppIconTypeList = 0` from
`EAknsAppIconTypeContext = 1`. Tomb Raider's two sizes correspond to these
roles: its list-sized icon is visible and its context-sized icon is transparent.
This correspondence does not establish why the game's authors left the latter
transparent. Pair order alone is not a reliable way to choose an icon role.

## The official N-Gage menu requests a size

The inspected nem-4 `SYM.ROM` has SHA-256
`afc1cc8b693f2b68d23ca203aa4d4eb4266baad4ab718f013a33b915d39574ef`.

In its `MenuEng.dll`, the size comes directly from Thumb immediates:

```asm
50a1e020: movs r0, #0x2a
50a1e022: movs r1, #0x1d
50a1e024: str  r0, [sp, #4]
50a1e026: str  r1, [r2, #4]
```

The call at `0x50A1E032` goes through the import stub at `0x50A21818`.
Its literal at `0x50A21824` points to ApGrfx export slot `0x505FEF48`, ordinal
144, which resolves to the Thumb function at `0x505F8E90`:
`RApaLsSession::GetAppIcon(TUid, TSize, CApaMaskedBitmap&)`.
That client packages the size and sends request `0x1B`.

Thus **42 and 29 are compiled into this menu DLL**, not read from a settings
file, resource, or layout service on this path.

The ROM's `CApaAppData::Icon(TSize)` at `0x505F7EF8` calls the selection routine
at `0x505F7390`. Its behavior is:

1. Search for an exact width-and-height match. The loop retains the last exact
   match if multiple entries have the same size.
2. If there is no exact match, calculate requested area minus candidate area.
   Choose the smallest nonnegative difference. This compares area, not separate
   width and height limits; equal differences retain the earlier candidate.
3. Return null if no candidate qualifies.

The exact-match loop is at `0x505F73AE–0x505F73DA`; the area calculation and
nonnegative-difference checks are at `0x505F7414–0x505F7428`. There is no pixel
or mask-transparency scan in this selection routine. The corresponding later
Symbian OSS implementation, `CApaAppIconArray::IconBySize` in
`appfw/apparchitecture/aplist/aplappinforeader.cpp`, corroborates this algorithm.
The ROM, rather than the later source alone, establishes the N-Gage behavior.

For Tomb Raider, the requested 42×29 size exactly matches pair 1. The official
menu does not need to inspect or reject the transparent pair 0.

## Comparison with other installed ROMs

The comparison covers the locally available ROM images, not every firmware
release for these product codes. Addresses below identify the inspected builds.

| ROM | Examined path | Finding |
|---|---|---|
| nem-4 | `MenuEng.dll`, call at `0x50A1E032` | Direct request for 42×29 |
| rm-36 | `MenuEng.dll`, call at `0x50848084` | Width and height obtained through a CDL layout interface; final numeric result not established |
| rm-84 | `AknsUtils::CreateAppIconLC`, `0x50690800` | List 42×29; context 44×44 |
| rm-86 | Same API, `0xF90AEB94` | List 42×29; context 44×44 |
| rm-320 | Same API, `0x834DE628` | List 42×29; context 44×44 |
| rm-321 | Same API, `0x83167558` | List 42×29; context 44×44 |
| rm-409 | Same API, `0x826D2F7E` | List 42×29; context 44×44 |
| rm-507 | Same API, E32 link address `0x8B6A` | List 42×29; context 44×44 |
| rm-707 | Same API, E32 link address `0x92F8` | List 42×29; context 44×44 |

On rm-36, the menu calls the CDL interface with UID `0x101FE1DD` and API index
`0x1A1`, then reads signed 16-bit width and height fields at offsets 10 and 12
of the returned structure. It tries the skin-aware icon API before falling back
to ApGrfx with those same dimensions. This is evidence of a layout-driven
request, not evidence that its resulting dimensions differ from 42×29.

On rm-84, the menu calls the skin API with icon type zero at `0x5086B5BE`.
Inside the skin library, instructions at `0x50690832` and `0x50690834` initialize
42×29; the type-one branch at `0x50690850` changes both dimensions to 44.
For the EABI libraries, the Belle SDK's `aknskins.dso` identifies
`CreateAppIconLC` as ordinal 92. That export resolves to the functions listed
above. The rm-507 and rm-707 libraries were decoded from their byte-pair-compressed
E32 files before disassembly; their addresses are link addresses, not observed
runtime load addresses.

These later skin-library results establish retained list/context size choices.
They do not prove that every menu passes through that API, that a skin always
uses the application's original bitmap, or that the final on-screen icon is
rendered at its legacy dimensions. Scaling and other presentation choices are
separate from selecting a source icon.

## Implementation and validation

The initial suggestion to skip fully transparent icons was a heuristic, not an
official N-Gage rule. The ROM evidence supersedes that suggestion: a host
launcher representing the N-Gage application menu should request the list-icon
size and use the documented selection behavior, rather than always taking pair
zero or examining transparency to infer the intended role.

This supports a general size-selection fix without a Tomb Raider-specific
exception. It does not justify forcing 42×29 as the rendered size or universal
request for every host, Symbian generation, and UI context. The implemented behavior is scoped to AIF source selection.


Fork commit `11232e10c` adds `get_icon_by_size` and `get_list_icon`, used by the
Android, iOS and Qt bitmap-icon paths. AIFs request 42×29 using the official
exact-size/area rule. If no candidate fits, the host launcher retains its first
pair fallback; non-AIF sources also retain their previous selection. This
fallback is host compatibility policy, not an additional rule established by
the N-Gage ROM. MIF/MBM decoding and guest IPC behavior are unchanged.

Review removed an arbitrary 127-pair limit, widened the pair accessor to
`size_t`, rejected incomplete trailing pairs, and changed area arithmetic to
`int64_t`. Long source comments and the dependency on this fork-only document
were removed from the code prepared for upstream.

Validation on that final source revision:

- Release iOS simulator build succeeded; the default regression suite passed
  all 12 checks, covering Final Battle, Calculator, N95 Calculator and strings.
- Regression screenshots were visually reviewed. The nem-4 host launcher showed
  Tomb Raider's visible face icon alongside Sky Force, Ashen and Call of Duty.
- The inspected logs contained no guest panic, access violation or emulation halt.
- macOS Qt built successfully. `ekatests` completed from its build directory,
  passing 5,884 assertions across 230 test cases.
- Android was not built locally; platform build coverage is deferred to upstream CI.
