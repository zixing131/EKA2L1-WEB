# AirPlay game output with controls on the phone

Ordinary screen mirroring includes the virtual keypad, FPS overlay and host menus.
The iOS frontend now registers a noninteractive external-display scene so an
AirPlay receiver shows only the guest picture. The phone keeps its existing
render view, touch coordinates, virtual keypad and peripheral input. Connect a TV
or Mac using Screen Mirroring in Control Center; disconnect there to stop output.
The external window stays black outside a game. Wired external displays use the
same scene path.

The output is an iOS-only GLES framebuffer blit, before presenting the phone's
non-retained drawable. Reading that drawable after presentation would be undefined.
Each present carries its own crop rectangle through a FIFO beside the existing
two-frame presentation fences. The crop is recorded before the guest-rotation
transform changes the drawing rectangle, so the external output matches the
already-rotated phone picture. It removes the phone's letterbox area, fits the
picture inside the external drawable, and preserves aspect ratio. UIKit overlays
never enter that framebuffer. There is no CPU screenshot capture or video encoder.

Drawable storage is allocated on the main thread, borrowing the graphics context
while the graphics thread waits without holding the output mutex. The main-thread
block checks the current connection again, rather than a captured layer that may
have disconnected in the meantime. The output retains the layer and serializes
presentation with disconnect and foreground changes. GPU completion occurs before
releasing that lock. Framebuffer bindings, clear color, color mask and scissor
state are restored so the guest renderer sees its previous state. The most recent
output allocation is reused until a surface change, then replaced; it is deleted
with the graphics context during shutdown.

Lifecycle gating belongs to the phone's SwiftUI scene. Aggregating scene activity
at the App level could leave the emulator running because the external scene is
still active after the phone goes to the background. Orientation requests likewise
select only the application scene. Connecting or resizing an external display
asks the phone render view to lay out again, which also re-presents static guest
screens through the existing resize path.

A Home-button round trip exposed another lifecycle distinction: the phone scene
returned to foreground-active, but iOS left the noninteractive external scene in
foreground-inactive. Requiring both scenes to be active, and dropping the output
on sceneWillResignActive, left the TV black indefinitely while the phone worked.
Inspection of the live scene states and the hidden render view identified the
cause. The external scene now remains eligible in either foreground state,
reconnects on sceneWillEnterForeground, and disconnects on background or actual
scene disconnection. The phone's own active-state gate still stops output before
the application goes to the background.

The external window follows scene coordinate-space changes and has flexible width
and height, rather than retaining the size it had when the scene first connected.
Simulator mode switching needed care during verification: switching through
640×480 exposed no external UIScreen or UIWindowScene, and switching directly
back to 1080p left UIKit reporting a 720×480 screen inside a larger Simulator
window. Inspecting the live scenes and window bounds distinguished that mismatch
from a GPU crop failure. Fully disconnecting and reconnecting the simulated
display restored consistent dimensions and a centered picture; forcing render
coordinates to the Simulator window's pixel size would hide the wrong problem.

Playback audio already permits AirPlay. The recording session now also includes
AllowAirPlay so opening a guest microphone stream does not remove AirPlay from
its permitted outputs. Routing remains under system control.

Apple's contracts: [external display presentation](https://developer.apple.com/documentation/uikit/presenting-content-on-a-connected-display)
and [AllowAirPlay](https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowairplay).

Release simulator validation passed all 12 default checks (Final Battle, 5320
Calculator, N95 Calculator and string catalog) and all 5 Angry Birds touch checks
with a 1080p external display connected. Visual checks confirmed portrait
letterboxing, landscape game output without host overlays, and static-picture
recovery after disconnect/reconnect. A Home-button round trip preserved the
guest process and restored both the external picture and phone key input. The
user also confirmed game-only video,
working phone controls and TV audio on the installed iPhone Air build.

One touch-suite run captured the menu-to-episode transition before loading had
finished, failing the fixed-wait PLAY assertion. The episode page appeared later
without another tap. Repeating the unchanged suite on the same build passed all
five checks; no input workaround or relaxed assertion was added.
