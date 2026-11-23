# Verifying Bluetooth netplay with two simulators on one host

Bounce Boing Battle reaches a live two-player match on the iOS simulator in both
direct IP and central-server discovery mode. Getting there took two wrong turns
that are worth writing down, because neither is visible from the emulator log.

## The freeze that was not a freeze

The joining side appeared to hang in "Choose opponent": the peer was listed by
name, but tapping the entry, the back arrow, and every other control did nothing,
and consecutive screenshots were pixel-identical while the host stayed
responsive. That looked exactly like a guest thread deadlocked on an unanswered
request, and it survived two rounds of probes:

```
get_by_address, in_completion=false      inquiry starts
next() current_friend=0                  peer found, completed with KErrNone
next() current_friend=1
report_search_end scheduling EOF, evt=32 timer armed (a valid event id)
delay-emu fired, request empty=false     EOF delivered to the guest
```

The host resolver was innocent all along. The real cause: the two simulators were
different models, so their windows are different sizes — 440x956 for the iPhone
17 Pro Max, 402x874 for the iPhone 16 Pro. Screenshots come back scaled to the
same 368x800 either way, so a coordinate computed from a screenshot and the
*other* device's window size lands somewhere plausible but wrong. Taps meant for
the list entry were landing in blank space below it.

What made this convincing rather than obviously wrong is that the earlier taps in
the same run did work: "Create game" and "Join game" are large enough that the
mis-scaled point still fell inside them. The tell was a swipe that happened to
cross the real entry position and immediately produced "Connection ready!".

Always read the frame per device before converting screenshot coordinates:

```sh
axe describe-ui --udid "$UDID" | jq -c '.[0].frame'
```

Press duration also matters somewhere in this game's UI, though less than the
coordinates did: on the "Play" button two short swipes and a plain tap did
nothing while `touch --down --up --delay 0.4` worked first time. Taps on the
larger "Create game" / "Join game" buttons were always fine once the coordinate
was right, so treat the long press as a fallback for a control that ignores a
correctly placed tap, not as a rule.

## Central mode cannot introduce two peers on one host

Discovery itself works — the matching server negotiates the port extension,
both emulators log in, and the player list correctly distinguishes same-source
peers by port:

```
type=2 <public ip>:35691
type=2 <public ip>:35689
```

But a peer address is whatever address the client's TCP connection came from, so
two emulators sharing one uplink are both advertised at the public IP, and
neither can reach it — there is no hairpin path back. Opening the UDP ports on
the server's firewall does not help either: the packets would arrive at the
server, which has no DNAT rule mapping them back to a particular client, and the
two clients want overlapping port ranges anyway.

The server's `BTNETPLAY_SAME_ADDRESS_OVERRIDE` exists for exactly this case. Set
it to `127.0.0.1` and same-address peers are advertised on loopback, keeping
their individually advertised discovery ports, which is enough for two emulators
on one machine to reach each other. It is a testing switch and must not stay set
for real traffic, where peers genuinely need the public address.

## LAN mode is structurally untestable this way

Two independent reasons, either one fatal:

- LAN discovery forces `HARBOUR_PORT` (35689) and `LAN_DISCOVERY_PORT` (35690)
  regardless of `internet-bluetooth-port`, so the second instance on the same
  host cannot bind them. It fails silently — `bind()`'s return value is dropped
  at the call site, so nothing appears in the log and only `lsof` shows it, as
  two sockets sitting at `*:*`.
- `handle_lan_discovery_receive` drops datagrams whose source matches
  `local_addr_` or loopback, so even a successful bind would filter the other
  instance's broadcast as its own.

Testing LAN needs two hosts with distinct addresses.

## A null dereference found on the way

`send_call_for_strangers` wrote to `matching_server_socket_` for every mode that
is not LAN, but only proxy-server mode ever creates that handle; the LAN branch
likewise assumed `lan_discovery_call_listener_socket_` exists even though
`setup_lan_discovery` returns early when it cannot find a local interface.
Reaching either one requires an observer to start a search in direct IP mode,
which happens when the device-selection notifier opens before the resolver has
cached a peer — the Dragon World ordering. Bounce populates the cache first, so
this never fired during these runs.

Both branches are now guarded, and the timeout is armed unconditionally: an
observer that never receives `on_no_more_strangers()` leaves its guest request
outstanding forever, which would be a genuine hang.

## Direct IP could not answer the device-selection notifier at all

Dragon World picks its peer through the standard Bluetooth device-selection
notifier before it ever opens a socket, so it exercises a path Bounce never
reaches. In direct IP mode the notifier returned `KErrNotFound` every time and
the game bounced straight back to its role menu.

`on_no_more_strangers` short-circuited to a failed request whenever no stranger
had been reported during the search. That test is wrong for direct IP: its peers
come from the config list and are never announced through the observer, so a
silent search there is normal and the friend's virtual Bluetooth address simply
has not been resolved yet. Letting the refresh decide instead — it is a no-op
when there really is no peer — makes the notifier return the configured peer,
after which Dragon World connects and both sides reach the synchronized
difficulty screen.

The same commit guards the two null socket handles described above; reaching
them requires precisely this notifier-before-resolver ordering in a mode that
never creates a matching-server socket.
