# Bluetooth Join outlives its query object, then indexes an empty friend list

## Symptom

On the N-Gage ROM, High Seize consistently terminated the iOS app after choosing
Multiplayer game, Bluetooth, then Join a game. The emulator log ended around L2CAP
socket setup without a panic or access-violation diagnostic.

Attaching LLDB changed the apparently single failure into three consecutive ones as
the earlier faults were removed. The first stopped on the libuv loop in
`asker_inet::send_request_with_retries` while dereferencing a null translated address.
After that was fixed, an empty friend list aborted in `std::vector::operator[]`. An
initial attempt to make handle teardown synchronous stopped both crashes but froze the
whole app while closing a Bluetooth socket.

The useful dead end was treating the null translated address as malformed guest input.
The same address passed validation immediately before the task was posted. At the
fault, LLDB showed that the task's captured `asker_inet` had an all-zero stored address,
which pointed to lifetime rather than descriptor or address conversion behavior.

## Root cause

`asker_inet` reused a `libuv::task` whose callback captured a raw `this`. uvlooper
implements a posted task with a separate `uv_async_t`, so releasing the owner's
`shared_ptr<task>` does not cancel an already posted callback, and uv_async callbacks
are not ordered against a later one-shot teardown callback. High Seize closes its
short-lived resolver/socket while the query is still queued, allowing the task and the
UDP/timer listeners to run against the destroyed asker.

The no-peer path then exposed a separate bounds check in
`midman_inet::get_friend_address`: it used `&&` between `index >= friends_.size()` and
`friends_[index].real_addr_.family_ == 0`. For an empty vector it evaluated the second
operand and indexed element zero instead of returning false.

Finally, waiting for libuv handle teardown inside `asker_inet::~asker_inet` inverted
the emulator's lock order. A synchronous `RSocket::Close` runs on the Symbian thread
while holding the kernel mutex. The shared libuv loop can simultaneously be inside an
ordinary inet receive callback waiting for that mutex. The Symbian thread waiting for
the loop therefore formed a permanent cycle with the loop waiting for the kernel.

## Fix

The asker now validates and translates the destination before posting work, so the
queued task does not revisit mutable guest-address state. Its task and UDP/timer
listeners share an atomic lifetime token and return without touching the raw owner once
destruction begins. The destructor transfers ownership of its uvw handles to a loop
cleanup closure and returns immediately instead of waiting while the kernel may be
locked. The cleanup closure resets listeners, closes the handles, and releases the
final references on the correct thread.

The friend lookup now uses a short-circuiting `||` bounds/family check, making an empty
peer list complete the inquiry normally.

With a Debug simulator build under LLDB, the original High Seize Join path was repeated
after each fault was found. The final build remained responsive in the no-game result
and could return to the previous menu without an exception or lockup.
