# N-Gage Store re-entry jumps to an unmapped function pointer

On the 5320/RM-409, opening the N-Gage Launcher Store, moving one tab to the
left, and returning to Store closed the Launcher and returned to EKA2L1's app
list. The native iOS process remained alive. This was therefore a guest failure,
not an iOS application crash.

## Narrowing the fault

The first useful failure was an instruction access violation at `0x000B9D10` in
the Launcher's `ngiplay` thread. Register capture placed the caller in
`NgiWebCore.dll`:

```text
pc  = 0x000B9D10
lr  = 0x74EB03DF
r0  = 0x0000002C
r1  = 0x000B9D11
r12 = 0x74EAFE83
```

Disassembly at the return address showed an indirect allocation call:

```text
74EB03D2  ldr  r1, [pc, ...]  ; address of a callback table in .data
74EB03DA  ldr  r1, [r1, #4]
74EB03DC  blx  r1
```

The target was not random. `NgiWebCore.dll` has link-time code base `0x8000`
and runtime code base `0x74E00000`. Relocating the raw Thumb address gives:

```text
0x000B9D11 + (0x74E00000 - 0x00008000) = 0x74EB1D11
```

The failed target was exactly the unrelocated form of a valid internal
`NgiWebCore.dll` function pointer.

Logging codeseg reference transitions explained why it appeared only after
switching tabs. Leaving Store unloads the browser stack, including
`NgiWebCore.dll`. Returning to Store reuses its cached process attachment.
The reuse path correctly restores the DLL's original `.data` bytes and clears
`.bss`, but then returns before the initial-attachment relocation loop. Every
code address stored in `.data` consequently reverts to its link-time value.

The nearby unimplemented AKNCAP opcode `0x49` is the keyboard repeat-rate
request. It happens before the fault but does not supply the invalid address and
is unrelated.

## Fix

Relocation is now a shared codeseg helper. Initial attachment still relocates
both code and data after imports are patched. When a cached attachment with
zero uses is revived, the loader restores `.data` and reapplies only its data
relocation entries. Retained code is deliberately not relocated a second time.

This fixes the loader contract rather than special-casing N-Gage or the Store:
any relocatable Symbian DLL whose static data contains code or data pointers now
survives detach and reattach with the same values it had after first load.
