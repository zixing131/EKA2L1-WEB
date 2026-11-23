# Local Network discovery through Apple's Bonjour service

The iPhone Air could join Dragon World through Direct IP after the N-Gage
notifier ABI fix, but automatic LAN discovery still failed. The LAN protocol
sent application-owned UDP broadcasts. Both the signed iOS app and its
provisioning profile lacked the multicast entitlement required for those
operations. Repairing asynchronous control-packet ownership corrected bad
unicast replies, but could not grant the missing broadcast capability.

Apple's [local network privacy guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)
allows system Bonjour APIs to publish and browse specific declared service
types without that entitlement. The application still needs the user's local
network permission. Both the iOS and macOS Qt bundles now declare
`_eka2l1._udp` in `NSBonjourServices` and include a local-network usage string.
The actual generated bundles were checked, not only their plist templates.

## Discovery contract

On Apple platforms, Local Network publishes and browses `_eka2l1._udp.local.`
through the system DNS-SD client. The service port is the existing Bluetooth
query port. TXT records contain protocol version `1`, the six-byte virtual
Bluetooth address, and a SHA-256 digest of the configured room password.
Matching is a discovery filter, as with the original room setting; the digest
does not make it an authentication or encryption protocol.

Each emulator instance has a name derived from its virtual address. Its own
advertisement is excluded, while matching instances on the same computer can
be found. The advertised query port is respected, allowing separate simulator
instances to use different query ports. IPv4 addresses are resolved through
DNS-SD and fed into the existing IPv4-mapped socket path. The game transport
and guest Bluetooth packet semantics do not change.

The Apple path does not open the old UDP broadcast listener. The legacy
non-Apple LAN implementation is retained, so automatic discovery between an
Apple build and a non-Apple build requires a compatible DNS-SD implementation
on that other platform. Direct IP remains available across platforms.

## Callback and peer lifetime

A shared DNS-SD connection is driven by a libuv poll handle. Registration,
browsing, resolution, address updates and destruction all execute on the
existing libuv thread. Shutdown stops the poll, removes its owner pointer,
cancels child operations and then releases the shared connection. Only the
poll allocation survives until libuv's close callback; it cannot call back
into the destroyed emulator.

Discovery keeps a bounded set of service instances. Departed or mismatched
peers invalidate their cached entries without shifting indices used by socket
requests. New searches choose the first valid peer rather than assuming slot
zero is live. The virtual address from TXT avoids a blocking query merely to
identify a discovered peer. Registration/browse failures can be retried on a
subsequent guest search, including after the user changes local network access.

## Validation

The native Bonjour integration test exercises real system registration and
browsing: matching rooms, exclusion of this instance and other rooms, the
advertised query port, departure, and immediate cancellation. It passed both
the normal build and an AddressSanitizer/UndefinedBehaviorSanitizer build.
The test is explicitly tagged because it requires local network discovery.

Release simulator and signed-device builds succeeded. The simulator regression
suite passed all twelve checks. The macOS Qt app built successfully, and its
packaged Info.plist contains the Bonjour and local-network declarations.

On the physical Air, the user selected Local Network and reopened Dragon World.
The Mac then observed the Air's Bonjour advertisement. With the simulator in
As Server, the Air's As Client automatically connected and entered the map.
The user confirmed the result, and the simulator screenshot showed the live
two-player desert stage with both dragons, projectiles and changing scores.
Neither device's captured log contained Bonjour errors, guest panics, access
violations, graphics halts or the earlier `-57` socket-error stream. The signed
Air build contains no multicast entitlement.
