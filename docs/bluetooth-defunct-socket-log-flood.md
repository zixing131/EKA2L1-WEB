# The Bluetooth queries socket that filled the disk

## Symptom

A user's `EKA2L1.log` on a physical device had grown to several gigabytes made up
almost entirely of one repeated line:

```
E .../services/src/bluetooth/protocols/btmidman_inet.cpp:130 [Service.Bluetooth]: Error on the Bluetooth queries socket! Libuv error code=-57
```

Two logs were pulled off the device to look at. The numbers alone say this is not
a logging problem:

| log | total lines | error lines |
|---|---|---|
| `EKA2L1.log` | 579,991 | 573,200 |
| `EKA2L1_TakeThis.log` | 996,623 | 498,042 (+498,042 for a second socket) |

In the second one the last line that is *not* the error is #538. Every line after
it, to the end of the file, is the error repeating. The emulator never logged
anything again — it was wedged, not merely noisy.

## Narrowing it down

`-57` is `UV_ENOTCONN` (Darwin `ENOTCONN == 57`).

The `TakeThis` log was the more useful of the two, because there *two* independent
UDP sockets — the Bluetooth queries socket and the LAN discovery listener — start
failing in the same instant and then alternate perfectly. Two unrelated sockets
dying together is not a bug in either of them; it is something happening to the
whole process.

The user then supplied the line that settles it, from a newer log:

```
E .../bluetooth/protocols/bonjour.cpp:71 [Service.Bluetooth]: Bonjour receive failed: -65569
```

`dns_sd.h` names that value directly:

```c
kDNSServiceErr_DefunctConnection = -65569,  /* Connection to daemon returned a SO_ISDEFUNCT error result */
```

`SO_ISDEFUNCT`. iOS marks every socket owned by a suspended app as **defunct**;
XNU's `soreceive()` returns `ENOTCONN` for the rest of that socket's life, and the
defunct socket carries `SS_CANTRCVMORE` so kqueue reports it readable forever.
Both errors are the same event seen through two APIs. The line right before the
flood in that log is a CenRep `changes saved`, i.e. the app was on its way to the
background.

### Why a dead socket becomes a hang

The defunct socket explains the error, not the half-million repetitions. That
comes from a difference inside libuv that is easy to miss:

* `uv__read()` (streams) calls `uv__io_stop()` on any error but `EAGAIN`. A dead
  TCP socket takes itself out of the loop.
* `uv__udp_recvmsg()` delivers the error and **leaves the read watcher armed**.

So for UDP, a listener that only logs and returns is a busy loop: the fd is
level-triggered readable, `uv__io_poll` returns immediately with a zero timeout
every iteration, and each iteration writes one line. The loop thread runs at
`thread_priority_very_high` and the file logger flushes per line, so the emulator
threads are starved out entirely. That is the wedge.

Three listeners had this shape — the queries socket, the LAN discovery listener,
and the asker. The asker was worse than the others: it also re-invoked its
requester's failure callback on every spin.

`bonjour.cpp` had already got this right (`uv_poll_stop()` on a receive error plus
a `failed` flag), which is exactly why its error appears once and the UDP ones
appear half a million times. The contrast was the fastest way to see the defect.

## Fix

Two parts, both in [PR #691](https://github.com/EKA2L1/EKA2L1/pull/691).

**Stop a UDP handle whose error says the socket is gone.** `is_socket_dead_error()`
in `common_inet.h` classifies the codes (`EBADF`, `EINVAL`, `ENETDOWN`, `ENOTCONN`,
`ENOTSOCK`, `EPIPE`); the three listeners call `handle.stop()` instead of returning
into another spin. The asker fails its request once and rebuilds the socket on the
next request — it cannot close the handle from inside its own listener, since uvw
stores exactly one listener per event type and destroying the handle would destroy
the callable currently executing.

**`midman::suspend()` / `resume()`.** Rather than only surviving defunct sockets,
avoid creating them: a frontend that can be suspended drops its sockets before the
suspension and builds fresh ones on return. Default implementations are no-ops, so
Qt and Android are unchanged.

Some care in the details:

- iOS drives it from `scenePhase`, and only from `.background`. `.inactive` fires
  for a control-centre pull or a call banner, which must not drop a live netplay
  search.
- Neither call waits for the session lock on main or for the loop thread.
  The bridge posts transitions to the control queue; socket setup and shutdown
  each finish in one loop task. System boot inherits the desired host state.
  [The lifecycle review](./netplay-suspension-lifetime.md) explains the lock cycle
  and connection-state ownership constraints.
- The asker is left out of the suspend path. Closing it would strand a synchronous
  requester waiting on a completion that can no longer arrive — the same hazard
  `asker_inet`'s destructor already documents about the kernel lock.
- `hearing_timeout_timer_` is kept across a suspension. It is not a socket, so it
  stays valid, and a pending stranger search keeps the timeout that guarantees its
  observer eventually gets `on_no_more_strangers()`.

## Verifying it without being able to reproduce defunct

The simulator cannot produce a defunct socket — the app is an ordinary macOS
process there, and nothing in userspace can set `SOF_DEFUNCT`. The spin fix is
therefore established from the libuv and uvw sources rather than experimentally.

The suspend/resume half *is* directly testable, and `lsof` on the app process
turns out to be a clean black-box oracle:

| | LAN (mode 2) | Direct IP (mode 1) |
|---|---|---|
| foreground | 1 socket on `:35689` | 1 socket on `:35689` |
| background | 0 | 0 |
| foreground again | 1 | 1 |

Stable over repeated cycles with a constant fd count. The useful part is the
absence of `EADDRINUSE` on the rebind: the socket can only bind the same port
again if the suspend genuinely closed the previous one, so a silent no-op in
`suspend()` would have failed this test loudly.

## Dead ends and things worth not repeating

- **The first log's timeline is misleading.** In `EKA2L1.log` the flood starts
  immediately after `Thread dragonworld forcefully killed`, which reads like a
  teardown race. It is not — that log just happened to catch the transition at a
  different moment. The `TakeThis` log, where the flood starts mid-session with two
  sockets at once, is the one that points at a process-wide cause. When two logs
  disagree about *when* something starts, prefer the one with more than one
  affected component.
- **The absence of other log lines is evidence, not an artifact.** It is tempting
  to read "nothing else in the log" as truncation. Here it was the actual symptom:
  a very-high-priority thread spinning with a flushing logger starves everything
  else.
- Guest-facing UDP in `internet/protocols/socket.cpp` does *not* have this bug —
  `handle_udp_delivery()` calls `uv_udp_recv_stop()` first thing. Worth checking
  before widening the fix; the same is true of every TCP path, for the libuv reason
  above.
