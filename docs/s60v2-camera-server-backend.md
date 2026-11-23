# Connecting the S60v2 CameraServer to the camera backend

## Symptom

Some S60 1st and 2nd Edition applications use Nokia's old `CameraServer`
client instead of ECam. EKA2L1 had no implementation of that service, so an
application could stop during startup or wait forever for an image. A first
diagnostic implementation accepted the five known opcodes and returned a
blank frame, but it was not a camera implementation: it did not use the host
camera backend and its initial interpretation of `GetImage` ownership was
wrong.

## Finding both IPC contracts

The S60 2nd FP3 `cameraserv.h` contract says that callers pass only an empty
`CFbsBitmap`; CameraServer calls `Create()` with the selected size and display
mode. High quality is 640x480 `EColor16M`, while low quality is 160x120
`EColor4K`.

Raw EKA1 IPC slots were misleading. Slot 0 looked like a pointer and slot 1
contained another nonzero word, but the latter was just unspecified register
state. Inspecting slot 0 showed a writable four-byte descriptor. Disassembling
the ROM's `cameraserver.dll` client completed the picture: `GetImage` places a
four-byte output package in slot 0, waits asynchronously, then calls
`CFbsBitmap::Duplicate()` with the returned integer.

That integer must therefore be the FBS object's global server handle, not a
handle from an FBS client's local object table. CameraServer has to create the
bitmap in the FBS server, keep it alive while capture is pending, write its
global handle to the output package, fill its shared pixels, and only then
complete the request.

Killer Virus on the N-Gage then exposed an older revision under the same
server name and opcodes. Disassembly of that ROM's `cameraserver.dll` showed
the client checking the requested display mode and dimensions, calling
`CFbsBitmap::Create()` itself, and sending two integer slots: the bitmap's
global FBS handle and the `Create()` result. The host distinguishes the forms
structurally. A valid FBS global handle selects the old contract; a writable
four-byte descriptor selects the FP3 contract. This keeps the compatibility
rule tied to the ABI instead of a device or game UID.

Treating the package contents as an input handle was a useful dead end: the
word was merely uninitialised client storage, so FBS lookup correctly failed.
Having the client create the target bitmap was also incompatible with the SDK
API and would have left the returned `CFbsBitmap` empty.

## Backend and lifetime fix

The new CameraServer session owns one instance from the existing camera
collection. `TurnCameraOn` reserves camera zero, lighting selects automatic or
night exposure, and `TurnCameraOff` releases it. `GetImage` selects the nearest
backend size, uses the old client's validated bitmap or creates the FP3
bitmap, and resamples an asynchronous capture into its aligned scanlines. A
server-created bitmap is discarded on backend error, request cancellation, or
requester death if the client has not duplicated it. Session teardown marks
the shared capture state cancelled before releasing the backend, so a late
callback never touches guest or FBS state.

The camera backends needed an explicit FBS `EColor4K` format. iOS converts
BGRA camera pixels to little-endian XRGB4444 for low quality and aligned BGR24
for high quality. The simulator's existing synthetic camera follows the same
path as AVFoundation, which makes the service testable without pretending that
a blank image is a capture. Android advertises the same format and converts
CameraX bitmaps to the FBS layouts rather than returning Android bitmap memory
as if it were BGR or XRGB data.

Android also used to invoke camera callbacks while holding both its reservation
and callback mutexes. A callback into CameraServer takes the guest kernel lock,
while teardown may already hold that lock and wait for the reservation mutex.
Callbacks are now copied under the driver mutexes and invoked after releasing
them, matching the iOS backend and removing that lock cycle.

## Verification

A small application built against the exact S60 2nd FP3 SDK exercised both
quality modes through the public `RCameraserv` API on an S60v2 ROM. Both
asynchronous requests completed with `KErrNone`. The low-quality bitmap was
160x120 `EColor4K` with a 320-byte stride and 38,400 bytes of pixels; the
high-quality bitmap was 640x480 `EColor16M` with a 1,920-byte stride and
921,600 bytes of pixels. Rendering the returned buffers produced the complete
simulator-camera colour bars with the expected channel order in both modes.

Killer Virus exercised the older N-Gage contract continuously: it advanced
from its publisher splash through the menu into gameplay and composited its
HUD, targets, and viruses over live simulator-camera frames. That control is
important because passing only the FP3 probe would leave the N-Gage client
waiting at its splash screen.

The Android Java camera source compiles with the project toolchain, the iOS
Debug application builds, and the shared emulator test suite remains clean.
