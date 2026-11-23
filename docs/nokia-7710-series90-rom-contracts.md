# Nokia 7710: Series 90 contracts between the older window table and newer IPC

The RM-12 V 04.10.0 firmware can supply a complete EKA1 system, but its flash
files are not directly loadable ROM dumps. Once reconstructed, No Man’s Land
2.06d initially crashed the host in `canvas_base::inquire_offset`. The guest had
not called that operation: an incorrectly translated window command treated a
window-group object as a canvas.

The flash image contains a compressed core, a separately compressed language
extension and a plain ROFS. For V10, the extension belongs at ROM offset
`0x1b00000`; its root directory list is `0x51b00080`. The reconstructed ROM
header must include the extension in its size and point to that root. The ROM
and extension supply 2,376 files, and ROFS supplies another 844. The RPK2 package
contains the full 3,220-file union and machine UID `0x101fbe09`. Only the ROM
size and root pointer are changed; no guest executable is patched.

Two installation assumptions hid these contracts. Series 90 has
`system/install/series90v10.sis` or `series90v11.sis`, and must select OS 7.0s
rather than falling through to 9.4. An EKA1 ROM with naming files can still have
additional ROFS content. When the user supplies an RPKG, installation must use
it even if the ROM could identify a device by itself.

## The window session and window object tables are different generations

The ROM’s `ws32.dll` negotiates version 1.0.151. Import-library ordinals from the
SDK identify the exported routines, and their Thumb stubs establish the wire
opcodes. The session uses ClaimSystemPointerCursorList 49, PrepareForSwitchOff
80 and SendEventToOneWindowGroupPerClient 85. Applying the UIQ 2 session shifts
would therefore be wrong.

The same library has only 389 exports; RWindowBase::AbsPosition is ordinal 390
in the later SDK. Its window table consequently has Size 12, InquireOffset 24,
PointerFilter 25, SetName 61, EnableScreenChangeEvents 94 and DisplayMode 97.
These commands need the missing-AbsPosition adjustment even though the session
uses the newer table. EnableGroupListChangeEvents is 105: the additional UIQ
CaptureLongKey gap does not apply. The protocol tests retain both Series 90 and
UIQ anchors, along with later S60 and Belle controls.

## Hardware defaults and patch ABI

7710’s WSINI specifies COLOR64K but omits screen dimensions. Its HAL
`InitialValue` table contains 640x320, but that table is not authoritative for
driver-derived attributes. The supplied 9500 and 9300i HAL tables contain the
same generic dimensions despite their 640x200 panels. In both 7710 and 9500,
`HAL::Get` dispatches attributes 31/32 through a non-null implementation
function, which requests video information through RHalDriver and takes the
returned pixel size. The machine UID attribute has no such function and is
read from the static table.

EKA2L1 supplies that driver response from its configured screen, so asking the
guest HAL cannot discover dimensions independently. Explicit WSINI dimensions
remain authoritative for configuration. In their absence, machine UID
`0x101fbe09` now has a 640x320 hardware default; the existing Series 80 640x200
default is retained. No display dimensions are inferred from HAL initial values.

Patch selection also needs an EKA1 boundary. A missing EKA1 variant must not
fall through to patches built for EKA2: for example, the generic camera and
audio-routing patches import EUSER ordinals beyond this ROM’s 1,986 exports.
Available EKA1 variants remain eligible; otherwise the ROM library is retained.

The compatibility changes belong to version selection, installation, window
protocol translation, hardware defaults and patch selection. They contain no
No Man’s Land UID check or game-specific opcode override.

## The menu animation uses the reference Color256 palette

The menu’s replayable preview initially had cyan skies and pink terrain. The
ROM’s GDI ordinal 164, `TRgb::Color256(TInt)`, calls palette.dll ordinal 3 and
indexes the table it returns at `0x5063a3ec`. All 256 entries match the Symbian
reference palette, whereas the emulator selected the older S60 palette for
OS 7.0s. For example, index 0x24 is 0x330000 in the ROM but 0xFFFFCC in the S60
table; index 0x6c is 0x111111 versus 0xFFFF66.

FBS now reads the table returned by ROM `DynamicPalette::DefaultColor256Util`
(ordinal 3) when it is an ARM/Thumb literal-return getter and
`SetColor256Util` (ordinal 4) immediately returns. The first 256 words of
`TColor256Util` are `iColorTable`; the emulator copies these actual values once
and shares them between GPU upload and software conversion. Both the getter
and every read must stay inside the library code image.

This does not infer a palette from the Series 90 marker or OS version. P800 and
P900 instead embed the table in GDI. Their EKA1 ordinal 164
loads a table literal, masks the index to eight bits, and loads the indexed
word. Recognizing that complete ARM routine permits reading the exact table
and removes the UIQ 2 palette special case as well. Both ROMs return the
reference palette.

Dynamic implementations and other getter forms keep the existing fallback.
Nokia 6680 OS 8.0 has a dynamic palette implementation and retains its S60
palette. The supplied Nokia 9500, 9300i and 9300 dumps all use the same
immutable DynamicPalette contract as 7710; every one of their 256 entries
matches the reference table. The Series 80 palette override is therefore
removed too. The resolved table addresses are:

