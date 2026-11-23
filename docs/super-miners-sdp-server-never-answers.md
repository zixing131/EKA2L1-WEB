# Super Miners finds no Bluetooth players, because the peer's SDP server never answers

## Symptom

Two emulator instances on one host, both on the 5320 (rm-409, Symbian 9.3), both
running Super Miners, both configured for direct-IP Bluetooth netplay pointing at
each other. The joining side sits in "WAITING FOR SERVER…"; the hosting side runs
its device search and reports **NO DEVICES FOUND**. Nothing appears in either log:
no panic, no socket error, no Bluetooth warning.

## Narrowing it down

The inet Bluetooth midman turned out to be innocent, and proving that took probes,
because almost nothing on this path logs at all. With traces on both instances:

```
get_by_address flags=158              inquiry starts
get_friend_address idx=0 size=1       the configured peer is there
asker send req size=1 -> reply len=13 peer's virtual Bluetooth address
asker send req size=1 -> reply len=12 peer's device name
next_impl direct idx=1 got=false      no second friend
report_search_end                      EOF delivered to the guest
```

So the resolver *did* report the peer. The game then closed the host resolver and
opened a net database subsession (`ENDCreate`/`ENDQuery`) — an SDP service search,
which is how it decides whether a discovered device is actually running Super
Miners. That is where it stalled, and an empty service search is what produces
"NO DEVICES FOUND".

On the peer, the request arrives intact. `SdpServer[10009220]` binds virtual port
1, accepts the connection and reads the whole 27-byte ServiceSearchRequest:

```
02 00 01 00 16 35 11 1C 00000000-0000-1000-8000-0002EE0425B1 FF FF 00
```

…and then does nothing at all: no reply, just another `Recv` posted. Three
separate emulator defects stack up on that one read.

### 1. `ESoShutDown` is not in the 9.x socket-server table

When its client timer expired, the SDP server called `RSocket::Shutdown`, opcode
`0x1E`, and the socket server logged `Unimplemented socket opcode: 30` and
*completed nothing*. The request status stayed pending forever, so the connection
was never torn down either.

The pre-S^3 message table is not published anywhere, but it is recoverable: it runs
the same families in the same order as the 9.5 table in `esockserver/csock/SOCKMES.H`,
and EKA2L1 already serves guests with `ESoClose = 0x1D` and `ESoCancelRecv = 0x20`.
Two slots sit between them, and the EKA1 table (also a single contiguous run) fills
the same gap with Shutdown then CancelIoctl. So `0x1E` is `ESoShutDown`. In the 9.5
table it is `ESoShutdown = 53 = 0x35`, also missing. Both are dispatched now, and
the base `socket::shutdown` completes with `KErrNotSupported` instead of dropping
the request on the floor — an unimplemented backend must still answer.

### 2. `ESoRecv` and `ESoRecvNoLength` had their argument layouts swapped

With the shutdown answered, the peer got as far as closing the connection, but it
still never replied. The read itself was being decoded wrongly.

Dumping the raw IPC arguments of the SDP server's read settles the layout:

```
op=0xA a0=0x7010B8 a1=0x0 a2=0x7010A8 a3=1   package at a0 holds 0x01000000
```

`a0` is a descriptor, not a value, and it holds exactly `KSockReadContinuation` —
the flag the SDP server passes. So the pre-S^3 client sends
`TIpcArgs(&aLen /* flags inside */, 0, &aBuffer)` for `ESoRecv`, which is what the
"length package first" branch in `socket_socket::recv` already handled. But the
dispatch passed `has_return_length = false` for `ESoRecv` and `true` for
`ESoRecvNoLength` — exactly backwards, in both the legacy and the reformed table.
The pointer was therefore read as the flags word, so every `Recv` ran with a guest
heap address as its flags, and `aLen` was never written back.

The branch condition needed one more correction. The reworked client passes
`TIpcArgs(someFlags, &aLen, &aBuffer)`, and falls back to the package-first form
only for `RecvFrom`, where the address takes the second slot; the extra
`!one_or_more` term made a reformed plain `Recv` read its flags from the wrong
place too, and made `ESoRecvNoLength` fail outright with `KErrArgument`.

### 3. L2CAP reported bytes read where the datagram remainder belongs

Now the flags were right (`KSockReadContinuation`) and the read completed with the
full 27 bytes — and the SDP server *still* posted another read instead of parsing.

`es_sock.h` and the `RSocket::Recv` documentation are explicit: *for non-datagram
sockets `aLen` returns how much was read; for datagram sockets it returns the
number of remaining unread octets*. `CSdpConnection::HandleReadL` uses it exactly
that way — non-zero means "this L2CAP datagram is not complete, keep reading".
EKA2L1 wrote 27 there, so the server waited for 27 more bytes that were never
coming, timed out, shut down and closed.

L2CAP is datagram-interfaced, and `l2cap_inet_socket::receive` already hands over
everything that arrived, so nothing is ever left of the datagram: it now reports a
remainder of zero. `RecvOneOrMore` is documented the other way around and keeps the
byte count, which the socket can tell apart because that path is the only one that
arrives with `SOCKET_FLAG_DONT_WAIT_FULL` already set.

## Result

With all three fixed, the host's search lists `PLAYER1`, CONNECT reaches the shared
`MULTIPLAYER / START GAME` lobby on both sides, and both instances enter the same
live cooperative match with a synchronised map and countdown; moving one miner
shows up on the other screen. No panic, access violation or graphics halt in either
log.

The reformed path is unchanged in behaviour where it was already right: two X7
(rm-707) instances still discover each other in Bounce Boing Battle and reach
"Connection ready!".

## Notes for next time

- Nothing on the inet Bluetooth path logs above trace, and `spdlog` only flushes on
  debug and above, so a search that "produces no log output" tells you nothing. The
  cheap way in is a temporary `flush_on(trace)` plus explicit probes; the expensive
  way is believing the empty log.
- `log-filter: "*:trace"` on an instance that sits in a guest busy-wait writes tens
  of gigabytes in minutes. Filter to the classes you need
  (`Service.Bluetooth`, `Service.Esock`, `Service.Internet`).
- Logging the owning process name at `bind` time is what identified the responder as
  the ROM's `SdpServer[10009220]` rather than the game, which is what made Symbian's
  own `btsdp/server/protocol/listener.cpp` the authority on what the reply depended
  on.
