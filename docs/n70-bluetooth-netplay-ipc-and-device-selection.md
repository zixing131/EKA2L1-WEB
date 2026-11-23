# N70 Bluetooth netplay: EKA1 IPC headers and device selection

Explode Arena and Dragon World both failed to start multiplayer on the N70 ROM,
although the same local Bluetooth transport worked for newer ROMs. Explode Arena
could create neither a usable SDP service nor a working client query. Dragon World
returned from its client role before it attempted any SDP or RFCOMM operation.

## Explode Arena

The N70 sits on a boundary that the socket-server implementation did not cover.
It uses the Symbian 8.1a generic opcode table rather than the older rearranged
table, but several generic socket, resolver, and NetDB operations were absent from
the dispatcher. In addition, the inet Bluetooth link-manager protocol returned no
socket at all. S60 opens that raw control socket and enables it with option 1 before
issuing HCI/link-manager controls; it does not need a user-data transport.

The generic 8.1a operations are now routed to their existing implementations, and
the inet link-manager supplies a control-only Bluetooth socket with no backing TCP
connection. Acceptance of the otherwise undefined protocol-level option is limited
to the exact link-manager option used by this stack.

The deeper failure was hidden by the N70 ROM's IPCv2-compatible server API. EKA2L1
used the platform version to decide that these requests carried a `TIpcArgs` type
header. The N70 kernel is still EKA1, however, and its client-side send ABI passes
only the legacy `TAny *[4]` argument array. The word after that array is unrelated
stack data. Treating it as a header assigned random descriptor types to otherwise
valid SDP database requests.

After the missing socket paths were restored, comparing EKA1 client construction
with the server-side descriptor access in the local Symbian sources made the
distinction clear: server IPC semantics can be the newer form while the syscall ABI
remains headerless. Socket-server tracing then
showed that the broken SDP open/create path recovered as soon as all EKA1 session
sends were decoded without a header.

The fix is in the common EKA1 session-send bridge, rather than in SDP or either
game. Both synchronous and asynchronous EKA1 sends now always use the legacy
headerless argument layout. Descriptor objects are still validated normally.

## Dragon World

Dragon World had a separate prerequisite. Before opening the socket server it calls
the standard Bluetooth device-selection notifier, UID `0x100069D1`. EKA2L1 had no
plugin for that notifier, so the request completed without the selected-device
payload and the game returned to its role menu.

The current SDK definition was not sufficient to reconstruct the N70 response:
its `TBTDeviceClass` is larger than the EKA1 type. Disassembling the N70 ROM's
`btextnotifiers.dll` established the actual 544-byte package layout. The Bluetooth
address starts at byte 0, the `TBuf<248>` name at byte 8, the four-byte EKA1 device
class at byte 512, and the three validity fields at bytes 516, 520, and 524. EKA2
keeps the package size but moves those fields after a twelve-byte device-class
object.

A generic device-selection notifier now returns the first configured netplay peer,
using the virtual Bluetooth address supplied by the inet midman and the response
layout appropriate to the guest kernel generation. With no configured peer it
returns not found instead of fabricating a device.

After both fixes, two N70 simulator instances completed real host/client sessions
in Explode Arena and Dragon World. Explode Arena reached the same two-player
deathmatch. Dragon World passed its waiting screen, synchronized the difficulty and
stage selection, and rendered the same live encounter with both player dragons and
both score counters.
