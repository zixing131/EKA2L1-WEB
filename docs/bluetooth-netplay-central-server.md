# Proxy-server Bluetooth netplay never worked, and how to run your own matching server

## Symptom

EKA2L1's Bluetooth netplay has three discovery modes: direct IP, LAN broadcast,
and "proxy server", where every emulator keeps a TCP connection to a central
matching server (`btnetplay.12z1.com:27138` by default) that introduces players
to each other. Direct IP and LAN work. Proxy server finds nothing — a guest
Bluetooth device search just times out with an empty list, on every device and
every game.

## Narrowing it down

Standing up a private matching server made the client side easy to observe,
because the server logs exactly what each client says and can be made to answer
with a known-good packet.

The protocol itself is tiny (`btmidman_proxserv_matching.cpp`):

```
client -> server   0x09 <len> <password>   log in / join a room
                   0x04                     list the other players in my room
                   0x0A                     log out
server -> client   0x05 <count> <entry>*
                   entry := 0x01 <ipv4:4> | 0x00 <ipv6:16>   network byte order
```

Two bugs, both on the emulator side:

**Login was never sent.** `send_login()` had exactly one caller: the
`uvw::error_event` handler installed before `connect()`. On the happy path the
connect handler only installed the data callback and started reading, so the
server never learned the client's password and could not put it in a room. The
error-handler call was not a retry either — the first thing `send_login()` does
is replace that handler with one that only logs.

**The player list was parsed from the wrong offset.** `read_and_add_friend()`
takes the packet base pointer and a cursor, advances the cursor past the address
family byte, and then reads the address itself from `buf` — the start of the
packet — instead of `buf + buf_pointer`. Every friend therefore decoded to the
same four bytes: opcode, count, family flag, and the first byte of the real
address. Even with a correct server reply, the emulator went on to send its
name/address queries to a garbage IP.

The cursor was also a plain `char`, which goes negative past 127 bytes — seven
IPv6 entries are enough — and the reply length was never checked against the
entry sizes.

Nothing in the repo's history suggests this path was ever exercised: the file has
not been touched since 2023, and the upstream default host resolves to
Cloudflare's proxy addresses, which do not forward TCP 27138 at all.

## The upstream server speaks a protocol no client has used since 2023

[`EKA2L1/btnet-server`](https://github.com/EKA2L1/btnet-server) does exist — a
90-line Node script, last touched 2022-08-25 — and it implements a completely
different wire protocol from the one above:

```
client -> server (TCP)   'l' '0' <len> <password>    log in
                         'l' '1'                      log out
                         'c' 'r'                      call for players
server -> peer (UDP 35689, not the TCP socket!)
                         'c' 'r' <'0'|'1'> <len> <requester address as ASCII>
peer   -> requester (UDP 35689)
                         'c' 'a'                      call accepted
```

That is the emulator's *own* protocol as of commit `1493e8af` (2022-08-25), and
`btnet-server`'s last commit is the same day. Then `86b9244d` (2023-03-14)
rewrote the client onto the binary opcode protocol and nobody touched the server
again. So proxy mode has been broken in two independent ways at once: the client
and the server have not spoken the same language for years, and the client-side
code that would have parsed a correct reply never worked either.

The two designs also differ in direction. The old one pushes: the server
forwards the requester's address to every other room member over UDP, and each
member answers the requester directly. The current one pulls: the requester gets
a list back on its own TCP connection and contacts the peers itself.

Push looks like it should help with NAT, but it does not — the server's push
goes to the peer's harbour port, which is exactly the port that has to be
forwarded anyway, so both designs need every player reachable on UDP 35689. And
pull is enough for a real session even though only the scanning side learns
about the other: the host registers the joiner from the inbound connection in
`btinet_socket`'s accept path (`add_or_update_friend`), so it never needs to
have discovered it.

There was therefore nothing to port back from the upstream server. Two of its
details are worth recording anyway: it also refuses to report a peer whose
address equals the requester's, which confirms that rule is intended behaviour
rather than an invention; and it keys room membership on `socket.remoteAddress`,
so two clients behind one public address overwrite each other's room entry and
either one logging out evicts both — key on the connection instead.

## Fix

- `setup_proxy_server_discovery()` sends the login from the `connect_event`
  handler, and the pre-connect error handler just logs.
- `read_and_add_friend()` reads from `buf + buf_pointer`, takes `nread` and
  bounds-checks the entry against it, uses a 64-bit cursor, and zeroes
  `real_addr_` first — `add_friend()` de-duplicates friends with `memcmp` over
  the whole `saddress`, so the uninitialised tail of an IPv6 address made every
  scan add the same peer again.
- `handle_matching_server_msg()` rejects replies shorter than the two byte
  header.

## Running a matching server

The server is a rendezvous point and nothing else: it reports the public address
each client connected from to the other clients that used the same password, and
carries no game traffic. Everything after that — name queries, virtual Bluetooth
address queries, L2CAP/RFCOMM payloads — is direct UDP between the peers, on
harbour port 35689 plus the virtual port range at `btnet-port-offset`. Peers
therefore still need UPnP or manual port forwarding; the server cannot relay for
them.

Two details worth keeping in any reimplementation, both forced by the client:

- Cap replies at 10 entries *and* 127 bytes. Older builds still have the 8-bit
  cursor, and `MAX_INET_DEVICE_AROUND` is 10.
- Never report a peer whose address equals the requester's. Both would want
  harbour port 35689 on the same public address, so they cannot reach each other
  anyway, and a client handed its own address calls itself.

The client prefers the AAAA record — `setup_proxy_server_discovery()` walks the
`getaddrinfo()` results for the first non-`AI_V4MAPPED` `AF_INET6` entry and
never falls back if the connection fails — so a stale AAAA on the server's
hostname breaks discovery for everyone even when the A record is fine.
