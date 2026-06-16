# dyncom interpreter — deep optimization plan

Follow-up to [`ios_snakes_perf.md`](./ios_snakes_perf.md). After the ASID
instruction-cache fix and the simulator render-scale cap, Snakes gameplay is
**CPU-bound on the dyncom interpreter** (guest os_thread ~99% in
`InterpreterMainLoop`, graphics thread ~80% idle). This plan targets the
interpreter itself.

## Constraints (non-negotiable)
- **No JIT.** iOS forbids W^X / JIT for this app, so dynarmic and 12L1R are out
  of scope here regardless of the A32 bug. This is purely about making the
  *interpreter* faster. (JIT is therefore NOT the ceiling-breaker it would be on
  other platforms — the interpreter is what ships.)
- **Cross-platform.** dyncom is shared with Android and desktop. No iOS-only
  forks of the interpreter; changes must keep all frontends correct.
- **Correctness first.** A wrong translation = silent guest corruption across
  every app. Every stage gates on `scripts/ios_regression_test.sh` (8/8) + a
  differential/fuzz check (below) before landing.

## Baseline hotspot map (Snakes gameplay, `sample` 6 s, os_thread = 3746)
| share | bucket | detail |
|---|---|---|
| ~78% | ARM interpretation | dispatch + inline handlers (~2115), `AddWithCarry` 123, `DataProcessingOperands*` ~290, addressing/`LdnStM*` ~320, `CondPassed` 61 |
| ~12% | guest memory | `ReadMemory32` 161 / `WriteMemory32` 112 / `ReadCode` 90 / `WriteMemory8` 35 + mmu page-walk lambda 64 |
| ~9.5% | present `wait_for` | not dyncom — the reverted double-buffer's target |

## How the interpreter works today (for grounding)
- Basic blocks are decoded once into a "cream" stream (`arm_inst` header +
  per-op struct) in a 128 MB bump buffer, cached by `(asid<<32)|vpc`
  (`instruction_cache`, after the recent fix).
- Execution is a computed-goto threaded dispatch (`GOTO_NEXT_INST` → label
  table). Each handler reads its pre-decoded cream, executes, `INC_PC`,
  `FETCH_INST`.
- **Every taken branch ends the block with `goto DISPATCH`**, which does a
  `std::unordered_map<u64,size_t>::find()` to locate the target block. There is
  **no block linking** — every branch pays a hash lookup.
- Data-processing ops compute their shifter operand through an indirect call
  `inst_cream->shtop_func(...)`.
- Data load/store use a 512-entry direct-mapped TLB fast path
  (`tlb::lookup`, `armstate.cpp`); **`ReadCode` bypasses it** and page-walks.

## Staged plan (ordered by impact ÷ risk)

### Stage 1 — memory fast-paths (low risk, ~3-5%)
- **1a. `ReadCode` via TLB.** `tlb_entry` already carries an unused
  `execute_addr` slot populated with `prot_exec`. Add a `lookup_exec()` and use
  it in `ARMul_State::ReadCode` (armstate.cpp:264) instead of always calling
  `read_code`. Fetches that hit a warm code page skip the page-directory walk.
- **1b. Inline the load/store fast path.** `ReadMemory32`/`WriteMemory32` are
  out-of-line calls invoked from every LDR/STR/LDM/STM handler; each re-fetches
  `core->mem_cache()` and the `InBigEndianMode()` branch. Inline a
  `tlb_read32/tlb_write32` fast path (lookup → deref) at the handler sites,
  falling back to the existing function on miss. Removes call overhead on the
  hottest accessors. Keep the big-endian path in the slow fallback (guest is LE).
- Risk: low. TLB semantics unchanged; only adds a hit path. Verify the exec
  lookup respects `make_dirty`/`flush` exactly like read/write.

### Stage 2 — block chaining (medium risk, the big structural win)
Eliminate the per-branch hash lookup. When `DISPATCH` resolves a branch target
to `ptr`, cache it on the originating branch so the next execution jumps
directly.
- Add to `arm_inst` (or the branch creams `bbl_inst`/`bx_inst`/…): a
  `std::size_t linked_block` + `std::uint32_t linked_vpc` + a
  `std::uint32_t link_generation`.
