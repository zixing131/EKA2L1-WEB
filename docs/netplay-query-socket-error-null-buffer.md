# Netplay query handlers parse the error path as if it were a datagram

## Symptom

TestFlight build 260839 (commit `a19e6eb87`), iPhone 17 Pro, iOS 26.5.2. The app was
launched at 16:49 and died at 17:02 — 13 minutes in, with `Role: Non UI` in the report,
i.e. it had been sitting in the background. Thread 5 (the libuv loop thread):

```
EXC_BAD_ACCESS (SIGSEGV), KERN_INVALID_ADDRESS at 0x0

Thread 5 Crashed:
0  EKA2L1  midman_inet::handle_queries_request(sockaddr const*, char const*, long long) + 40
1  EKA2L1  uvw::udp_handle::recv_callback(uv_udp_s*, long, uv_buf_t const*, sockaddr const*, unsigned int) + 412
2  EKA2L1  uv__udp_io + 476
3  EKA2L1  uv__io_poll + 980
4  EKA2L1  uv_run + 1604
```

The register state pins it down without needing a dSYM:

```
x1: 0x0                 <- buf
x20: 0x114fd0f00        <- this
x21: 0xffffffffffffffc7 <- nread == -57 (ENOTCONN)
```

`+ 40` is the function prologue plus the first load, and `handle_queries_request` opens
with `*reinterpret_cast<const std::uint32_t*>(buf)`.

## Root cause

`uvw` publishes a `udp_data_event` for a successful receive and an `error_event` when
`nread < 0`. The netplay code wired the error handler straight into the datagram parser:

```cpp
bluetooth_queries_server_socket_->on<uvw::error_event>([this](const uvw::error_event &event, uvw::udp_handle &handle) {
    handle_queries_request(nullptr, nullptr, event.code());
});
```

`handle_queries_request` never looked at `nread` and never null-checked `buf` or
`sender`, so *any* receive error on that socket was a guaranteed null dereference. The
error code carried in `nread` was -57 (`ENOTCONN`), which is what iOS hands back once a
backgrounded app's sockets have been torn down — matching the `Role: Non UI` line and
the 13-minute gap between launch and crash.

`btmidman_lan_matching.cpp` had the identical wiring into
`handle_lan_discovery_receive`, which dereferences `addr` on its very first line and
`buf[0]` shortly after. Only the mode in use decides which of the two fires first.

Both parsers were also reading fixed offsets out of the datagram with no length check at
all — `buf[5..8]` for the port query, `buf[1]` plus a `memcmp` of `buf[1]` bytes for the
LAN password compare. These sockets listen on the LAN (and, in proxy mode, take traffic
relayed from a public server), so a short or hostile datagram over-reads.

## Fix

The two `error_event` handlers now log the libuv error code instead of pretending an
error is a datagram. Independently, both parsers reject null `buf`/`sender`/`addr` and
validate `nread` before touching the payload — `handle_queries_request` computes the
per-opcode payload size it needs on top of the 5-byte header, and
`handle_lan_discovery_receive` bounds the password length byte against what actually
arrived. The unknown-opcode diagnostic moved ahead of the length check so an unrecognised
opcode still reports as such rather than as a truncated packet.

Verified by firing malformed datagrams at ports 35689/35690 of a running simulator build
(empty, 1-byte, header-only, truncated-payload, unknown-opcode, and an over-long LAN
password length): all are rejected with a log line, the process survives, and a
well-formed `GET_NAME` query still answers `<asker id> 'd' 0x06 "EKA2L1"`.
