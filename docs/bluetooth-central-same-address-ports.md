# Central netplay collapsed same-source peers onto one UDP endpoint

## Symptom

Two emulators could join the same central-server room, but games either found no
peer or contacted themselves when both TCP connections had the same public IP.
This is common for two devices behind one NAT and unavoidable when two simulators
share the host's tunnel. Different `internet-bluetooth-port` values did not help:
proxy discovery discarded the configured value and always assigned UDP 35689 to
every returned address.

## Narrowing it down

The matching-server logs proved that both clients logged in and that a player-list
request returned a peer. The next UDP step did not reach the other emulator. A
live Explode Arena session made the missing discriminator clear: the two processes
already used distinct discovery ports and virtual port offsets, but the old wire
entry contained only an address. Once both TCP peers appeared as the same address,
the receiver had no way to select the other discovery socket.

Simply disabling the server's same-address filter was a useful diagnostic but not
a fix. It handed both clients the same `address:35689` endpoint. Rewriting that
address to loopback made a same-host test pass, but is also unsuitable for normal
deployment because two separate machines behind one NAT must receive the public
address, not their own loopback address.

## Fix

The binary matching protocol now has a backward-compatible port extension. After
login, a current server sends a two-byte capability message. A current emulator
then advertises its configured discovery port. Player-list entries for an extended
client use new IPv4/IPv6 entry tags and append that port in network byte order;
legacy clients and servers retain the old address-only entries and fixed port.

Proxy discovery no longer overwrites `internet-bluetooth-port`, and iOS exposes
the listen-port setting in central mode. The TCP receive side buffers and frames
capability and player-list messages so TCP fragmentation or coalescing cannot lose
a list reply.

The matching server can therefore distinguish same-public-IP peers by
`address:port`. A same-address override remains strictly a same-host test tool;
production can allow same-source peers without rewriting their address.

## Dragon World exposed discovery ordering in the device notifier

Dragon World still returned immediately from its Client role even after the
extended endpoint was correct. Unlike the other games, it opens the standard
Bluetooth device-selection notifier before it performs a host-resolver inquiry.
The notifier only inspected the already-cached friend list, so central mode had
not yet sent `GET_PLAYERS` and synchronously returned `KErrNotFound`.

The notifier now participates in inet discovery when its cache is empty. It waits
for the matching-server/LAN search, refreshes the selected peer's virtual Bluetooth
address, then completes the original asynchronous notifier request. Cancellation
unregisters the observer and invalidates queued callbacks; callback state is shared
independently of the plugin lifetime so ROM or notifier-server teardown cannot run
a queued host callback through a destroyed plugin. The notifier server also now
forwards `RNotifier::CancelNotifier(TUid)` to the selected plugin instead of merely
completing the cancel IPC.
