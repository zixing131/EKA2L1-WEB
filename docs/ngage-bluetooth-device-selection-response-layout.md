# N-Gage Dragon World immediately leaves its Client role

Dragon World (`0x102735C5`) on the N-Gage ROM (`nem-4`, EPOC6) returned immediately
to its role menu after choosing As Client. The original report involved two
physical iPhones, with an iPhone Air as the client. Reversing the roles between
the Air and an iOS simulator reproduced the failure with a debugger attached.

## The response succeeds, but says the address is invalid

The shared Bluetooth device-selection notifier writes a 544-byte
`TBTDeviceResponseParams` package. It assumes a 248-character device name and
only distinguishes EKA1 from EKA2 when positioning the device-class validity
fields. That covers the previously investigated N70 and newer layouts, but not
the older N-Gage layout.

The N-Gage guest's initialized response descriptor had length and capacity
`0x220` (544). Its embedded name descriptor, however, had capacity `0x100`
(256), not 248. The package's total size therefore does not identify its layout.

Offsets below are relative to the package payload, excluding its outer
descriptor header:

| Field | N-Gage ROM | Current EKA1 response |
|---|---:|---:|
| Bluetooth address | 0 | 0 |
| Name descriptor | 8 | 8 |
| Name capacity, UTF-16 characters | 256 | 248 |
| Device class | 528 | 512 |
| Address-valid flag | 532 | 516 |
| Name-valid flag | 536 | 520 |
| Device-class-valid flag | 540 | 524 |

The N-Gage ROM's `btextnotifiers.dll` independently establishes the contract.
In this dump its code begins at guest address `0x50496ef4` and is Thumb code.
The response constructor at `0x50496f50` initializes the name with capacity 256,
constructs the class at `0x210`, and clears flags at `0x214`, `0x218`, and
`0x21c`. The address setter at `0x50496f90` copies the address and sets the flag
at `0x214`; the validity getter at `0x50496ff8` reads that same location.
These are client-library offsets, independent of the emulator's implementation.

The newer local Belle SDK declares a `TBuf<KMaxBluetoothNameLen>` with
`KMaxBluetoothNameLen = 248`. Its headers alone cannot establish the N-Gage
contract. Likewise, the N70 response documented in
[the earlier investigation](./n70-bluetooth-netplay-ipc-and-device-selection.md)
must not be generalized to every EKA1 ROM.

## Controlled check against the iPhone Air

The simulator used Direct IP to the Air, which remained in As Server. A direct
UDP virtual-address query received a valid reply from the Air. At the notifier's
completion breakpoint, the emulator had copied that same address into the
response and was completing with `KErrNone`. It had set flags at 516/520/524;
the actual N-Gage address-valid field at 532 was still zero. Continuing normally
returned the game to the As Client / As Server menu.

On the next request, the debugger changed only the outgoing response payload:
name capacity 256, class at 528, flags at 532/536/540, with the wrongly placed
class and flags cleared. The address and successful completion status were left
intact. This time the client proceeded through Connecting, established TCP
connections to the Air, and reached the two-player desert stage with 1P/2P HUD.
The user also confirmed the matching two-player game on the physical Air.
The debugger was then detached. This was an explicitly temporary diagnostic
mutation, not a game patch or a rebuilt emulator.

## Other observations that should not obscure the ABI failure

The Air's initial log contained a large stream of query-socket error `-57`
messages after the recorded guest teardown. A fresh app launch no longer had
that error stream and still reproduced the immediate return. It is not needed
to explain the controlled successful-response failure above.

The simulator initially retained a different netplay password. Matching it to
the Air was necessary for the LAN experiment, but did not fix the immediate
return reported on the Air. In the reverse-role LAN experiment, the simulator
notifier completed with `KErrNotFound`, so Direct IP was used to separate peer
discovery from response serialization. LAN discovery itself was not established
as reliable by this investigation.

## Fix

The error is in `write_selected_device` in
`src/emu/services/src/notifier/bluetooth.cpp`, shared by the synchronous cached
peer path and asynchronous discovery completion. Serialization now uses a
256-character name for EPOC6 and derives the class and validity offsets from
that capacity. The existing 248-character layouts and EKA1/EKA2 device-class
distinction are preserved for other versions. Changing all EKA1 responses to
the N-Gage offsets would break the N70 fix.

The S60 1st Edition SDK under `C:\Symbian\6.1\Series60` independently confirms
the old contract: `btdevice.h` declares `TBTDeviceName` as `TBuf<256>`, and
`btextnotifiers.h` uses that name in the response with no trailing padding after
the three validity fields. This agrees with the ROM constructor and accessors.

The permanent implementation built successfully for Release simulator and
physical iOS targets. The Release simulator regression suite passed all twelve
checks, including Final Battle, Calculator and N95 Calculator. The installed
physical Air, acting as a Direct IP client of the simulator, reached the map
without debugger edits. LAN mode still failed separately; see
[control-packet ownership](./bluetooth-control-packet-buffer-lifetime.md).
