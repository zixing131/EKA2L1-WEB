# GetImage is a viewfinder poll, not a photo: Killer Virus stuttered behind a shutter sound

## Symptom

*Attack of the Killer Virus* on the N70 (`nem-4`) crawled on a physical iPhone, and
the phone kept playing the camera shutter sound over and over for as long as the game
was on screen. Android showed the same stutter — a good hint that nothing in the iOS
backend was to blame on its own.

## Narrowing it down

The emulator log stopped at app startup (the game's `.pcm` sound files being opened),
with no camera line anywhere: the EKA1 `CameraServer` HLE had no logging at all, so
the log said nothing about what the game did next. What it did show was the
dependency closure of `killervirus.app` — `cameraserver.dll`, `ecam.dll`, `camext.dll`,
`camservercore.dll`, `jpegdecoder.dll` — i.e. the game is a camera app.

Two things narrowed it from there:

* On an EKA1 device the `ecam` patch DLL never binds. The log is full of
  `Unable to patch export N of ecam_general.dll` (exports 4..25 don't exist in the
  8.1a `ecam.dll`) and `Invalid ordinal 2123/2128/2132 (of 2073) ... from euser.dll`.
  That patch is built against the S60 5th SDK, so on Symbian 8.1a the ECam dispatch
  path is simply not in play. The game's camera traffic goes to the HLE
  `CameraServer` (`src/emu/services/src/camera/camera.cpp`), which is registered for
  EKA1 only.
* That server implements exactly one frame-producing opcode, `GetImage` (4), and it
  served every call with `instance::capture_image()` — a **still photo capture**.

Running the game against the simulator's mock camera made the access pattern obvious:
`Service.Track` logged `Calling service: CameraServer, id: 4` about **15 times a
second**, continuously. `GetImage` is not "take a photo" for this protocol; it is how
an app that predates `CCamera`'s viewfinder API draws a live viewfinder — it polls
frame after frame after frame.

So each displayed frame ran a full host photo pipeline. On iOS that is
`AVCapturePhotoOutput capturePhotoWithSettings:` — which starts the capture session,
captures at the sensor's full photo resolution, produces a JPEG, decodes it,
rescales it to 640x480, converts it, and stops the session again — plus the system
shutter sound, which apps cannot suppress. On Android it is
`ImageCapture.takePicture` followed by a JPEG decode and scale. Fifteen of those per
second explains both the stutter and the machine-gun shutter.

Worth noting as a dead end for anyone chasing this from the log alone: the `ecam`
patch failures look alarming and are real, but they are not this bug. They only mean
the S60v3+ ECam path is inert on EKA1, which is exactly why the `CameraServer` HLE
exists.

## Fix

Serve `GetImage` from the driver's **viewfinder feed** instead of a still capture, and
leave the feed running between requests:

* `camera_session` now owns a `feed_state` shared with the driver callbacks (frames
  keep arriving on a driver thread, so that state has to outlive the session that
  started it). `wants_frame` gates conversion work so the backend does nothing while
  no request is pending; `pending` is only touched under the kernel lock, which the
  frame callback takes anyway to complete the request.
* The feed is (re)started when the size or format changes — `SetImageQuality` swaps
  between 160x120/`EColor4K` and 640x480/`EColor16M` — and when a previous feed
  reported an error, so a dead feed cannot leave a client waiting forever.
* `start_feed()` drops the kernel lock around `receive_viewfinder_feed()` for the same
  reason the old capture path did: a driver that cannot start the feed reports it
  through the same callback, synchronously.
* Both backends deliver the viewfinder at exactly the requested size with FBS-style
  4-byte-aligned scanlines, which is what `epoc::get_byte_width()` computes for the
  target bitmap, so the copy is now a straight `memcpy` and the nearest-neighbour
  rescale only survives as a fallback.
* The resolution-ladder search is gone (the feed is requested at the size we want),
  replaced by a `supported_frame_formats()` check — both backends drop a feed request
  for an unknown format *without* calling back, which would hang the client.

The EKA1 `CameraServer` also had no logging whatsoever; it now traces feed start/stop
and the quality/lighting settings under `Hle.Camera`.

## Result

On the simulator the game runs at a steady 15 FPS with the camera poll rate matching
it exactly — the camera is no longer the limiter, the emulator's render is. The
still-capture path is untouched for the ECam dispatch path, where a guest asking for
a photo really does want one.

One thing this does not address: `orientConnection` forces portrait delivery, so a
640x480 landscape request is served by squashing a 480x640 portrait buffer. That is
pre-existing behaviour shared with the ECam path, and it costs a CGImage rescale per
frame on top of the format conversion.

---

# Part two: frames arrived sideways, and the simulator could not see it

## Symptom

With the feed in place, the picture on hardware was rotated — on iOS *and* on
Android, which already ruled out one backend misbehaving on its own. Two devices,
two apps, five observations (D is the scene's "up" as displayed, counter-clockwise
from screen-up; φ is how far the phone itself was turned counter-clockwise):

| app | φ = 0 | φ = +90 | φ = −90 |
|---|---|---|---|
| Killer Virus (N70, portrait guest) | D = +90 | D = 0 | — |
| Camera (5320, landscape guest) | D = 0 | D = −90 | D = +90 |

## Narrowing it down

Both rows have the same slope in φ, so the frame was bolted to the *phone* and
ignored the guest picture entirely. The rows differ by a constant, and the wsini
files explain it: `rm-409` has `SCR_ROTATION2 90` for its 320x240 landscape mode
while `nem-4` declares no `SCR_*` at all. A presenter probe confirmed the camera app
switching to `mode.rotation=90` and Killer Virus sitting at `mode.rotation=0`.

That fits `D = 90 + hir − (mode.rotation − panel_mount)` on all five observations —
where the leading 90 is a raw landscape-right sensor readout that was never
corrected. `orientConnection` asked AVFoundation for portrait and, on hardware, that
request simply did not take.

The simulator was no help here, and not by accident: its mock camera synthesized the
test pattern straight at the requested size, so it never entered the rotate-and-scale
path at all. It was blind to this class of bug by construction.

## Fix

`camera::set_frame_rotation()` carries the angle a frame — already upright in the
host's natural orientation — still has to turn to be upright in the guest's picture.
The iOS presenter pushes it from the same place the accelerometer angle is computed.

The two angles are *not* the same, which is the subtle part:

* The **host** term enters with opposite signs. Turning the phone counter-clockwise
  spins the scene clockwise inside a sensor buffer, while the interface counter-
  rotates the picture to keep it upright for the viewer, so the camera takes
  `−host_interface_rotation` where the accelerometer takes `+`.
* The **guest** term keeps its sign for both. An app that composes for a rotated
  panel already lays the frame out for that panel.

Getting this wrong is invisible in a portrait test, where the host term is zero —
which is exactly how the first cut of this fix shipped a sign error past two green
simulator runs.

Backends now own the "raw readout to upright in the host's natural orientation" step
and add the shared angle on top:

* iOS pins the capture connection to the cameras' native landscape-right readout
  rather than asking for a rotation that hardware ignored, and folds the whole angle
  into the CGContext pass that was already rescaling the frame — so the rotation is
  free. Stills take the same angle.
* Android pins `ImageAnalysis` to `Surface.ROTATION_0` so `getRotationDegrees()`
  always means "upright in the natural orientation" instead of depending on what the
  display was doing when the use case was bound, then subtracts the guest angle.
  Only the backend can see the host display there, so it folds the display rotation
  in itself and the presenter contributes the guest term alone.

The simulator's mock now synthesizes in the landscape shape and orientation a real
sensor reads out in and goes through the identical rotate-and-stretch, so the
orientation path is finally exercised there.

## What was deliberately left alone

**The stretch.** A guest scales the frame over its own window, and on hardware the
sensor buffer has a fixed shape whatever the screen is, so the two stretches largely
cancel: Killer Virus ends up ~13% wide instead of the ~37% *squashed* that
aspect-preserving crop would produce. Filling the destination edge to edge is the
faithful behaviour, not a shortcut.

**`ui_rotation`.** The emulator's own picture rotation is not in the angle, matching
the accelerometer, which has the same gap.

**Android stills.** That path reads `getRotationDegrees()` only to compute an output
size and never rotates anything — a pre-existing gap, left as is rather than
restructured blind.