- In `DISPATCH`, after computing the target key, if the *incoming* branch's
  `linked_generation == current_generation` and `linked_vpc == Reg[15]`, set
  `ptr = linked_block` and skip the `find()`. Otherwise do the lookup and record
  the link.
- **Invalidation via a generation counter:** bump `cache_generation` on every
  full flush (overflow reset, `imb_range`, embedded-interpreter
  `clear_instruction_cache`). Any link with a stale generation is ignored →
  re-resolved. This makes chaining correct across SMC, DLL reload, buffer
  overflow, and the embedded-fallback clears, without walking/patching links.
- Indirect/return branches (`INDIRECT_BRANCH`, `bx`, `ldm pc`) have a dynamic
  target → cache only helps when the target repeats (e.g. tight loops, hot
  callees). Store last-target + verify `linked_vpc == Reg[15]` so a changed
  target just falls back to `find()`. Direct branches (`DIRECT_BRANCH`) have a
  fixed target → always chain after first resolve.
- Expected: branches are very frequent in a tight game loop; removing the hash
  probe per taken branch is the single largest interpreter-internal win.
- Risk: medium — correctness hinges entirely on the generation counter covering
  every cache-mutation path. Heavy tests on SMC / process-switch / overflow.

### Stage 3 — ALU/flag micro-opts (low-medium risk, ~3-5%)
- **3a. `AddWithCarry`** (123 samples): replace the manual carry/overflow bit
  math with `__builtin_add_overflow` / `__builtin_sub_overflow` (clang/gcc
  intrinsics, both supported) for carry+overflow in one op. Mechanical, easy to
  differential-test against the old implementation exhaustively.
- **3b. (defer) Lazy NZCV flags.** Storing result+operands and computing flags
  only when a conditional reads them is the classic interpreter win but is a
  large, error-prone rework of flag semantics (MSR/MRS, SPSR, shifter carry).
  High risk; revisit only if Stage 1-3a aren't enough.

### Stage 4 — shifter-operand specialization (medium, optional)
The `shtop_func` indirect call per data-processing instruction defeats branch
prediction. Specialize the common handlers (immediate, LSL-by-imm, register)
into dedicated labels that inline the shift, leaving the indirect path only for
rare register-specified shifts. Medium gain, adds handler labels.

### Stage 5 — instruction fusion / superinstructions (medium-high, structural)
Since JIT is off the table, the way to attack the ~78% dispatch share (beyond
block chaining) is to **execute more work per dispatch**. At translation time
(`InterpreterTranslateBlock`) detect common adjacent patterns and emit one fused
cream + handler, halving the `GOTO_NEXT_INST`/`FETCH_INST`/`INC_PC` overhead for
that pair. High-value candidates for the Symbian (ARMv5/v6 + Thumb) guest:
- `CMP/SUBS Rn,#imm` + conditional `B` → fused compare-and-branch (the dominant
  loop/condition idiom).
- `SUBS Rn,#1` + `BNE` → loop-counter decrement-and-branch.
- address-calc + access: `ADD Rd,Rn,#imm` (or shifted reg) immediately consumed
  by `LDR/STR [Rd]`.
- back-to-back `LDR`/`STR` and `MOV`+use pairs.
- Thumb 16-bit pairs benefit most (dispatch cost is a larger fraction there).

Rules: only fuse *within* a basic block; never fuse if the first op's flags are
consumed by something other than the fused branch, or if the `S` bit makes the
intermediate state observable; a jump that lands on the second op's PC simply
misses the (start-PC-keyed) cache and re-translates a fresh block from there, so
fusion can't be entered mid-pair → no correctness hazard. Start with the two
compare/sub-and-branch fusions (biggest, cleanest), measure, then widen.
- Risk: medium-high — fused flag/branch semantics must exactly match running the
  two ops separately; lean hard on the differential/fuzz harness.

## Realistic ceiling (no JIT)
Stages 1-5 target the ~12% memory + the ~78% dispatch (block chaining + fusion)
+ ~5% ALU. The interpreter dispatch+execute is the hard floor, but chaining and
fusion meaningfully shrink the dispatch share. A plausible net is **~20-30% CPU
reduction** → Snakes ~30 → ~38-45 FPS on the simulator, with more headroom on
device (no software-blit tax). Diminishing returns past that — the interpreter
is the product, so squeeze dispatch (Stages 2 + 5) hardest.

