# A merge dropped dyncom's breakpoint resume fix, and the Avkon patch died with it

## Symptom

On the 5320 (rm-409), Calculator opened its Options menu fine but died the moment
the right softkey dismissed it:

```
KERN-EXEC 3 — Thread Calculator
```

The regression suite caught it as two failures (`RSK closes Options menu`,
`no guest crash`). The kernel log showed an undefined instruction rather than a
bad memory access:

```
cpsr=0x20000000, cpu->TFlag=0, r15=0x814F5124
Undefined instruction encountered in thread Calculator
Last instruction: ldmdavs r8!, {r1, r6, r7, fp, sp, lr}  (0x683868c2)
```

`0x683868c2` is not a plausible instruction for Avkon to be running. Split as
Thumb it is two perfectly ordinary ones — `ldr r2, [r0, #12]` and
`ldr r0, [r7, #0]`. So the memory was fine; the CPU was decoding Thumb code in
ARM mode, and `cpu->TFlag=0` says so outright.

## Narrowing it down

Disabling the two `register_rom_export_breakpoint("eikcoctl.dll", 70, ...)`
entries in `builtin_patches.cpp` — the native mirror of
`s60v3_empty_avkon_menu_fix.lua` — turned the suite green, and re-enabling them
reproduced the crash every time.

The obvious next suspicion, that the patch callback jumps somewhere wrong, is
wrong. Two pieces of evidence:

- Replacing the callback body with a log line and nothing else — no register
  writes, no PC redirect — still crashed. Planting the hook is sufficient.
- That log line reported `r1 = 0x00000000` at the hook. The patch only acts when
  `(int32)R1 < 0`, so on this ROM it would never have done anything anyway.

The hook fired exactly once, at `0x814F511C`. The guest died at `0x814F5124`,
eight bytes later.

That put the fault in dyncom's `BKPT_INST` handler, which on this branch read:

```c
cpu->RaiseException(exception_type_breakpoint, cpu->Reg[15]);
LOAD_NZCVT;
if (cpu->Reg[15] != pc) goto DISPATCH;
cpu->Reg[15] += cpu->GetInstructionSize();
```

Two things go wrong there, and they compound. `LOAD_NZCVT` reloads `TFlag` from
`cpu->Cpsr`, but nothing flushed the live flags into `Cpsr` before raising —
the system call handler in the same file does exactly that (`SAVE_NZCVT;`
immediately before `RaiseSystemCall`). `TFlag` therefore comes back as 0 on a
Thumb breakpoint and `GetInstructionSize()` answers 4 instead of 2. And
`handle_breakpoint` restores the displaced instruction and puts PC back on the
breakpoint so the single step in `epoc.cpp` can re-execute it, which leaves
`Reg[15] == pc`, skips the `goto DISPATCH`, and lets the fall-through advance
past that very instruction. Two mis-steps of four bytes is the observed +8.

## The part that was not a discovery

All of the above had already been diagnosed and fixed, in this tree, on
2026-07-22, by `60921e509 fix(scripting): handle RM-409 empty Avkon menus` — the
same commit that introduced the Avkon patch that needs it. It added exactly two
things to `BKPT_INST`: the missing `SAVE_NZCVT;`, and a bail-out for the case
the handler has stopped the core:

```c
// A debugger or scripting hook may stop the core so it can restore and
// single-step the displaced instruction. In that case the breakpoint
// itself must not advance PC first.
if (cpu->NumInstrsToExecute == 0) {
    goto END;
}
```

`handle_breakpoint` opens with `running_core->stop()`, so this is always the
path taken for a scripting hook.

Both lines are still on `ios`. They are absent on `ios-next`, and the transition
is a merge: `8a32e8160 Merge branch 'master' into ios-next` (2026-08-20). Its
second parent is PR #604, `perf/dyncom-interpreter`, which branched from a point
*before* `60921e509` and rewrote the interpreter. The merge resolved the
`BKPT_INST` region in favour of the perf side and dropped both lines without a
conflict.

Restoring them makes the region byte-identical to `ios` again, and the suite goes
back to 12/12 with the Avkon patch enabled.

`upstream/master` never had the fix at all — `60921e509` was fork-only, while the
perf work was upstreamed — so every dyncom user is running the broken resume
path, and with it every shipped `scripts/*.lua` patch that hooks Thumb code.

## Dead ends worth avoiding

- Adding `SAVE_NZCVT;` on its own is not enough. It fixes the instruction-set
  half and *changes* the failure — the guest then dies further away with garbage
  registers — but the skipped-instruction half still derails execution. Seeing
  the crash move is not evidence of progress here.
- Do not start from the patch table. The offsets (`0xF4` / `0x12C`), the export
  ordinal and the method hash are all fine, and the hash check means a hook that
  resolves at all resolved onto the right bytes.
- `scripts::handle_breakpoint` already carries an `is_thumb` recovery that takes
  the mode bit from the registered hook address instead of trusting
  `get_cpsr()`. It addresses the same stale-`Cpsr` problem for the *address*
  only, and its presence makes it easy to assume the mode question is handled.
- Pickaxe (`git log -S`) will not find this. History simplification skips
  merges, so it reports only the commit that *added* the lines and stays silent
  about the merge that removed them. Walking `--first-parent` and sampling the
  file at each step is what actually located it.
