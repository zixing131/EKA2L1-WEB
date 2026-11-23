# Icy Tower tilt controls require the Symbian Rotation channel

Icy Tower on the X7 (rm-707) ignored physical iPhone movement in every holding
orientation. Asphalt 6 and Ferrari still responded to tilt on the same device.
The sideways game picture made another screen-rotation error look plausible,
but changing axis signs cannot help a game that never opens a sensor channel.

The installed Icy Tower executable (UID 0x2003B10C) includes a compiled Java-style
sensor adapter. Its native setup at image address `0x000b5ea0` creates a
`CSensrvChannelFinder`, constructs a channel-type filter of `0x10205089`, checks
that type in the returned list, then opens the channel and starts listening with
one desired item, one maximum item and a buffering period of 20 ms. The type
constant is built with ARM instructions, rather than stored as a literal, so a
byte search for the UID does not find it. It is the **Rotation** channel, not
Accelerometer (`0x1020507e`) or the old RRSensor API.

The iOS driver advertised and instantiated only an accelerometer. Filtering for
Rotation therefore returned an empty list even though CoreMotion worked. The
existing racing games exercised the supported channel and did not expose this
gap. No changes to the game's executable or to its screen orientation are needed.

The compatibility reference is Nokia's `sensrvorientationsensor.h` and the OSS
`oss.FCL.sf.os.devicesrv/sensorservices/orientationssy` implementation,
particularly `CSSYOrientation::DataReceived`,
`CalculateDeviceRotationInDegrees`, `SsyConfiguration.h`, and
`CSSYChannel::AppendData`. Rotation is a timestamp plus three signed degree
values, with the EKA2 ARM structure padded to 24 bytes. It reports absolute
angles around device axes, not gyroscope angular velocity. The reference derives
these angles from accelerometer data and quantizes them to 15 degrees.

The iOS backend now exposes a separate Rotation channel backed by the existing
CoreMotion pump. After the established host-to-guest axis mapping, it derives
X from `atan2(z, y)`, Y from `atan2(x, -z)`, and Z from `atan2(-x, y)`, normalizes
to 0–359 degrees, and applies the reference resolution. An axis parallel to
gravity has no measurable rotation and reports -1. `atan2` also handles cardinal
positions without the reference implementation's divide-by-zero special case.
The accelerometer channel retains its existing scaled samples and channel ID.
Both channels use the existing callback buffering, cancellation and teardown
barriers; the new channel adds no host callback or ownership path.

The shared query response also now writes `iChannelDataTypeId`, previously left
at the client buffer's initial value. The official `TSensrvChannelInfo` copy
constructor preserves this field, and data consumers use it to identify packet
layouts. This is a general metadata correction, not an iOS or game exception.

Android now exposes the same derived channel for each native accelerometer.
Its synthetic channel ID keeps the original accelerometer ID and marks Rotation
with the high bit, so opening either channel selects the same native sensor with
a separate event queue and packet conversion. The existing Android acceleration
axis convention is retained. Both backends use the shared angle conversion.

Unit checks cover all six cardinal positions, quadrant signs, undefined rotation,
angle wrapping, resolution, timestamps and independence from acceleration units.
Physical movement must still be checked on hardware because iOS Simulator has
no accelerometer.

Validation: 2,239 sensor assertions pass; the Android backend compiles for
ARM64 and ARMv7 with NDK r27/API 21. A host harness with a fake Android sensor
provider exercises the actual backend through discovery, filtering, channel IDs,
properties, concurrent acceleration/rotation delivery, request rearming,
pause/resume and close. This does not substitute for an Android hardware test.

The Release iOS simulator and signed device builds succeeded. Standard simulator
regression passed 12/12, Angry Birds touch regression passed 5/5, and Asphalt 6
reached the Nassau race with 9/9 checks passing. On iPhone
Air, the user confirmed Icy Tower tilt controls work correctly with the fixed
build, using the original locked orientation and phone-top-to-the-left grip.
