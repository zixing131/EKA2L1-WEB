# Rebuilding discovery without blocking the host lifecycle

The defunct-socket fix needs more than closing and reopening file descriptors.
Reviewing the paths around that operation exposed three independent correctness
problems: a main-thread lock cycle, transient errors being treated as permanent,
and TCP reassembly state surviving the connection it belonged to.

The SwiftUI scene callback runs on main. Acquiring `session_mutex` there can wait
for a launch or device boot, which holds that mutex while issuing synchronous
graphics commands. The graphics thread may in turn dispatch layer work
synchronously to main. Making only the later libuv call asynchronous does not
break this cycle. Lifecycle notifications now record the desired state without
walking the emulator and enqueue their application on the control queue. Each
transition is retained, including a background/foreground pair that arrives while
a boot is busy. A boot also applies the latest desired state to its new midman,
so a system created while already backgrounded does not miss the earlier event.
The facade owns the session mutex, and each emulator state borrows it. A queued
notification can therefore lock before checking whether shutdown has removed the
state, without dereferencing a mutex inside an object that may already be gone.

The shared error listener receives both send and receive errors. `ENETDOWN`
describes an unavailable network interface, and `EINVAL` can describe an invalid
operation, rather than an unusable socket. Neither justifies permanently stopping
discovery. They are excluded from the fatal-socket classification; invalid handle
and disconnected-socket errors still stop UDP reception to break the busy loop.
This matters on desktop and Android as well: those frontends do not invoke the
iOS lifecycle recovery path when their network returns.

The proxy receive buffer is owned by one TCP connection. Retaining an incomplete
player list when closing that connection makes a new connection's first response
look like the missing tail of the old packet. Closing discovery now clears that
buffer. It retains the search timeout, whose ownership is the outstanding search,
not the socket.

All discovery setup and state transitions execute on the libuv thread. The LAN
and proxy setup routines must finish in the same task, rather than posting a
second task that can run after a following suspend or destruction has closed the
handle. Proxy logout also runs on the loop thread, avoiding concurrent access to
the shared pointer being reset by suspension.

The focused host tests use a loopback UDP query and an in-process IPv6 proxy.
Errors and TCP fragments are injected at uvw's event boundary, so the tests can
exercise the production listeners without relying on macOS to create an iOS-only
defunct socket or on TCP choosing a particular segmentation. The partial-reply
fixture advertises 127.0.0.1:4660 after reconnection, and checks the observed peer,
not just the presence of a rebound socket.

Removing each correction separately made the focused checks fail: restoring the
fatal classification for `ENETDOWN` stopped the UDP listener, and retaining the
proxy buffer produced port 32512 instead of 4660. On a Release simulator build,
holding the session mutex from another thread let the asynchronous lifecycle
calls return in about 12 microseconds; restoring their blocking lock made the
same calls wait for the holder's five-second safety timeout. These measurements
test responsiveness under contention, not emulator performance.
