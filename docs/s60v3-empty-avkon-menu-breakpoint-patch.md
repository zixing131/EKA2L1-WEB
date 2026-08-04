# S60v3 empty Avkon menu breakpoint patch

## Symptom

On the Nokia 5320 / RM-409 and Nokia N95 8GB / RM-320 firmware, pressing the
left soft key on 7Days' first screen terminated the guest with
`E32USER-CBase 21`. The same soft key opened Calculator's Options menu normally,
so this was not an input mapping or host-side menu problem.

## Diagnosis

7Days' application resource declares no menu bar for that screen. Tracing the
guest failure into the S60v3 `eikcoctl.dll` builds showed that
`CEikMenuBar::StartDisplayingMenuBarL` obtains the menu-title array count,
subtracts one, and calls the array accessor even when the count was zero. The
accessor therefore receives index `-1` and raises the CBase array panic.

Later public Symbian Avkon source checks the resulting title index before doing
the lookup and exits menu display when it is negative. That establishes the
compatibility behavior: an empty menu request should clean up and return, not be
turned into a host-side special case for 7Days.

The extracted Z-drive DLL was useful for disassembly, but its file begins with a
0x78-byte ROM image header. `codeseg` points at the mapped code address after that
header, so the breakpoint address and mapped-image XXH32 fingerprint must be
calculated from the runtime code view. Confusing those two coordinate systems
initially produced the wrong fingerprint.

RM-409 and RM-320 contain the same faulty behavior at different image addresses,
and their cleanup blocks are not identical. A raw address therefore cannot cover
both devices. Conversely, hashing the complete DLL makes a hook unnecessarily
firmware-specific: unrelated changes elsewhere in `eikcoctl.dll` invalidate it
even when the target method is unchanged.

RM-320 also loads `eikcoctl.dll` before the scripting manager registers built-in
patches. A purely deferred hook therefore never sees a later codeseg-loaded
event, even with the right address and hash.

An early instruction layout reused the existing cleanup block by removing its
null check. It fixed the empty menu but made Calculator fault when closing a real
menu, because its CBA can already be null on the normal cleanup path. Calculator
was therefore an essential control, not just a smoke test.

Moving the fix to the scripting breakpoint engine exposed three engine defects
that its original app-only patches did not exercise. First, dyncom translated the
Thumb `BKPT` instruction (`0xBE00`) to `SVC 0`, so the breakpoint callback never
ran. Second, dyncom reports CPSR.T clear inside that callback; using CPSR to infer
the displaced instruction size therefore reinstalled an ARM breakpoint over
Thumb code. Third, ROM code is physically shared between processes, while the
engine saved and restored every preimage by process UID. A second process could
capture an already-installed breakpoint as its supposed original instruction.

Calculator then exposed one more dyncom contract bug: after the scripting hook
stopped the core, the `BKPT` handler still advanced PC past the displaced
instruction. The following one-instruction step therefore skipped the original
Avkon call and eventually corrupted Calculator's menu-close path.

## Fix

The scripting manager now has a ROM-export breakpoint primitive. It locates the
method through its stable Symbian export ordinal, defines that method's code span
as its entry through the next higher export (or the end of the code segment), and
computes XXH32 only over that span. A breakpoint is installed at a method-relative
offset only when the DLL UID3 and method fingerprint both match. Image relocation
and unrelated changes elsewhere in the DLL no longer require a new patch entry,
while an unknown implementation is still rejected rather than patched by an
unsafe byte search.

For these builds, `CEikMenuBar::StartDisplayingMenuBarL` is export 70. The known
method fingerprints are `0x39A36149` (RM-409) and `0x1595EE13` (RM-320), with
breakpoint offsets `0x182` and `0x17E`. At the array-accessor call after
`Count()-1`, the native callback checks that explicit index in `r1` and leaves
the normal path untouched. When it is negative, it clears `r7` and moves the
guest PC by a build-specific relative offset into the method's existing
null-safe cleanup block. No application UID or title-specific condition is
involved. A matching Lua patch keeps desktop and Android behavior in sync with
the native-only iOS patch.

The breakpoint engine treats resolved ROM hooks as shared: it registers one
hook, keeps one true preimage, and does not restore it on every process switch.
The original instruction is restored only for the normal single-step performed
after a hit. Export-based registration resolves an already-loaded ROM codeseg
without depending on a process attachment and validates its UID and local method
hash before delegating to the normal shared-ROM breakpoint path.

Dyncom now translates Thumb `BKPT` to the ARM breakpoint form understood by its
internal decoder. Breakpoint dispatch takes the authoritative Thumb bit from the
registered hook, rather than dyncom's temporary CPSR state. It also restores the
original instruction PC before invoking the callback, so callbacks that change
PC keep their final resume address. The latter also makes the existing Warhammer
40K skip callback work as intended. The dyncom handler synchronizes CPSR before
raising the breakpoint exception and, when the callback stops the core, exits
without advancing PC so the displaced instruction is actually single-stepped.

ROM codeseg hashing was also corrected to hash mapped ROM bytes instead of the
null heap buffer used only by RAM-loaded code segments.

## Verification

On both the RM-409 and RM-320 simulators, 7Days remained on its first screen
after repeated left-soft-key presses with no guest panic, fatal error, or access
violation. Calculator still opened its populated Options menu with the left soft
key and closed it with the right soft key.
