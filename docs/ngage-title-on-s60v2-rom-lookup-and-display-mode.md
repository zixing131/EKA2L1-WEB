# Tomb Raider on the N70: ROM lookup and per-mode display depth

Tomb Raider (an N-Gage title, `0x101FBF9D`) runs on the N-Gage (nem-4, EPOC 6.1)
but on the N70 (rm-84, Symbian 8.1a) it stopped with `KERN-EXEC 3` a second after
the splash. The ROM-address failure was an emulator defect. A subsequent renderer
failure exposed a separate display-mode issue; platform compatibility alone does
not prove that an N-Gage renderer accepts the N70's default display format.

## 1. `RFs::IsFileInRom` returned success without writing the address

The access violation was a read of `0x2C4` from `FbsCli.dll`. Symbolising the
faulting PC against the ROM burn tree (`TRomImageHeader.export_dir_address`,
nearest export below the address) put it in `CFbsBitmap::Load`, in the branch
that fast-paths a multi-bitmap file that lives in ROM:

```
IsFileInRom(aFileName, rompointer);      // CFbsBitmap::IsFileInRom
rompointer += aFileOffset;
if (rompointer[0] != KMultiBitmapRomImageUid) ...   // <- faulted here
```

The EKA1 `RFs::IsFileInRom` client stub hands the server a `TBuf8<4>` — an `EBuf`
descriptor whose four data bytes live *inside* the descriptor — and, when
`SendReceive` returns `KErrNone`, reads those bytes back as the ROM address.
EKA2L1's handler bailed out early for a file it could not open:

```cpp
if (!f) {
    ctx->complete(0);       // KErrNone, and nothing written back
    return;
}
```

so the guest read whatever stack garbage the buffer happened to hold. Here the
game asks about an empty filename (its `RFile`-based `Load` overload passes
`KNullDesC`), the open fails, and the leftover word was `0x2C4` — the file offset
from the same stack frame one call earlier.

Fix: always write the address back (0 when the file is not in ROM), matching
F32, which writes the out-parameter on every `KErrNone` completion.

## 2. The N70 supports 16MU and a separate 64K screen mode

The renderer factory observed during the earlier investigation accepts
`EColor64K` (7) or `EColor4K` (10). Forcing the emulator's reported mode to 64K
allowed the game to progress, but did not establish the N70's actual contract.

Static analysis of the original rm-84 ROM contradicts a universal EKA1 16-bit
limit. The ROM SHA-256 is
`cb062dc84820b728a60b525115049b250e3f4912638a777d49edef79972c4e0e`:

- `scdv.dll` export 10, `CFbsDrawDevice::DisplayMode16M`, at `0x5005DB9C`
  returns 11 (`EColor16MU`).
- Its screen constructors (exports 2 and 12) reach `0x5005DA78`, which accepts
  4K, 64K, and 16MU. The 16MU branch constructs a separate implementation;
  its initializer at `0x5005255C` stores mode 11.
- `bitgdi.dll` forwards its screen constructor to scdv with the requested mode.
  N70 WSERV enumerates supported screen devices at `0x505D81A0`, trapping failed
  creation attempts, and selects a device matching the configured mode. This
  differs from the newer OSS `CScreen::CreateScreenDeviceL` implementation.

The N70 configuration contains two equally sized screen modes:

```
WINDOWMODE COLOR16MU
SCR_WIDTH1 176
SCR_HEIGHT1 208
SCR_WIDTH2 176
SCR_HEIGHT2 208
SCR_WINDOWMODE2 COLOR64K
```

WSERV builds `SCR_WINDOWMODE%d` at `0x505D92B6`, parses an override into each
mode's `+0x38` field at `0x505D93E0`, and otherwise inherits the global mode at
`0x505D9418`. Thus the configured default is 16MU and the second mode is 64K;
these are distinct modes even though their dimensions match. This is static
ROM evidence, not a physical N70 observation during the game.