## Verification strategy (per stage)
1. `scripts/ios_regression_test.sh` → 8/8 (Final Battle 90 s in-game +
   Calculator input/menus, no guest panic).
2. Re-`sample` Snakes gameplay; confirm the targeted hotspot actually dropped
   and nothing new appeared.
3. **Differential correctness** for the risky stages (2, 3): reuse the existing
   CPU test harness (`src/emu/cpu/src/12l1r/tests/`, `src/tests/`) and/or the
   12L1R fuzz path (`FLAG_ENABLE_FUZZ` runs interpreter alongside JIT and
   compares state) to diff old-vs-new interpreter register/memory/flag state
   over randomized instruction streams — with explicit cases for SMC,
   cross-page branches, process switches, and cache overflow (the block-chaining
   invalidation paths).
4. Land stages independently (each its own commit) so any regression bisects
   cleanly; keep block chaining behind a compile/runtime switch until proven.

## Suggested order
1 → 3a (quick, safe, bankable) → 2 block chaining → 5 instruction fusion (the two
dispatch-share wins, with the test harness in place) → 4 shifter specialization
(if still short).

## Status (2026-06-14)
Landed and committed: **Stage 1a** (ReadCode via TLB), **Stage 3a** (inline
AddWithCarry), **Stage 2** (direct-mapped L1 in front of the block map). Snakes
gameplay went from a steady ~30-32 to a steady ~34 FPS; regression 8/8 throughout.
ReadCode self-time dropped ~90→~22 and AddWithCarry left the profile; the block-L1
gain was within noise (the per-block lookup was already amortised) but is correct
and not harmful.

**Stage 5 (instruction fusion) and Stage 4 (shifter specialization) deferred** —
see "Harness-gated work" below.

### Batched execution-quantum check — investigated, rejected (unsafe)
A tempting "free" win: `GOTO_NEXT_INST` runs `if (num_instrs >=
cpu->NumInstrsToExecute) goto END` on **every** instruction; moving it to block
boundaries (DISPATCH) would drop a load+compare+branch per instruction. **It is
not safe.** That same check is how a thread is stopped *immediately* after a
blocking syscall: the `SWI_INST` handler (interpreter ~L3829) runs the HLE, which
for a blocking SVC calls `prepare_reschedule → dyncom_core::stop()` →
`NumInstrsToExecute = 0`; with the PC unchanged the handler falls through to
`FETCH_INST; GOTO_NEXT_INST`, and only the per-instruction check there exits
before the just-blocked thread executes past its `WaitForRequest`. Polling at
block granularity would let a blocked thread run extra instructions — a
correctness break in the reschedule path, which is the emulator's most fragile
area (past stray-signal panics live here). The only safe residue (keeping the
counter in a host local to drop the per-instruction memory RMW, while leaving the
check itself per-instruction) is ~1% and still brushes the SVC path — not worth
it. **Not done.**

---

## Harness-gated work (lazy flags · inline mem · shifter specialization · fusion)
Everything bankable without a test harness is now done. The remaining
interpreter wins all change instruction *semantics* or *dispatch shape* in ways
the regression script (real ROM execution) can catch only opportunistically, not
exhaustively. They should be built **behind a differential interpreter test
harness**, in this order once it exists:

1. **Inline memory fast-path** (~3-5%, lowest risk of the four). Inline the TLB
   hit path of `ReadMemory32`/`WriteMemory32`/`ReadMemory8/16` into the LDR/STR/
   LDM/STM handlers (they are out-of-line calls today, ~273 samples). Mechanical;
   the harness just confirms loads/stores are bit-identical incl. unaligned,
   big-endian, and TLB-miss fallback.
2. **Shifter-operand specialization** (~2-4%). Replace the per-data-processing
   `shtop_func` indirect call with dedicated handlers for the common shifters
   (immediate, LSL/LSR/ASR by immediate, plain register), leaving the indirect
   path for register-specified shifts. Harness covers every shifter form incl.
   carry-out and the `Rs==15`/`Rn==15` PC cases.
3. **Instruction fusion / superinstructions** (~3-5%). CMP/CMN/SUBS+conditional-B
   and SUBS+BNE first. Needs a synthesized dispatch-table/InstLabel entry + a
   fused cream + handler, and look-ahead in `InterpreterTranslateBlock`. Harness
   must cover the fused flag+branch equivalence and the "jump lands on the 2nd
   op" (start-PC-keyed cache re-translates from there) case.
