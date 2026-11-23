# Smash The Bugs: unavailable X7 haptics becomes USER 0

Smash The Bugs 1.2 (`0x20066B64`) on the X7/Anna ROM (`RM-707`) reaches its
Loading screen and then panics with category `USER`, reason `0`. This is a guest
panic; the iOS host stays alive. The package investigated was
`Smash the bugs-34s60v529.sis.sisx`.

## Failure chain

The QML feedback module starts a haptic effect while constructing the game.
`QFeedbackSymbian::setEffectState()` lazily obtains a `CHWRMHaptics` instance through
`QFeedbackSymbian::haptics()`. Opening its actuator reaches the ROM's haptics server,
packetizer and hardware adaptation:

```
hwrmhapticsvibepacketizerplugin.dll
  -> hapticsadaptation.dll
  -> isaaccessldd.ldd / logical device iaddriver
```

EKA2L1 has no factory for `iaddriver`. `logical_channel_create()` in
`src/emu/kernel/src/svc.cpp` returns `KErrNotFound (-1)` when no registered LDD
factory accepts the name. The adaptation leaves, and the haptics server subsequently
leaves with `KErrNotSupported (-5)`. A temporary guest-stack probe confirmed the
same error reaching the game through `hwrmhapticsclient.dll` and the call to
`OpenActuatorL()` in `qtfeedback_symbian.dll` (return offset `+0x8F1`).

The reason this becomes a panic, instead of merely disabling vibration, is the
Qt plugin's exception boundary. Its `haptics()` helper wraps `NewL()` and
`OpenActuatorL()` in `QT_TRAP_THROWING`, which catches a Symbian Leave and calls
`qt_symbian_throwIfError()` to raise a C++ exception. Its caller uses the Symbian
`TRAP` macro around the effect operation. That macro catches `XLeaveException`
separately, but calls `User::Invariant()` for any other C++ exception. The converted
Qt exception therefore reaches the invariant branch and causes `USER 0`.

The actual ROM plugin was decompressed and disassembled to check this against
source, rather than assuming a current source tree exactly matched the ROM:

- `+0x8EE`: virtual call to `OpenActuatorL()` inside the helper's TRAP.
- `+0x912`: call to `qt_symbian_throwIfError()` after its trapped result.
- `+0xB66`: caller enters the lazy haptics helper while starting an effect.
- `+0xB88`: caller's catch-all landing pad calls `__cxa_begin_catch()`.
- `+0xB8C`: calls EUser ordinal 669, `User::Invariant()`; saved return is `+0xB91`.

The matching source is Qt Mobility's
[`qfeedback_chwrmhaptics.cpp`](https://github.com/qtproject/qt-mobility/blob/master/plugins/feedback/symbian/qfeedback_chwrmhaptics.cpp).
The installed Belle SDK's `qglobal.h` defines `QT_TRAP_THROWING`; `e32cmn.h`
defines the invariant in the TRAP catch-all. Local Symbian OSS sources under
`oss.FCL.sf.os.devicesrv/hwrmhaptics` document the client/server initialization and
unsupported-resource paths. This evidence does not require a general guest
exception-unwinding defect to explain the crash.

## Isolation and scope

At revision `9c0ccbcda`, Release iOS Simulator, temporarily removing only the
`qtfeedback_symbian.qtplugin` metadata let the same installation pass Loading and
reach the game with moving bugs. Restoring it reproduced `USER 0`. This was a
labelled diagnostic experiment, not a proposed per-game workaround. The plugin
was restored and all temporary source probes were removed.

The missing capability is the shared Symbian haptics path, rather than iOS touch
input or graphics. A general fix should provide the HWRM haptics contract through
an emulator service or client patch, with coherent actuator capabilities and effect
lifecycle. Pretending the Nokia modem/IAD channel opened successfully would expose
further unimplemented hardware IPC and is not a sufficient fix. Disabling the Qt
plugin globally would also hide feedback support from other applications.

## Shared service fix

The shared emulator now supplies `!hwrmhapticsserver` for EKA2 devices. The ROM
client, packetizer and Qt feedback plugin remain intact; only the hardware-facing
server is replaced. The implementation follows the OSS client/server split in
`hwrmhapticssession.cpp`, `hwrmhapticsservice.cpp`, `hwrmhapticscommondata.cpp`, and
`hwrmhapticsreservationhandler.cpp`. The X7 packetizer's vtable was matched to
`hwrmhapticspacketizer.h` to confirm the packed Vibe command layouts.

The service initializes per-session resources, advertises Any/Device actuator
aliases, opens the virtual actuator, and returns decoded outputs starting at slot
1. Capability requests contain the logical actuator type, unlike most other
requests, which contain the device handle. This distinction was confirmed by an
independently compiled Belle SDK program running against the X7 ROM client.

Simple MagSweep and Periodic effects have per-session handles and states; pause,
resume, stop, disable, strength and duration changes preserve the SDK lifetime
semantics. Updating an effect's duration does not reset its elapsed time. Output
uses the existing cross-platform vibrator interface, approximating the waveform
and envelope with the effect's duration and magnitude. IVT/timeline, streaming,
bridge commands and proprietary licensing are unsupported, rather than being
reported as successful operations.

Reservations use the default untrusted-client policy: equal priorities queue in
FIFO order; subsequent reservers become suspended and are promoted on release.
Force-without-CCoeEnv is rejected for untrusted clients. Haptics and actuator
notifications use the OSS slots and stay pending when no state changes. Cleanup or
disconnect cancels the retained request exactly once, releases reservations and
stops output owned by the session. Server shutdown clears ownership before
tearing down sessions, so destructors cannot traverse partially destroyed state.

Malformed packets, bad handles, read-only output descriptors and short output
buffers are rejected. A failed output write does not commit an Open or consume an
effect handle. Host tests cover the X7 packet layouts, malformed/truncated input,
state transitions, elapsed time, independent sessions and infinite duration. The
independent SDK program additionally checks real client calls, raw IPC, actuator
notification order, reservation promotion and cancellation on cleanup/disconnect.

## Verification

The final shared sources are fork commit `a5db8a9aa` and upstream branch commit
`96fe8bd14`; the eight changed source/build/test files are byte-identical between
them. The Release iOS Simulator package passed the default regression 12/12 and
Angry Birds real-HID touch regression 5/5 on the iPhone 16 Pro simulator (iOS 27).
Screenshots of the Final Battle prompt, calculator menus and Angry Birds carousel
were reviewed. Release host `ekatests '[haptics]'`, run from its build directory,
passed six cases with 378 assertions, as did an isolated ASan/UBSan build of those
tests. The Belle SDK ARMv5 probe passed all 63 checks against the final package's
X7 ROM client and HLE server, including notification cancellation on disconnect.

Smash The Bugs passed Loading with the original Qt feedback plugin, rendered
moving bugs, and responded to opening its menu and returning to the game. The
final log scan found no guest panic, access violation, graphics halt, missing IAD
device or leftover diagnostic probes. The independent probe app and its guest log
were removed after validation. Physical waveform fidelity was not tested on a
handset. Menu interaction still reaches existing SpriteDLL/redraw-canvas stubs;
those graphics paths are unchanged by this fix.
