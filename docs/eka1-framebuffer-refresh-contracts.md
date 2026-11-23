# EKA1 framebuffer refresh and the OS 7 ROMs

Styx Road: Hell Striker (黄泉道) on the N70 intermittently showed a black
lower half or a fully black frame. The regression was traced to c2fc53b15:
its 60 Hz polling could present the framebuffer after the game cleared it
but before it finished drawing and called `CFbsDrawDevice::Update()`.
The earlier investigation measured 15 black frames in 30 samples at that
commit and none at its parent.

Polling was introduced for a different hardware contract. The P900 ROM's
screen driver does not submit updates: its `Update()`, `Update(const TRegion&)`
and `UpdateRegion(const TRect&)` methods immediately return. Software that
writes the mapped framebuffer therefore needs presentation without an
explicit update call.

## The ROMs distinguish two contracts

The Symbian graphics source's `screendriver/inc/BITDRAW.H` gives the three
update methods empty default implementations. The hardware drivers in
`smomap`, `smint` and `smcot` inherit that behavior. Other screen drivers
implement submission; for example, `sgeneric/scdraw.inl` forwards the update
to its screen helper. An empty base method is not a promise that every
physical screen behaves alike.

The original EKA1 scdv images were inspected together with their
code in `SYM.ROM`, before emulator patch redirection. Export 2 constructs the
screen device on these ROMs. The old GCC vtable has two header words; counting
from the first method pointer, the update methods occupy slots 33, 34 and 35.
Bitmap-device tables can contain empty methods even on a submission device,
so the screen factory and its derived tables must also be checked.

| Device / firmware | OS | Screen update entry points | Refresh contract |
|---|---|---|---|
| Sony Ericsson P900 / P900 | 7.0 | `500c72e8`, `500c72ec`, `500c72e4`: each `bx lr` | Scanout |
| Nokia 9300 / RAE-6 | 7.0s | `5013be1c`, `5013be20`, `5013be18`: each `bx lr` | Scanout |
| Nokia 9500 / RA-2 | 7.0s | `5014bfcc`, `5014bfd0`, `5014bfc8`: each `bx lr` | Scanout |
| Nokia 9300i / RA-8 | 7.0s | `50144fcc`, `50144fd0`, `50144fc8`: each `bx lr` | Scanout |
| Nokia 7710 / RM-12 | 7.0s | `5010f8fc`, `5010f900`, `5010f8f8`: each `bx lr` | Scanout |
| Nokia 3650 prototype / NHL-8 | 6.x | Screen overrides submit through `5042ddec` | Scanout (see below) |
| Nokia 6680 / RM-36 | 8.0 | Screen overrides forward to helper `5005ad78` | Explicit submission |
| Nokia N70 / RM-84 | 8.1a | Screen overrides forward to helper `5005c7d4` | Explicit submission |
| Nokia N-Gage QD / RH-29 | 6.x | Screen overrides forward to helper `504127b8` | Scanout (see below) |
| Nokia N-Gage / NEM-4 | 6.x | Screen overrides forward to helper `50415928` | Scanout (see below) |

The P900 image has 12 draw-device tables; all share the empty update entries.
The four Series 80/90 images each have 17 and likewise share empty entries.
The N70 and N-Gage images additionally contain screen overrides that process
pending regions and forward updates. These addresses identify the inspected
firmwares; they are not constants used by the emulator.

## Refresh policy follows the observed OS boundary

Every ROM with empty update methods belongs to OS 7.0 or 7.0s, and the 8.0 and
8.1a controls implement explicit updates. The 6.x screen drivers also track
dirty rows and forward them, but that is not the whole contract there:
MiniBlaster (0x08180318) takes the panel address from `UserSvr::ScreenInfo`
and then only writes that memory during play. Its game thread makes no window
server, HAL, raw-event or draw-device call per frame, so on a 6.x phone the
mapped memory itself reaches the panel. Limiting the polling to OS 7 left that
game on a white screen once it entered a level.

The window server therefore starts its polling timer up to and including
`epoc7`, keeping the policy independent of firmware names and application
call order. It still records framebuffer
mappings for change tracking on every supported path. Explicit scdv updates
and the LCD driver's `lcd_driver_submit_queue` retain their existing
presentation paths.

A client calling `Update()` cannot disable polling on OS 7, and another
process querying or remapping framebuffer memory cannot enable it on later
versions. This avoids a runtime guess based on whichever client most
recently mapped memory or submitted an update.

The grouping is supported by the inspected ROMs, not a universal Symbian API
guarantee. A future ROM with a different display contract would require
refining this boundary. The inspected scdv image headers all have flags
`0x3`; those flags do not distinguish the two contracts. The checked ROM
header and standard HAL display attributes contain no explicit scanout versus
submission flag. `lcddev.dll` presence matches the current samples, but a DLL
name alone is not a refresh contract: N70 directly references it, while the
N-Gage scdv dependency tables do not include it despite implementing updates.
Binary recognition would need to identify the actual screen factory and its
methods, rather than mistake empty bitmap-device methods for screen behavior.