4. **Lazy condition flags** (QEMU `cc_op` style, biggest *potential* but
   highest risk and uncertain payoff). Defer NZCV computation to first read.
   Diluted for ARM (predication reads flags often) and flags are already
   denormalised (`NFlag/ZFlag/CFlag/VFlag` separate u32 "for speed"). Only
   attempt with the harness fully trusted; covers every S-bit op, `MSR/MRS`,
   `SPSR`, shifter carry-out, and conditional execution.

### Differential test harness — landing plan
Goal: prove an optimized interpreter is **bit-identical** to the reference over
large, edge-case-heavy instruction streams — the verification the regression
script can't give for shared CPU code.

> **Status: Phase 1 landed** (`test(cpu): add dyncom interpreter differential
> test harness`). `src/emu/cpu/src/dyncom/tests/dyncom_difftest.cpp` +
> `scripts/cpu_difftest.sh` build a standalone host tool (CMake options
> `EKA2L1_CPU_DYNCOM_ONLY` + `EKA2L1_BUILD_DYNCOM_DIFFTEST`, both default OFF) that
> runs randomized ARM **data-processing** instructions through dyncom and checks
> against an independent golden ALU model (16 opcodes + barrel-shifter carry-out,
> independent 64-bit add-with-carry) **and** a second dyncom instance (self-A/B),
> plus a negative control. Passes 200k–500k random cases. iOS/Android/desktop
> builds unchanged.
>
> **Phase 2 landed** (`difftest harness phase 2 -- load/store + memory diff`):
> single data transfers (LDR/STR/LDRB/STRB, immediate offset, pre/post-index,
> up/down, writeback) with `Rn` constrained into a mapped data window; each case
> diffs registers **and** the window bytes vs the golden + self-A/B. The
> load/store comparison was confirmed live by injecting a deliberate golden bug.
> Unlocks verifying the inline memory fast-path.
>
> **Phase 3 landed** (`difftest harness phase 3 -- register-specified shifts`):
> the golden barrel shifter + generator now cover register-specified shifts
> (`Rs`-amount, LSL/LSR/ASR/ROR incl. the >=32 carry-out rules), so every ARM
> data-processing operand2 form is exercised. Passes 600k cases. Unlocks shifter
> specialization + lazy flags.
>
> **Remaining phases:**
> - *Phase 3b — edge corpus:* hand-written footguns (`Rn/Rm == 15`, condition
>   boundaries, RRX, mode/Thumb, register/scaled load-store offsets, LDM/STM,
>   halfword/signed transfers).
> - *Phase 4 — instruction streams + self-A/B toggle:* random basic blocks ending
>   in a backward branch (loops), run for a fixed budget; add runtime flags to
>   toggle the block-L1 / fusion so A=opt, B=no-opt must match. Unlocks fusion and
>   re-validates block chaining/L1.
> - *Phase 5 — trace replay (optional):* capture a real Snakes/FBattle
>   instruction+state trace and replay through A/B for real-workload coverage.
>
> Note: the data-processing + load/store golden already retroactively validates
> the committed Stage 1a/2/3a micro-opts (600k cases exercise ReadCode, the block
> L1 and the inlined AddWithCarry with zero divergence).

### First harness-gated optimization landed: inline memory fast-path
`perf(dyncom): inline the TLB-hit memory fast path`. ReadMemory8/16/32/64 +
WriteMemory8/16/32/64 split into an inline fast path (TLB hit) in `armstate.h`
(cached `mem_cache_` TLB pointer → no circular include) and an out-of-line
`...Slow` path (miss/fault/BE/log). Names unchanged → interpreter handlers
untouched. **Verified** by the harness (600k data-proc + load/store cases incl.
memory diff, PASS) + `ios_regression_test.sh` 8/8; Snakes profile shows
ReadMemory32 self-time ~161→~7, WriteMemory32 ~112→~2 (call overhead gone).

### Shifter-operand specialization — tried, measured neutral, REVERTED
Inlined the common shifter forms (immediate/register/LSL-imm/LSR-imm) into a
`compute_shifter_operand` dispatcher to drop the `shtop_func` indirect call.
Harness-verified correct (800k cases, all shifter forms). But the compiler kept
`compute_shifter_operand` out-of-line (~11% of the thread, ~430 samples), so it
just swapped an indirect call for a direct call + a pointer-compare chain (and a
fallback indirect call for the rarer register-specified shifts) — Snakes FPS
stayed within noise (33–35 vs a 34 baseline). The shifter dispatch is inherently
data-dependent and the shift computation is cheap, so there's no win without a
broad translate-time precompute / cream-tag refactor whose payoff doesn't justify
it. Reverted.

