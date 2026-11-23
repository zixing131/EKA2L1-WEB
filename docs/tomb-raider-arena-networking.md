# Restoring Tomb Raider's original N-Gage Arena

Tomb Raider's N-Gage Arena is an early AirPlay service, not an HTTP API. The native game delegates its online screens to `GSBAPP.APP`, which sends IPC requests to `AIRPLAYSERVER.EXE`. The latter discovers a billing provider and exchanges reliable, little-endian messages over UDP port 41001. Later N-Gage HTTP libraries present on the same emulated drive do not describe this protocol.

The investigation separated emulator contracts from missing service infrastructure and modifications in the installed game. A sibling `ngage-arena` project implements the private service, the replacement billing provider, and narrowly versioned client repairs. EKA2L1 contains only general configuration and EKA1 service fixes.

## EKA1 service contracts

The N-Gage ROM's `sysagt.dll` constructs `RSystemAgent::NotifyOnCondition` as two requests: synchronous opcode 8 supplies a count in slot 0 and a descriptor containing 12-byte UID/state/type records in slot 1; asynchronous opcode 4 then waits for the conditions. Leaving opcode 8 uncompleted stalled client initialization. The S60 1.2 SDK declarations and the ROM's `sysagx.exe` agree on the layout. Conditions are combined with AND; comparison types 32–35 mean equal, not equal, supplied state greater than current state, and supplied state less than current state. Unknown properties do not satisfy a condition.

The existing event queue also returned before registering a nonempty notification and discarded unrelated subscribers after any event. It now registers one pending notification, keeps unmatched subscriptions, clears a completed condition set, and removes session-owned entries during teardown. The server clears sessions before its queue is destroyed. Any-event notifications use the same queue.

Old-architecture host resolver opcode 0x25 was already identified by the protocol enumeration but absent from dispatch. Routing it through `get_by_name` lets the native `RHostResolver` request reach the Internet resolver.

The EKA1 `RSocket::SendTo` and `RecvFrom` request block stores the address of a `TSockAddr` descriptor, not the address of its payload. Treating it as a raw socket address read descriptor metadata as the address family and wrote received address bytes over the descriptor header. Both paths now unwrap the descriptor and check its capacity before passing the payload to the socket backend. Ordinary send/receive calls without an address keep their existing path.

## DNS overrides

`config.yml` now supports a general `hosts` mapping from complete DNS names to numeric IP addresses. Matching ignores ASCII case and a final dot. It does not match arbitrary suffixes or redirect unrelated services. The resolver preserves the original guest name in its result and uses numeric-only resolution for configured values. IPv4 and IPv6 still follow the family requested by the guest.

The resolver also resets saved `addrinfo` and iteration pointers after freeing a previous lookup. Otherwise a failed subsequent lookup could leave a stale pointer for iteration or destruction.

## Native client and service findings

The installed, recognized GSB executable had its Arena entry replaced with an immediate return. Restoring that function prologue enabled its existing online UI. This belongs in the separate installation helper, not in the emulator's application launcher or input handling.

The original billing provider depends on a retired subscription flow. A small S60 SDK DLL supplies the same ordinal and GCC 2.x virtual-table interface for the local service. It also creates the missing first Modem row expected by AirPlay before it queries ETel. It performs no cellular billing or SMS operation.

A second client defect appeared when submitting a nonempty race taunt before ever opening Messages. The caller owned a valid message cache, but the network client only retained that cache pointer in its inbox-download path. Its sent-message path then truncated the cache file and dereferenced null while saving it. The installation helper's version-checked repair retains the supplied cache before adding and saving the outgoing message. An already empty cache left by the failed attempt must be backed up and removed while the emulator is stopped.

Directory replies contain packed records, not textual menus. The native challenge page requires two records with decimal rank strings: opponent and current player. The result page separately consumes a result row and an outcome row. Empty rank strings trigger a client assertion. Incoming challenge messages and the welcome message have different type bytes.

Director's Cut recordings and Shadow Race recordings are distinct formats. Clips contain a compressed 12,684-byte game snapshot, input streams, and optional camera streams. Race files carry a course header, checkpoints, a checksummed completion time, random seeds, and input streams without that snapshot. The game replays downloaded race input to verify its time before allowing a challenge. A Home-level race is invalid because its level lacks the save-crystal model used for checkpoints.

An abandoned or lost challenge may upload the opponent's downloaded recording. The server must not credit that file as the losing player's personal record. Scores and messages are committed once per downloaded challenge, with duplicate final uploads returning the original result. The private service's +10/-5 league policy is explicitly local; Nokia's original backend rules and official downloadable assets are unavailable.

Native validation included clip recording and camera preview, upload/download, a calibrated Caves race, two local identities, a 33.766-second recording challenged with a 6.126-second winning run, and a 2.490-second revenge run. The native result screens showed +10 for a win, -5 for a loss and cumulative score 5 after the revenge. Incoming messages and temporary completed-month trophy fixtures were also inspected; the fixtures were removed afterward.

The guide directory downloads full recordings into the native player, including pause, rewind and fast-forward. A recording was also uploaded through the game’s Strategy guide → Caves category and downloaded from that level’s guide directory. A separate live mentor character and a native offline library for reopening downloaded guides after a restart described in historical accounts have not been established in this recognized game revision. The client has an unused type-2223 path whose filename is literally `NOT USED !`, and the original installation lacked the optional location-to-guide `adverts.dat` table. An authored Caves room-0 table subsequently enabled the native STRATEGY icon and direct guide-directory request from within the level; that path also entered the same full-recording player. These findings do not justify inventing an additional replay mode in the emulator. Official walkthrough assets and original backend data are not recovered by this work.

The final emulator source matches commit `efb24fc9a`. Its Release simulator build passed the default regression with 12 passes and zero failures; screenshots and guest logs were reviewed. `ekatests` completed with 5,907 assertions in 232 test cases. The independent private service passed all 24 tests. The original device identity, account cookie, saved games and logging settings were restored after the two-account validation; the requested hosts mapping remains enabled.

[Upstream PR #707](https://github.com/EKA2L1/EKA2L1/pull/707) contains the nine shared code and test changes as `a8f332659`, with identical changed-file content. Its [CI run](https://github.com/EKA2L1/EKA2L1/actions/runs/34707125029) passed Android, Windows/Linux/macOS desktop, iOS Release device, dyncom differential and sanitizer checks. The private service, client repairs and this investigation document are outside the upstream patch.

A downloaded guide also continued to rewind, restart, play and pause with the local UDP service stopped. This verifies local playback within the loaded session, not an offline browsing or reopening workflow after restarting the game.

A subsequent cold launch with the service stopped retained the downloaded `current.i3d`, but Options offered no clip browser and Arena entered its connection screen before any directory access. The recognized game's clip loader has one direct caller, which selects either the Arena download or the built-in demo; the filename-prefix parsing in that caller does not establish another launch interface. The remaining offline gap concerns reopening saved content, not persistence of the downloaded file. A historical downloaded guide or another game revision is still needed to establish the unresolved mentor and offline access contracts.