EKA2L1 ignored the per-mode field, returned the global mode for every query,
and changed only dimensions when switching modes. The query also used the
wrong reply mechanism: the [client's `WriteReplyInt`](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/master/windowing/windowserver/nonnga/CLIENT/RSCRDEV.CPP)
and [server's `SetReply`](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/master/windowing/windowserver/nonnga/SERVER/scrdev.cpp)
return the mode as an integer, not in an output descriptor.

The fix stores display and initial DSA formats per screen mode, returns the
requested mode through the IPC completion value, and updates the active format
and framebuffer stride on a switch. A color-depth change terminates active DSA
with the display-mode-change reason and discards the old transfer texture.
The existing EKA1 DSA depth detection remains available; it is a compatibility
heuristic for clients' pixel writes, not evidence of a hardware color limit.

Runtime validation distinguishes the two paths: with the default N70 mode the
renderer failure persists. Selecting screen mode 1 (the second mode, 64K) using
the existing per-game settings allows the menu, intro and Lara's Home to run.
The captured game session showed 60 FPS in the 3D room without access violations.
No title-specific mode override is added to emulator code or shipped configuration.

Further targeted tracing identified the game's renderer-selection failure.
During startup in mode 0, screen-device requests query display mode and current
size/rotation; none requests a global mode switch or enumerates per-mode display
formats. The captured Avkon SGC requests specify application mode `-1` (unset).
The game's `Main` thread also requests `UserSvr::ScreenInfo`.

At runtime address `0xE0A3CC48`, the renderer factory calls a wrapper at
`0xE0A328DC`, whose import resolves to N70 WS32 code at `0x505F3265`.
That function issues session opcode 63, `GetDefModeMaxNumColors`, and returns
the display-mode field of the reply. The factory accepts only enum values 7
(`EColor64K`) and 10 (`EColor4K`); otherwise it returns null. Its caller at
`0xE0A50184` dereferences that null object and attempts a virtual call. The
first access-violation capture has `r0 = 0`, `lr = 0xE0A50194`, and invalid
`pc = 0x0A020D00`. Thus the reported reads are consequences of an invalid
instruction-fetch target, not evidence of framebuffer-stride overrun.

This startup path does not implement automatic fallback to another screen mode.
Manual mode 1 succeeds with EKA2L1 because its `GetDefModeMaxNumColors` reply
uses the active screen's display mode. A separate fidelity question remains:
the later OSS WSERV implementation uses `FirstDefaultDisplayMode()` instead.
The N70 ROM server implementation of that specific reply has not been traced,
so manual success must not be presented as proof of real-N70 automatic switching
or of the exact original server contract. No automatic selection was added.

## 3. A host alert can block input

An iOS modal alert with unreadable text covered the emulator during validation.
Dismissing it restored access to the keypad. A subsequent debugger capture at
`drivers::ui::show_yes_no_dialog` recovered the guest's original message:
`Ideaworks3D` / `App Error: -1`, with button labels `a` and `b`. The call arrives
through `notifier_client_session::notify`; it is a guest notification rendered
by the iOS frontend, not an iOS permission request. The message originates in the
game-installed `E:\System\Libs\ECSTB.DLL`: at its preferred image base,
`0x1000089C` formats the error, `0x1000093C` selects `App Error: `, and
`0x10000954` appends the integer. Its call at `0x1000095C` enters the notification
helper at `0x10000620`, which supplies `Ideaworks3D` and the two button labels.
The guest stack then identified the failed operation: the `GSBAPP` thread in
`apprun[10003a4b]0001` calls the library-loading helper at `ECSTB + 0xD60`.
Its load at `+0xD90` returns `KErrNotFound (-1)`, branches to the error reporter
at `+0xD98`, and leaves return address `ECSTB + 0xD9C` on the captured stack.
The filename descriptor contains `BIGINT.DLL`; the accompanying UID type is
`{0x10000079, 0, 0x10005E0F}`. The original import name retained on the stack is
`BIGINT[10005e0f].DLL`. The emulator loader independently logs that this load
failed before returning `error_not_found`.

This is a missing guest dependency: `BIGINT.DLL` is absent from the N70 ROM's
directory and the installed C:/E: libraries. The native N-Gage ROM contains
`/System/Libs/Bigint.dll` at ROM header address `0x507C91A0`, with matching
UID3 `0x10005E0F`. No library was copied between ROMs and no missing-load result
was suppressed. Temporary stack and code-segment tracing was removed after
diagnosis.

The unreadable presentation is a separate lifetime bug in `input_dialog_ios.mm`:
the dispatched block retains references to the caller's temporary/local UTF-16
strings rather than owning their contents. At the main-thread block breakpoint,
the button string's stack storage had already been overwritten. The iOS fix
copies the strings into values owned by the asynchronous blocks, including the
same lifetime issue in input-dialog initialization and cancellation.

A targeted Release simulator check on N70 confirmed the original message and
both labels display correctly; tapping `a` dismisses the alert, and Select opens
the game's New Game menu. The captured log had no guest panic or access violation.
No regression suite was run for this dialog change, as requested. The underlying
guest error remains separate from the presentation fix.

## Verification

Validated on 2026-09-12 at `9da421a52` plus the per-mode working-tree changes,
using the final Release simulator build (installed executable SHA-256
`4e28572eee51fce07f281bd87f82a744a679dd7ecf943af34216ed593388ec97`).

- The screen-mode test passed 16 assertions, covering equal-size depth changes,
  framebuffer stride, DSA texture disposal, repeated selection and invalid modes.
  The existing window-surface and GDI-store tests passed 104 assertions in 17 cases.
- The default iOS regression suite passed all 12 checks; Angry Birds passed all
  5 checks, including touch navigation and carousel dragging. Saved screenshots
  were inspected visually.
- N70 Tomb Raider reached the intro and Lara's Home with screen mode 1 selected
  in the existing per-game settings. The default 16MU path still failed.
- Native N-Gage (`nem-4`) Tomb Raider displayed its main menu and responded to
  Select by opening the New Game menu. Both successful Tomb Raider sessions had
  no guest panics, access violations or graphics halts in the captured logs.

The first default-suite attempt ended in a host `objc_release` crash inside
UIKit navigation-bar dictionary merging. A full retry with the same installed
binary passed, and subsequent launches did not reproduce it. Its cause remains
unresolved; these results do not establish that it is pre-existing or fixed.
Earlier results with the global 64K clamp do not validate this implementation.