| ROM | Color256 table | Provider |
|---|---|---|
| Nokia 7710 | `0x5063a3ec` | palette.dll ordinal 3 |
| Nokia 9500 | `0x502daf2c` | palette.dll ordinal 3 |
| Nokia 9300i | `0x502d77ec` | palette.dll ordinal 3 |
| Nokia 9300 | `0x502ca14c` | palette.dll ordinal 3 |
| Sony Ericsson P900 | `0x5018b3ac` | GDI ordinal 164 |
| Sony Ericsson P800 | `0x50182af8` | GDI ordinal 164 |

These addresses are observations, not constants in the implementation. The
remaining version-based fallback is only used for an unsupported provider.
Supporting runtime palette changes remains a separate limitation.

## Saving must complete the older AppArc request

Opening Save as reached AppArc opcode 17 and left the UI waiting indefinitely.
The ROM’s APGRFX ordinal 122 constructs AppForDocument with the filename in
slot 0 and a 272-byte UID/TDataType result descriptor in slot 1. The later
protocol uses filename slot 2 and result slot 0. The 7.0s/8.0 dispatch now routes
this request to the existing document-type handler with the correct slots and
completes it synchronously.

## Host text input uses the EKA1 FEP factory

The save-name editor can focus and draw a caret while the host keyboard remains
unavailable. The ROM loads `system/fep/hwrfep.fep` (UID2 `0x10005e32`, UID3
`0x101f4d25`), whereas the existing host bridge is an EKA2 Avkon ECOM plugin.
Copying that plugin to an EKA1 ROM would not provide the required ABI.

The EKA1 FEP contract exports `NewFepL(CCoeEnv&, const TDesC&,
const CCoeFepParameters&)` at ordinal 1 and the settings dialog at ordinal 2.
An EKA1 bridge implements these factories and the standard
`MCoeFepAwareTextEditor` interface, without Avkon editor-state casts. Its patch
map matches the original FEP identity and OS 7.0s export table. Focus changes
publish input availability; the existing host keyboard button sends F20 to
open the input dialog. An FEP-priority control receives that key through the
EKA1 control stack; relying only on the FEP event callback misses this route.
Text is committed through the editor's inline-edit
transaction. Losing focus cancels the pending host request, and destruction
notifications avoid calling an editor that has already been destroyed.

An immediate host-open failure can complete its request before the active
object is activated. The failure path consumes that completion before leaving,
so it cannot become a stray signal in the next scheduler wait. FEP teardown and
asynchronous errors also cancel the inline transaction before releasing state.

## Dialog decorations use a bitmap brush

The Rename dialog's title appeared white and its border was incomplete even
after the palette was corrected. The ROM sends SetBrushOrigin, UseBrushPattern
and DiscardBrushPattern as GC opcodes 14, 15 and 16. The emulator did not handle
these requests or render a patterned brush. The title gradient and border are
bitmap patterns supplied by the guest.

The graphics context now retains the selected FBS bitmap until discard, reset
or destruction. Rectangle and text-box fills repeat that bitmap with phase
relative to the brush origin. The GC origin moves both the destination and the
brush field, so it must not change their relative phase. Commands snapshot the
bitmap through the existing draw-command path, and texture repeat is restored
to clamp after each patterned fill. This restores the ROM's yellow gradient
without a dialog-specific color or drawing override.

The default save name was also present in the editor but invisible on screen.
The ROM draws the text and then highlights the selection with a white rectangle
in `EDrawModeXOR` (2), restoring `EDrawModePEN` (32) afterward. Ignoring drawing
mode painted a white rectangle over the text. The ROM's SetDrawMode export is
ordinal 203 and sends GC opcode 61.

Solid rectangle fills now support NOTSCREEN and XOR when every source RGB
channel is zero or 255. For those inputs, the blend equation
`S * (1 - D) + D * (1 - S)` is exact bitwise XOR, with destination alpha
preserved. Repeating the draw restores the original pixels. Intermediate
source channel values cannot use this equation for bitwise XOR; other logical
drawing operations retain their existing limitations. The implementation does
not inspect the text, application, or dialog identity.

## Validation

The final source is recorded by fork commit `166979291`, built as
`Release-iphonesimulator`. The default regression passed all 12 checks and
Angry Birds passed all five touch checks. Relevant host tests completed
normally with 927 assertions in 42 cases. The last FEP-only error-path change
reused the passing shared-code regression and was followed by a targeted run
with the final rebuilt EKA1 DLL.

On 7710, the original SIS installs, the menu preview uses the ROM palette, and
Easy Peasy reaches gameplay. Rename displays its gradient, border and default
text both selected and unselected. Native input opens with the existing text,
cancels and reopens, commits a replacement, and disappears when the guest
dialog loses focus. A saved game was listed and loaded back into the battlefield.
The final run had no guest panic, access violation or graphics halt.

The unchanged palette controls were also checked with P900 No Man's Land and
9500 Calculator at 640x200. The complete ROM/RPKG union roundtrips through the
native installer with all 3,220 paths and file contents matching extraction.
The EKA1 FEP was built after `priv` using S60 2nd Edition FP3 (`armi urel`);
its UID/header fields, exports and import bounds match the ROM ABI.
