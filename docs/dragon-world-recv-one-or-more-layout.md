# Dragon World stalls at the difficulty screen in netplay

Two-player Dragon World (`0x102735C5`, RM-84) got as far as the difficulty menu
and stopped. Both sides drew the menu, the highlight kept pulsing, and every key
was ignored — on the server *and* on the client, so it was not a case of one side
waiting for the other's choice.

## It was not the input path

The obvious suspicion was a key mapping: the game had accepted OK on the title
screen and on three menus before this one, but maybe "confirm" is a different key
here. It is not. Pressing down at the difficulty screen and comparing per-line
brightness across screenshots shows the pulsing line never changes, so the
selection does not move either. The game renders at 28-30 FPS the whole time —
it is running, it is simply refusing input while it waits for something.

That something is on the wire. `lsof` showed the three expected TCP connections
established, and the emulator log was repeating

```
socket.cpp:1034 Receive data with non-zero flags, please notice! (flag=7822116)
```

about thirteen times a second. 7822116 is 0x775CA4 — a guest heap address, not a
flags word. That line is easy to dismiss as upstream noise, and it had been
present for the whole session; it is in fact the bug announcing itself.

## What the guest actually asks for

Probes on `socket_socket::send/recv/read/write` showed a strict alternation:
the game writes four bytes, then posts a receive, forever. The payload arrives
correctly — the peer's four bytes land in the buffer and the receive completes —
so the link is fine.

The receive is opcode 12 (`socket_so_recv_one_or_more`) with

```
arg0 = 0x775B24   arg1 = 0   arg2 = <buffer descriptor>
```

`arg1` being zero rules out the S^3 layout `TIpcArgs(flags, &aLen, &aBuffer)`:
`RecvOneOrMore` with a length always passes a real `TSockXfrLength` pointer.
The pre-rework client packs the length package first and the flags second, which
fits: `arg0` is the package, `arg1` is a flags value that happens to be 0.

`socket_socket::recv` picked its argument layout from `one_or_more` rather than
from the esock generation:

```cpp
if (has_return_length && (!one_or_more || has_addr)) { ... }
```

so this request fell into the S^3 branch, took `0x775B24` as the flags, and
looked for the length package in `arg1` — which is the integer 0, so
`get_descriptor_argument_ptr(1)` returned null and `size_return` stayed null.
Every receive therefore completed with the data in place but **the guest's
`TSockXfrLength` never written**. Dragon World trusts that value, read zero
bytes every time, discarded the peer's message, and sat in its handshake
forever with input gated off.

The fix is to key the branch off the esock generation instead — the same
`epoc95` boundary the opcode dispatcher already uses to pick between the
reworked and the pre-rework opcode tables. `send()` already treats the first
slot as a package-carrying-flags for every variant, which is the same
convention; `recv()`'s `one_or_more` exclusion was the odd one out.

That boundary covers more than this device: every pre-`epoc95` non-oldarch
device (S60v3, S60v5) now takes the same branch for `RecvOneOrMore`. Only
epoc81a was verified directly; the rest rest on the existing "s60v5 and down"
comment describing the same convention, and the regression suite has no other
socket user to confirm it.

With that, the server advances past the difficulty screen to map selection, and
picking a map drops both machines into the same live match: identical terrain
and enemies, both dragons visible on both screens, 1P/2P score and health bars,
movement on one side mirrored on the other.

## Notes for reproducing

Both simulators need the same device — RM-84, not RM-36; the 6680 ROM is not
installed on every container, and a `-LaunchROMCode` that is missing fails with
"Emulator failed to initialise" rather than falling back.

Screen updates lag screenshots noticeably here: a menu transition can take five
to ten seconds to appear in `simctl io screenshot`, which makes a working
keypress look like it was dropped. Wait before concluding an input did nothing.
The map screen wants the number key rather than OK, and a 0.12s tap on it is
sometimes swallowed where a 0.5s press is not.

Menus have no visible cursor box; the selected line is the one whose brightness
changes between consecutive screenshots, so `magick ... -format '%[fx:mean]'`
over each line's band identifies it without guessing.
