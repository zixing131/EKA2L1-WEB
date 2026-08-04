# The debug write command rejected every self-patching guest, so cracked S60v2 titles quit at startup

## Symptom

Nokia 6680 (rm-36, `epoc80`) → *Sky Force Reloaded*.

Picking the game from the app list flashes the launcher and drops straight back to it. Nothing
appears on screen, and the log shows no panic and no access violation — the guest thread is killed
with exit type `kill`, category `None`, reason `0`. That is a deliberate, graceful `RThread::Kill(0)`,
not a crash, which makes the failure look like the game itself deciding not to run.

Note that the same device also has a *Sky Force* (the first game) installed, and that one behaves
differently — it is a single monolithic `.app`, while *Reloaded* is a 1 KB `.app` stub plus a
348 KB `Force.dll`. Do not mix the two up when reproducing; only the `.app` with UID `0x1020D923`
is the EKA1 build of *Reloaded*, and there is an unrelated S60v3 `skyforcereloaded.exe`
(UID `0x2002517B`) on the same drive that the 6680 cannot launch at all.

## Narrowing down

### The DLL loads fine — the interesting log lines are buffered away

The first trace ends at `Try loading Force.dll ... failed` for the `c:` path and then stops dead,
which reads as if the loader wedged. It has not: the stub tries `c:\system\apps\SkyForceReloaded\`
first, then `e:\`, and the second attempt succeeds. The success line is a `LOG_TRACE`, and the file
sink only flushes at `LOG_DEBUG` and above, so `simctl terminate`'s SIGKILL throws away the tail of
the log. Temporarily raising the sink to `flush_on(trace)` is what makes the real sequence visible:

```
Loaded library: Force.dll
Unblanking screen in AKNCAP session stubbed
Trying to display dialog type: 27578 with message:
Thread skyforcereloaded forcefully killed with category: None and exit code: 0
```

So the engine loads, a note is raised, and the app exits. Chasing the loader further is wasted
effort.

### The note says what went wrong

`notenof.cpp` prints a nonsense type (27578) and an empty message because its EKA1 payload parse is
off. Dumping the raw 36-byte request shows the real content:

```
ba 6b 11 00 | 04 00 | 2c | "Memory full" ...
```

Type is the `04 00` at offset 4 — `note_type_error` — and `0x2c` at offset 6 is an 8-bit descriptor
header, `(11 << 2) | EBufC`, introducing the 11 characters of **"Memory full"**. That is AVKON's
text for `KErrNoMemory`, and apparc leaves with exactly that error when
`CApaApplication::NewApplication()` returns NULL. The heap is not actually exhausted — the process
gets a 32 MB maximum — so something is returning NULL on purpose.

### The stub is a crack, and its patch is not landing

Disassembling the 1 KB `.app` shows all it does is: `RLibrary::Load("Force.dll")` against `c:` then
`e:`, take the library's base (its entry point offset is 0, so `library_entry_point` returns the
base), build the constant `0xE12FFF1E` — `BX LR` — on the stack, write those four bytes over
`base + 0x11884`, and finally `Lookup(1)` and call it. `force.dll + 0x11884` disassembles to a
function prologue (`push {r4, r5, r6, r7, r8, sb, sl, lr}`), so the stub is neutralizing a check —
the binary is a cracked release, consistent with the `BCDFGHJKLMNPQRSTVWXYZ0123456789?` serial
alphabet and `tware`/`tdemo` strings inside the engine.

Reading guest memory at the kill point settles it: the word at `force.dll + 0x11884` is still
`0xe92d47f0`, the original `push`. The patch never applied, the protection function ran, it failed,
`NewApplication()` returned NULL, and apparc turned that into "Memory full".

### The write is rejected on a bad bound

An SVC trace names the mechanism — a single `SVC 0xc00097 debug_command_execute` right after
`library_entry_point`. Its arguments are correct (`addr=0xe0211884`, `len=4`, `write=true`), and
`debug_command_do_read_write` neither logs an error nor reaches the actual store. It returns at

```cpp
if (is_write) {
    if (len > static_cast<std::int32_t>(buf->get_length())) {
        return epoc::error_overflow;   // no log on this path
    }
}
```

The descriptor the guest hands over is a `TPtr8` built with the two-argument constructor: raw words
`20000000 00000004 0040f070`, i.e. type `EPtr`, **length 0**, max length 4, pointing at the four
patch bytes. The byte count for the transfer comes from the command's separate length argument, not
from the descriptor, so checking it against the descriptor's *current length* rejects every caller
that passes a plain output-style buffer. Since the length is zero, the check can never pass.

## Fix

Bound both directions by the descriptor's capacity instead. `get_max_length()` already returns the
length for the constant descriptor types that have no separate maximum, so one check covers reads
and writes and stays correct for a `TDesC8` source.

A four-byte write also has to invalidate the CPU's instruction cache. The general path already
called `imb_range()` after writing, but the `len == 4` fast path returned early and skipped it —
and four bytes is exactly one ARM instruction, which is the whole point of this command. The write
branch now falls through to the same flush; the four-byte read still returns early.

With both in place the patch lands, the game boots to its language selection menu and runs.

Two things left alone deliberately: the EKA1 note payload parse in `notenof.cpp` is wrong (it reads
the type at offset 0 and the text at offset 3, and the text is narrow rather than the descriptor it
actually is), and *Sky Force Reloaded* renders with badly wrong colours and scaling on this device.
Neither is caused by this change.
