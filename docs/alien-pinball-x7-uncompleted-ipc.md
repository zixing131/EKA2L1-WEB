# Alien Pinball hangs on a black screen on the X7

Alien Pinball Ultimate launched on the X7 (rm-707) and never drew anything. The
FPS counter sat at 0 and the emulator used no CPU at all, which already rules out
a runaway guest: nothing was executing.

## The guest was deadlocked, not slow

A `sample` of the host process showed the Symbian OS thread parked in
`kernel_system::reschedule` → `thread_scheduler::switch_context` →
`common::event::wait`, and the timing thread waiting with no timeout. That pair
means the scheduler had no runnable thread *and* no pending timer: every guest
thread was blocked on a request that would never complete.

The emulator log ended mid-sentence, because panics and syscall traces are
`LOG_TRACE` and spdlog only flushes at `debug` and above. Attaching LLDB and
evaluating `(int)fflush(0)` releases the stdio buffer of a process that is stuck
and never will exit — much cheaper than rebuilding with a different flush level.

With `log-svc: true` and `log-filter: "*:trace"` the tail became readable:

```
Sending 14 sync to !Fontbitmapserver     EFbsMessBitmapCreate
Sending 54 sync to !Fontbitmapserver     EFbsMessBitmapClean
Sending 54 sync to !Fontbitmapserver     EFbsMessBitmapClean
Sending 74 sync to !Fontbitmapserver     ???
Unhandled FBScli opcode 0x4A
Calling SVC 0x800000 wait_for_any_request     <- forever
```

`fbscli::fetch`'s `default` branch logs the opcode and returns without completing
the message. Every FBS client call is a `SendReceive`, so the caller waits on that
request for the rest of the session -- which is the point: an unimplemented opcode
announces itself as a hang at a named opcode instead of degrading quietly.

## Opcode 74 is not in the public enum

`TFbsMessage` in the Symbian^3 FCL ends at 72 (`EFbsMessGetGlyphCacheMetrics`),
and EKA2L1's `fbs_opcode` matches it one for one. 73–75 exist only on Nokia's
firmware, so the definition had to come from the phone itself.

Extracting `z:\sys\bin\fbscli.dll` from the ROM is straightforward: search
`SYM.ROM` for `fbscli` encoded UTF-16LE, read the 10-byte `rom_entry` that
precedes the name (size, linear address, attributes, name length), and subtract
the ROM base from `iCodeAddress` in the `TRomImageHeader` at that address.
`arm-none-eabi-objdump -D -b binary -m arm` over the code then shows three call
sites loading an opcode above 72 into `r1` before the `RFbsSession::SendCommand`
veneer. Their addresses appear verbatim in the image's export directory, and the
public `FBSCLI2U.DEF` names those ordinals:

| Opcode | Ordinal | Function |
|---|---|---|
| 73 | 15 | `CFbsBitmap::SetDisplayMode(TDisplayMode)` |
| 74 | 16, 17 | `CFbsBitmap::SetSizeInTwips(...)` |
| 75 | 20 | `CFbsBitmap::SwapWidthAndHeight()` |

All three are header mutations that stock Symbian performs client-side, writing
straight into the shared bitmap; Nokia moved them into the server. Opcode 74
carries the bitmap handle in argument 0 and the twips width and height in
arguments 1 and 2, which is what the disassembly of both overloads builds into
its `TIpcArgs`.

## The next server along

Implementing opcode 74 moved the game past FBS and straight into the same shape
of failure in another server:

```
Calling service: !AppListServer, id: 56
Unimplemented applist opcode 0x38
Calling SVC 0x800000 wait_for_any_request     <- forever
```

Opcode 56 is `RApaLsSession::RegisterListPopulationCompleteObserver`. On a real
device the server completes it immediately when `iAppList.IsFirstScanComplete()`
is true and holds the message otherwise. EKA2L1 scans the registries before any
guest process runs, so the observer is always satisfied on registration; its
cancel counterpart completes too. Behind it came opcode 5,
`RApaLsSession::AppCount`, which answers with the count as its completion code
and leaves control panel items out of the list.

## Fix

* Implement FBS opcode 74 (`set_bitmap_size_in_twips`) and name 73/75 in the
  opcode enum so the next reader does not have to redo the disassembly. Both
  APIs behind 73 and 75 return `TInt`, so their callers can live without them
  until something asks for one.
* Implement applist opcodes 56, 57 and 5.

The unimplemented-opcode branches keep hanging on purpose: a deadlock at a named
opcode is how the next gap gets found, and completing it with `KErrNotSupported`
would only turn that into a silent misbehaviour further along.

The game now boots to its menu and plays.
