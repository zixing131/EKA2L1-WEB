# Rayman 9210 on the Nokia 9500 ROM

[Upstream issue #841](https://github.com/EKA2L1/EKA2L1/issues/841) reports
blue/green graphics, no audio and insufficient Series 80 controls. The attached
package is Rayman 1.0 for the Nokia 9210/9290: its bundled readme explicitly
lists those two devices. Testing it with the Nokia 9500 (RA-2, Series 80 v2)
reproduces the graphics problem, but the binary evidence does not justify an
emulator workaround for graphics or audio.

The investigation used fork revision `13f263f87141d2f843d844531eacd8abc8dc5445`,
a freshly built Release iOS simulator app, and the original `rayman.SIS` in
demo mode. The installed executable's SHA-256 is
`d76d381a1f1eac336abade34935c50a4d01dc4a4f2caf28b3a0b94e074b76155`.
No crack or game modification is needed to reach the first world.

The 9500 dump required manual device registration because its Z tree lacks the
product metadata understood by the installer. Registration used the existing
RA-2 Series 80 classification, `epoc7` and isolated writable drives. The original
ROM and Z files were preserved. This was a simulator test of this ROM/game
combination, not a test on a physical 9500 or a 9210 ROM.

The 9500 ROM SHA-256 is
`2f6f3b77a6ad89b125fd138faa96186531660b6da52e64e4b0642362cda3e733`.

## The game assumes a 12-bit framebuffer

The original 9500 `system/data/wsini.ini` declares `WINDOWMODE COLOR64K`.
Rayman obtains `TScreenInfoV01` through `UserSvr::ScreenInfo()` at linked
address `0x100444E8`, takes the screen address and adds fixed offsets. That
structure supplies an address and dimensions, not a pixel format. The game
does not query the video-info HAL to select a format appropriate to the 9500.

A debugger dump of the guest framebuffer contains the game's `0x0RGB` values,
including `0x06B6`, `0x0FE7` and `0x05F6`, alongside the ROM UI's RGB565
pixels. Symbian's `TRgb::_Color4K()` in `graphicsdeviceinterface/gdi/inc/GDI.INL`
defines this 12-bit encoding: red occupies bits 8–11, green bits 4–7 and blue
bits 0–3. Interpreting these words as the 9500's RGB565 produces the reported
blue/green image.

As an explicitly temporary diagnostic, changing the test copy's window mode
to `COLOR4K` restored the gold Rayman logo, normal menu colours and the first
level's blue sky and green terrain without changing the executable or renderer.
The original file was restored after the comparison. This demonstrates the
format mismatch; changing the 9500's mode globally would also reinterpret its
own UI and is not a valid emulator fix or a replacement for the right ROM.

## The 9500 ROM rejects the game's old audio interface

Rayman uses the pre-MMF `RMdaSession`/`RMdaObject` interface. The actual
9500 `MediaClient.dll` exports it imports contain these implementations:

| Ordinal | Interface | ROM address |
|---|---|---|
| 4 | `RMdaSession::Connect` | `0x5033CAAC` |
| 7 | `RMdaObject::SynchPackage` | `0x5033CBF4` |
| 26 | `RMdaObject::SynchCreateMessage` | `0x5033CB18` |
| 28 | `RMdaObject::SynchMessage` | `0x5033CBB0` |

Each is exactly `mvn r0, #4; bx lr`: return `KErrNotSupported` (`-5`). These
are ROM instructions executed by the emulator, not missing HLE server handlers.
The game's audio initialisation at `0x10046984` calls these functions but does
not act on their errors. Creating its Sound thread and reading `sound.dat`
therefore cannot establish working audio on this ROM.

Replacing those returns with success or supplying an invented audio session
would change the ROM contract. A compatible 9210/9290 ROM, or a separately
ported game using a supported audio interface, is needed to resolve this
compatibility problem. Actual audible output was not measured in this test;
the conclusion above follows from the complete imported audio path.

## Keyboard input and the touch overlay are separate

The tutorials use `Q`/`S` for left/right, `Z`/`X` for climbing/crouching,
`P` for punch and Enter for jump/continue. The simulator control API already
accepts all ASCII letters and sends their Symbian keyboard scan codes.
Both uppercase `S` and lowercase `q` moved the character in opposite
directions in the first level; Enter navigated menus and jumped. The temporary
12-bit display configuration made this movement directly visible.

The phone-style touch overlay lacks the full Series 80 keyboard. That is a
frontend feature gap, independent of the graphics and audio compatibility
failures. The API can exercise those keys without changing guest input
semantics; its documentation now spells out letter names and chords.

No shared emulator behaviour was changed for this investigation. In particular,
these findings do not support a PR claiming to fix all of #841 by changing
screen or audio emulation.

## The two 9300 dumps have the same incompatibility

The local Nokia 9300 ROM/RPKG and Nokia 9300i ROM/Z packages were tested
separately with isolated writable drives, the same Release build and the same
unmodified Rayman SIS. Both reach the title screen and first level. Both retain
the blue/green colours with their original `COLOR64K` configuration. Neither
supplies the old audio implementation: the four imported functions again
consist of `mvn r0, #4; bx lr`.

| ROM | `Connect` | `SynchPackage` | `SynchCreateMessage` | `SynchMessage` |
|---|---|---|---|---|
| 9300 | `0x5032BCCC` | `0x5032BE14` | `0x5032BD38` | `0x5032BDD0` |
| 9300i | `0x5033936C` | `0x503394B4` | `0x503393D8` | `0x50339470` |

Both Z trees contain `system/install/series80v20.sis`; changing to these ROMs
does not change the guest platform to Series 80 v1. The 9300 test used the
RAE-6 Series 80 classification and the RPKG's machine UID `0x101F8ED1`;
the 9300i test used RA-8. As with the 9500, they needed manual registration.
The 9300 RPKG also contains one document-template filename with an invalid
filesystem character; that template was omitted while extracting the test
copy. No system files were modified or omitted.

The ROM SHA-256 values are:

- 9300: `ca4b0bc929519b046994c8501b0135b688d8d7910d6669f4791805e3ab373596`
- 9300i: `d10e8a71700221e0586f2ebbc10634dcd00798eaf70b2e49d6c4b53c19b5c77d`

The 9500's built-in Desk calculator also rendered normally after restoring its
original configuration. That is a control for ROM UI rendering, not evidence
that a 9210-specific direct-framebuffer game is portable to it.
