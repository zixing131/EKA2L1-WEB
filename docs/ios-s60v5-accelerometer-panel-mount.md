# S60v5 accelerometer inversion — wsini rotation is panel-relative, not body-relative

## Symptom

Tilt steering in Asphalt 4 on the N97 (rm-507, S60v5) felt reversed: tilting the
iPhone left steered the car the wrong way. Asphalt 6 on the X7 (rm-707,
Symbian^3) — the title the orientation remap was originally calibrated against —
remained correct on the same build.

## Background

Commit `32b85e505` made the iOS sensor backend rotate CoreMotion samples into
the emulated device's frame: `θ = guest screen_mode.rotation + host interface
rotation`, applied as `R(−θ)` to the X/Y axes in `dispatch_sample`. The guest
part of that formula took `current_mode().rotation` (the wsini `SCR_ROTATIONn`
value) at face value as "CCW angle from the displayed picture to the emulated
device's natural orientation". Calibration on the X7 confirmed the whole chain
end-to-end, so the formula looked settled.

## Narrowing it down

Since the same code path was demonstrably correct on Symbian^3, the suspect was
whatever differs per device: the wsini screen-mode table. Dumping both ROMs'
`z:\system\data\wsini.ini` (UTF-16LE — `iconv` before grepping):

| Device | Portrait mode | Landscape mode |
|---|---|---|
| X7 (rm-707, S^3) | 360×640, rotation **0** | 640×360, rotation **90** |
| N97 (rm-507, S60v5) | 360×640, rotation **270** | 640×360, rotation **0** |

That table is only consistent with `SCR_ROTATION` describing the
**framebuffer-to-LCD-panel** rotation, not picture-to-body: S60v5 nHD devices
(5800/N97/5230 family) mount a landscape-native panel, so their portrait UI is
the rotated mode (270) and landscape is the native one (0). Symbian^3 devices
mount the panel portrait-native, so there the two notions coincide — which is
exactly why the X7 calibration worked and silently baked in the wrong
interpretation.

The real Symbian Sensor Framework defines accelerometer axes in the **device
body frame** (normal portrait orientation), independent of how the panel is
mounted. So the guest part of θ must be the picture-to-body angle:
`mode.rotation − natural_mode.rotation`. On the N97 the old formula was off by
the panel mount angle (a constant 90°) in every mode.

Why 90° felt like "reversed" rather than "sideways": racing games calibrate a
neutral point at race start. After that auto-zero, a 90° frame error makes the
steering axis read the roll-induced change of the *other* axis with flipped
sign — i.e. clean inverted steering, with the pitch cross-talk absorbed by the
calibration.

Dead end worth avoiding: neither game-specific behavior nor the legacy
RRSensorApi was involved (EKA2L1 only implements the Sensor Framework, both
titles use it); the difference was purely per-device data.

## Fix

`submit_screen_frame` (`IosEmulator.mm`) now subtracts the first screen mode's
rotation as the panel mount angle before adding the host interface rotation.
Mode 1 is the orientation the device is normally held in — the frame Symbian
defines the sensor axes in — so this compensates landscape-native panels while
leaving portrait-native devices (all Symbian^3 calibration anchors) untouched.
`set_motion_rotation` already normalizes negative degrees.

The Android backend still passes raw samples through and predates even the
mode-rotation remap; it keeps its known flaw and is upstream-candidate work.
