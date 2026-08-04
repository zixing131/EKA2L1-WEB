# A guest's undefined STRD calls host address 0 (TestFlight 260812)

## Symptom

TestFlight build 260812 (26.7.0) died on an iPhone 18,4 about 21 minutes into a
session:

```
Exception Type:  EXC_BAD_ACCESS (SIGKILL)
Exception Subtype: KERN_PROTECTION_FAILURE at 0x0000000000000000
Termination Reason: CODESIGNING 2 Invalid Page
Triggered by Thread:  4
```

`CODESIGNING 2 Invalid Page` reads like a signing or JIT-permission problem, and
that is the first dead end worth skipping: it is simply how the kernel classifies
an *instruction* fetch from an unmapped page. `esr: 0x82000006 (Instruction
Abort) Translation fault` with `pc: 0x0` says the process branched to address 0.
Nothing about entitlements, nothing about the (off by default) dynarmic JIT.

The crashing thread is the emulation thread:

```
0   ???                                   0x0
1   EKA2L1  InterpreterMainLoop(ARMul_State*, unsigned int&) + 22456
2   EKA2L1  eka2l1::system_impl::loop()
3   EKA2L1  -[EKA2L1Emulator startWithDocumentsPath:]::$_5::operator()()
```

## Narrowing down

The dSYM from the matching CI run (`dwarfdump --uuid` → exact match on
`16CB6A51-209B-3095-B2FA-07CEB876AB9F`) does not carry line information for the
`cpu` target, so `atos` only resolves symbol + offset and `llvm-dwarfdump
--lookup` finds no compile unit at all. The register dump turned out to be more
informative than the line number would have been.

`InterpreterMainLoop` is one enormous function, so `lr` inside it is not much of
a clue on its own — but the crash happened *at the call*, with the AAPCS64
argument registers still intact:

| Register | Value | Meaning |
|---|---|---|
| `x0` | `0x117400000` | `ARMul_State *cpu` |
| `x1` | `0x80a3eef8` | `unsigned int inst` |
| `x2` | `0x16be1e8dc` | `unsigned int &virt_addr` (on the crashing thread's stack) |
| `x9` | `MLnSRegisterOffset + 0` | a comparison constant still live in a register |

Three arguments in that shape, plus `x9` holding the address of
`MLnSRegisterOffset`, pin the call site exactly: the fallback branch of
`MLS_GET_ADDR` in `arm_dyncom_interpreter.cpp`, which compares
`inst_cream->get_addr` against the two inlined fast forms and otherwise calls
through the pointer. The pointer was null.

`x1` is the offending guest instruction, so it can be decoded directly.
`0x80a3eef8`:

- `cond = HI`, bits 27..25 = `000`, bits 7..4 = `0xF` → the extra load/store
  space; the decode table matches `strd` (`arm_dyncom_dec.cpp`).
- `P = 0`, `U = 1`, `I = 0`, `W = 1`, `L = 0`, `Rn = r3`, `Rt = r14`, `Rm = r8`.

`GetAddressingOp()` dispatches the misc load/store family on
`BITS(inst, 24, 27)` and `BITS(inst, 21, 22)`. For `BITS(24,27) == 0`
(post-indexed) it only handles `BITS(21,22) == 2` and `== 0`. This instruction
has `BITS(21,22) == 1`, i.e. post-indexed *with* write-back, so the function
falls off the end and returns `nullptr`. `INTERPRETER_TRANSLATE(strd)` stores
that null into the instruction cream, and `STRD_INST` — after the `HI` condition
passed — branched to it.

The encoding itself is nonsense: `P = 0` together with `W = 1` is UNPREDICTABLE
for extra load/store, and STRD with `Rt = r14` would use `r15` as the second
register. No compiler emits this. The guest had run off into data and was
executing it as code.

## Fix

The emulator must not turn a bad guest instruction into a host process death.
Both `LS_GET_ADDR` and `MLS_GET_ADDR` now check the pointer before the indirect
call and, when it is null, jump to a shared `UNDEFINED_ADDRESSING_MODE` label
that logs the instruction and PC, saves flags and raises
`exception_type_undefined_inst`. That lands in
`kernel_system::cpu_exception_handler`, which dumps the guest context and kills
the offending thread with `KERN-EXEC 3` — the same treatment any other undefined
instruction gets, and the host survives.

`LS_GET_ADDR` shares the guard because `GetAddressingOp()` has the same kind of
hole on the plain load/store side: the `ldr`/`str` decode patterns do not exclude
`bit4 == 1`, so a register-form encoding in the ARMv6 media space reaches
`GetAddressingOp()` and also returns `nullptr`. `GetAddressingOpLoadStoreT()`
returns `nullptr` by design for the Thumb-only variants, and its callers
(`ldrt`/`strt`/`ldrbt`/`strbt`) go through `LS_GET_ADDR` too.

This is upstream (Citra-derived) behaviour, not iOS-specific; the crash simply
shows up as a hard process kill on iOS instead of a debugger stop.

## Left open

Why the guest branched into data is *not* answered by this crash report — it
carries no guest PC, process or thread name, and the crash log alone cannot say
which app was running. The new `LOG_ERROR` prints the instruction and the guest
PC, and the exception path dumps the full guest context, so a recurrence is now
self-diagnosing from `EKA2L1.log`. Worth keeping in mind for that follow-up: the
dyncom translation cache is a bump allocator kept across processes and tagged by
ASID, so stale-block and code-page-reuse bugs are a plausible source of a runaway
guest PC.

## Related

- `docs/ios-guest-pc0-deadloop.md` — the mirror image, guest PC reaching 0.
- `docs/ios-angrybirds-exit-null-callback.md` — a guest-side null function
  pointer call rather than a host-side one.
