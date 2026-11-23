# X7 Bounce Boing Battle: common Bluetooth options and reform NetDB opcodes

Bounce Boing Battle could discover the other X7 instance over inet Bluetooth,
but hosting and joining failed at two different points. Creating a game stopped
after setting socket option family `0x2020`, option `0x997`. After that failure
was removed, joining stopped on socket-server opcode `0x42`.

The first request is not an L2CAP-specific option. The Symbian Belle Bluetooth
headers identify `0x2020` as `KSolBtSAPBase` and `0x997` as
`KBTSetNoSecurityRequired`. Symbian's common `CBluetoothSAP` consumes this option
before protocol-specific option handling. EKA2L1's inet Bluetooth sockets instead
forwarded it to the generic socket implementation, which rejected the unknown
family and caused the guest to leave with `KErrGeneral`.

Inet netplay has no Bluetooth security manager, and the requested policy is
specifically to disable security. The shared inet Bluetooth socket now accepts
that exact common-SAP option and continues to reject unknown options. L2CAP
delegates through the inet Bluetooth base so RFCOMM and L2CAP share the common
behavior without a title-specific exception.

Opcode `0x42` initially looked unrelated because older EKA2 socket-server tables
assign NetDB operations lower numbers. The Symbian communications framework's
`SOCKMES.H` shows that the Symbian 9.5 reform table assigns `ENDCreate` through
`ENDRemove` to `0x42` through `0x45`, with cancel and close at `0x95` and `0x96`.
The corresponding `RNetDatabase` client source confirms that `Open` creates the
subsession with address family and protocol as its two arguments, while query,
cancel, and close use those reform opcodes. EKA2L1 only dispatched the older NetDB
table, so the X7 request never reached the existing NetDB implementation.

Socket-server dispatch now selects the reform NetDB create, query, cancel, and
close opcodes for Symbian 9.5 and newer, preserving the existing mappings for
earlier systems.

With both fixes in a Release simulator build, two X7 instances discovered each
other, reached the shared connection-ready lobby, synchronized stage and match
length, and entered the same live court. Both remained at 60 FPS and the shared
match advanced to a visible 1–0 score.
