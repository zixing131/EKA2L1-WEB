# Controller touch, motion and haptics on an external iOS display

The game-only AirPlay output had no way for a controller to interact with touch
games. Its framebuffer excludes phone controls, and a controller's ordinary key
mapping only emits Symbian key events. Motion and vibration also used the phone
regardless of which peripheral was active.

The controller mapping editor now has an AirPlay & Full Screen section with a
pointer switch, six independently bindable pointer actions, and motion and
controller-vibration switches. These preferences are stored per controller model
separately from guest key bindings. The default pointer actions are R3 to show or
hide, d-pad/left stick to move, and A for touch. Holding A while moving produces
a drag; releasing A ends it. Only pointer input requires both an active external
display and Full Screen. Motion and vibration follow the active controller while
the game is visible, including when the pointer is hidden.

Pointer state lives in normalized picture coordinates. The bridge publishes the
same phone-surface crop used by the external blit under a short geometry lock.
Touch events use that crop and the existing rotation-aware window-server input
path. The external view fits that picture into its bounds and overlays a
high-contrast arrow whose tip is the touch location. A display link moves the
arrow even when the guest does not redraw. No framebuffer readback or forced
guest redraw is needed to move it.

Pointer mode consumes its bound controls before guest key dispatch. Inputs held
while changing modes remain suppressed until released, preventing a held touch
button from becoming an unintended game key or a newly enabled pointer from
starting a drag. Hiding the pointer, changing geometry, disconnecting, leaving
Full Screen or suspending the game cancels any synthetic contact. Its reserved
identity uses the same bounded touch-slot allocator as phone touches.

The iOS sensor backend samples either CoreMotion or the selected controller's
GCMotion at the requested guest sampling rate. Manual controller sensors are
enabled only while a channel is listening and the driver is not paused. The
accelerometer channel receives total acceleration; the Symbian Rotation channel
uses separated gravity when the controller provides it, or total acceleration
otherwise. Rotation retains the Symbian absolute-angle contract described in the
Icy Tower investigation; it is not a raw angular-velocity channel. Controller
axes follow the guest picture independently of the phone's physical orientation.
Phone CoreMotion is restored when there is no selected motion-capable controller.

A serialized timer and the driver's existing owner guard prevent queued work
from reaching a destroyed driver. Pause cancels sampling and waits for callbacks
already in flight. Guest callbacks run outside the listener-list lock to preserve
the kernel/list lock order.

Vibration requests create a Core Haptics engine from the selected controller's
default haptic locality when supported, with the phone as fallback. Each new
request replaces its previous player; an indefinite request loops until stopped.
Switching controller or backgrounding stops registered engines and invalidates
their cached routing revision. Engines and players are serialized with route
changes, and no asynchronous callback captures a vibrator's C++ lifetime.
The common driver API's zero intensity remains its default-intensity sentinel;
negative motor direction is represented by intensity magnitude.

Apple's [GCMotion](https://developer.apple.com/documentation/gamecontroller/gcmotion),
[manual activation](https://developer.apple.com/documentation/gamecontroller/gcmotion/sensorsrequiremanualactivation)
and [GCDeviceHaptics](https://developer.apple.com/documentation/gamecontroller/gcdevicehaptics)
contracts determine capability checks and activation. The local Symbian
`hwrmvibra.h` defines replacement and indefinite-duration behavior.

The Swift host harness exercises real GameController snapshots, click/drag
pairing, cancellation, held-button transitions, dead zones, diagonal speed,
boundaries, aspect fitting, remapping and persistence. The simulator feedback
harness runs the actual sensor/vibration backends with controller snapshots and
mock haptic engines; it covers packet delivery, axis transforms, pause/resume,
source changes, repeated teardown, routing, stop/replacement and engine failures.
These tests do not verify physical controller vibration or AirPlay latency.

Both Release builds succeeded. The default guest regression checks, string
catalog validation and all five Angry Birds touch checks passed. Final Battle
and Angry Birds screenshots showed the expected game/episode views. The Release
build was installed and launched on iPhone Air, and the user confirmed that
the controller features worked in physical-device testing.