### Lazy condition flags — NOT pursued (data shows it regresses dyncom)
Profiled the flag mechanism: `CondPassed` (flag **reads**, via ARM predication) is
~2% of the thread, while the flag **writes** (`UPDATE_NFLAG`/`UPDATE_ZFLAG`,
carry/overflow from the inlined `AddWithCarry`) cost ~0 measurable — flags are
already denormalised (`NFlag/ZFlag/CFlag/VFlag` as separate u32 "for speed"), so
writes are cheap and inlined. Lazy/deferred NZCV (QEMU `cc_op` style) helps when
reads are rare (x86: only branches read flags); on ARM, widespread predication
reads flags constantly, so deferring would make the ~2% read path *more*
expensive while saving ~0 on the already-cheap writes — a net regression. Not
worth the large, invasive, correctness-sensitive change.

**Conclusion:** the interpreter-internal wins are exhausted. The remaining
dispatch-share lever is **instruction fusion** (needs Phase 4 stream self-A/B),
and the only structural multiplier (JIT) is off-limits on iOS. The landed wins —
ASID instruction cache, simulator render-scale cap, ReadCode-via-TLB, inlined
AddWithCarry, block-L1, and the inline memory fast-path — stand.

- **Design: self-A/B (no external dependency).** For an optimization that should
  be behaviour-preserving, the reference is the *same* dyncom with the
  optimization disabled. Gate each optimization behind a flag (compile-time
  `EKA2L1_DYNCOM_<OPT>` or a runtime bool on `dyncom_core`); the harness runs the
  identical input through an optimized and an unoptimized `dyncom_core` and
  asserts equal state. This catches any divergence the optimization introduces
  (which is exactly the regression we care about) without needing a golden
  oracle. (An external oracle — Unicorn/QEMU TCG — could be added later to also
  flag *pre-existing* dyncom bugs, but that is a heavier, separate effort.)
- **Inputs:**
  - *Randomized single instructions:* a constrained generator emits valid
    ARM (and Thumb) encodings across classes — data-processing (all shifter
    forms, S bit, all conditions), load/store (imm/reg/scaled, pre/post, byte/
    half/word, LDM/STM reg lists incl. PC), multiply/long-multiply, branch
    (B/BL/BX/BLX), `MSR/MRS`, `SWP`, saturating/`CLZ`. Seed registers/CPSR/
    memory randomly (incl. flag-boundary values, PC-as-operand, mode bits).
  - *Targeted edge corpus:* hand-written cases for the known footguns —
    `Rd/Rn/Rm == 15`, condition-code boundaries, shifter carry-out, unaligned
    access, mode/Thumb switches, and (for fusion/chaining) cross-page blocks,
    SMC via `imb_range`, process-switch (`set_asid`), and translation-buffer
    overflow.
  - *Trace replay (optional, high value):* capture an instruction+initial-state
    trace from a real Snakes/Final Battle run and replay it through A and B —
    real-workload coverage on top of the random corpus.
- **Oracle/compare:** after each step (instruction for single-op tests, block for
  fusion/chaining), diff the full guest-visible state: `Reg[0..15]`, CPSR +
  `N/Z/C/V/Q`, mode/Thumb, exclusive-monitor state, and every byte of memory the
  step could have touched. Report the first divergence with the offending
  encoding, both states, and a minimal repro seed (deterministic RNG).
- **Build/run:** a standalone test target (extend
  `src/emu/cpu/src/12l1r/tests/`, which already stands up a CPU with flat memory)
  driven by `scripts/cpu_difftest.sh`; runs N random seeds + the edge corpus +
  any captured traces, exits non-zero on first divergence. Cheap enough for CI
  and a pre-land gate for each harness-gated optimization.
- **Effort:** harness ≈ a focused mini-project (generator + state-diff +
  A/B wiring), then each optimization lands against it with its own corpus
  additions. This is the prerequisite for items 1-4 above; without it, those
  changes shouldn't go into shared CPU code.
