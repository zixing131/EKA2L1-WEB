# Talking Tom loses audio and flashes black between actions

Talking Tom on the X7 could animate without playing its WAV effects, ignored
microphone input, and briefly replaced the 3D scene with a black frame while
changing actions. Other applications, including Angry Birds, could still play
audio, so neither the iOS output device nor the global mixer was broken.

## Audio diagnosis

The effect-player path reached Qt Phonon, but Phonon asked AppArc to recognize
the file before creating the player. EKA2L1 advertised MPEG audio but not WAV,
and the generic recognizer returned `application/octet-stream` for a valid
`RIFF....WAVE` file. Phonon therefore selected its dummy player. Recognizing
the RIFF/WAVE signature and advertising `audio/wav` lets the normal player
path handle the same files without a title-specific exception.

The MMF device also replaced the guest's negotiated stream properties with
8 kHz stereo. Playback and recording now use the MMF configuration.

The first physical-device recording fix queried RemoteIO's hardware format and
performed a manual box-filter conversion to the guest rate. The result was not
speech: Talking Tom repeated noise. RemoteIO already has a client-side stream
format converter, and the manual path made its callback contract unnecessarily
different from the working Cubeb backend. The input unit now declares the
guest's signed 16-bit format on RemoteIO input bus 1's output scope and lets
RemoteIO convert between that and the current hardware route. The unit is
disposed after each recording cycle so a later wired or Bluetooth route gets a
fresh converter.

There was a separate routing bug. Recording selected `PlayAndRecord` with
`DefaultToSpeaker`, which deliberately changes iOS's normal output-route
policy. Talking Tom keeps recording active while it listens, so effects stayed
on the phone speaker even after headphones were connected. Input sessions now
use `PlayAndRecord` without that override, allow HFP and A2DP routes, and return
to the steady-state `Playback` category when the last recorder stops.

## Black-frame diagnosis

High-frequency screenshots distorted the duration, so the transition was
recorded as video and correlated with temporary OpenVG command counters. The
black interval was not a `vgClear`, a failed image handle, or a Window Server
redraw split. A normal frame drew two full-window images: a base image followed
by the completed scene. At action boundaries the guest occasionally swapped a
construction frame that omitted the second full-window draw; the smaller GDI
controls were still valid, which is why only the scene turned black.

The first fix added a second full-screen EGL bitmap, replayed it during Window
Server composition, and made all EGL swaps non-blocking. Hardware testing
showed that it did not remove the flash, while the extra full-screen work and
lost GLES pacing reduced Angry Birds below 60 FPS. That scheme was removed in
full.

The replacement keeps the original single EGL surface. The OpenVG context
counts image draws whose transformed quad covers the window. Once a surface
has established a coverage level, one swap that drops below it is deferred.
If the next swap restores coverage, only the completed scene is published. If
coverage remains reduced for a second swap, it is accepted as a legitimate new
scene, so a real transition to a simpler or blank view is delayed by at most
one frame rather than hidden indefinitely.

Only OpenVG presentation is allowed to coalesce without sleeping the guest.
GLES keeps the original Window Server refresh pacing; this distinction is what
restored Angry Birds to 60 FPS without bringing back the OpenVG construction
frame.

## Result

A 32.7-second Release simulator recording exercised three action transitions
and contained no dark scene frames after removing all diagnostic logging.
Sampled frames kept the cat, room, and controls intact. Angry Birds again held
60 FPS, its touch regression passed 5/5, and the standard Release regression
suite passed 12/12 both with and without reinstalling the app.
