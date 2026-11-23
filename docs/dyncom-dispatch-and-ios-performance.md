# Dyncom dispatch and the next iOS performance work

> **Update (2026-09-26).** Measured on a real workload (Snakes on the 5320, host
> CPU per guest instruction), separate dispatch sites cost +4.2% and a cached
> handler offset +0.9%, so neither landed. Block linking and CMP+branch fusion
> did (−6.6% together with a block L1 index fix); see
> [dyncom_optimization_plan.md](./dyncom_optimization_plan.md).

The September 2026 investigation started with two different goals: reduce host
CPU consumption while Snakes already meets its frame target, and investigate
Angry Birds Rio 1-8 stutter during entry, flight and collisions on the X7.
A higher instruction benchmark score is useful evidence, but does not establish
that either game's frame times improved.

## A compiler transformation hid an available optimization

The existing interpreter already uses computed goto with a dispatch at each
handler. That source layout was misleading. Disassembly of the Apple Clang
Release build showed one shared indirect dispatch branch in
`InterpreterMainLoop`, plus a separate indirect branch for the loop accelerator.
The compiler merged the handler tails. The next-opcode branch consequently lost
its per-handler prediction context.

An empty volatile assembly statement with a read/write `inst_base` operand and a
unique immediate operand keeps these sites separate without emitting an
instruction or adding a memory barrier. The resulting loop has 142 dispatch
branches. In the host test executable, the function grows from 64,560 to
80,900 bytes (about 25%); this can increase instruction-cache pressure. Branch
site counts alone therefore do not establish a net win for every game. The
per-instruction execution budget, stop checks, exception paths and
single-step behavior are unchanged. The non-GNU switch implementation is
unchanged. This is a shared interpreter optimization, with no title-specific
recognition.

This failure mode is also documented by the
[CPython investigation of merged dispatch tails](https://github.com/python/cpython/issues/129987).
LLVM's [tail duplicator implementation](https://github.com/llvm/llvm-project/blob/main/llvm/lib/CodeGen/TailDuplicator.cpp)
explicitly accounts for indirect-branch interpreter performance. Increasing its
size/predecessor/successor limits in this build did not restore the dispatch
sites, so no LLVM command-line tuning is retained.

## Experiments and their limits

The host benchmark warms translated blocks, runs 16,777,216 guest instructions
per case, and checks the resulting register/flag state against an independent
ARM data-processing model. It includes immediate-only, register-only and mixed
ADD/EOR/MOV instruction streams. It measures a narrow dispatch/ALU workload,
not the memory, branch, HLE or graphics mix of a whole game.

Representative interleaved Release measurements on this host:

| Implementation | Immediate | Register | Mixed |
|---|---:|---:|---:|
| Baseline | 71.5 ms | 72.9 ms | 78.2 ms |
| Separate dispatch sites | 69.8 ms | 69.9 ms | 70.5 ms |
| Cached handler pointers + separate sites, experimental | 66.2 ms | 66.6 ms | 68.0 ms |

These are local measurements, not portable speed guarantees. The retained change
reduces the mixed case by about 10%. Caching handler pointers in translated
instructions gives a further small gain in this microbenchmark, but changes
instruction-header size/alignment and increases some translated instruction
strides. It remains an experiment until a representative game workload justifies
the larger change. Predecoding rotated immediates was also tested: its immediate
case improved while its register case slowed down, with little mixed-workload
benefit. That change was discarded.

The dispatch change passed 800,000 single-instruction golden cases, 22,008
whole-program cases against dynarmic without interpreter fallback, VFP host vs
software comparisons, cache/ASID and loop-accelerator cases, breakpoint resume,
undefined-instruction reporting and negative controls.

## What the game investigation establishes

Rio's actual 1-8 scene was reached and visually checked, including a launched
bird, its trajectory, and a score increase after collision. Both baseline and
candidate use dyncom with JIT opt-in disabled. Release builds were used.

The simulator's graphics thread spends most of its sampled time in
`draw_bitmap` through `GLRendererFloat` triangle rasterization. The guest thread
spends substantial time asleep in scheduler events; among its executing samples,
`InterpreterMainLoop` dominates and VFP software helpers account for a much
smaller share. This rules out treating every busy host core as interpreted guest
physics. It does not identify the cause of the reported physical-device stutter:
the device uses hardware GLES and needs its own profile.

A preliminary 20-second comparison gave these results (process CPU uses 100%
per host core, and frame rates are presentation submissions):

| Scene | Baseline | Separate dispatch sites |
|---|---|---|
| Rio 1-8, settled before the first launch | about 150% CPU, about 60 submissions/s | 151.7% CPU, 60.5 submissions/s |
| Snakes level 1, initial straight-running sequence | 139.4% CPU, 39.8 submissions/s | 139.1% CPU, 39.8 submissions/s |

The baseline Rio CPU value was converted from Mach absolute-time ticks using
this host's timebase; treating these counters as nanoseconds produces a bogus
3.6% result. Screenshot readback was outside the measured intervals. These
short runs do **not** demonstrate a meaningful game-level CPU reduction. Snakes'
sequence can enter its death animation, so it is also not a substitute for a
long, controlled gameplay/energy run at a fixed frame target. The dispatch
change remains an unlanded candidate, not a claimed fix for Rio stutter.

The Mac was locked during this run. AXe accessibility enumeration worked, but
its screenshot and touch paths blocked in SimulatorKit framebuffer/orientation
queries. Temporary renderer frame export and debugger-driven bridge input
allowed guest visual checks; these probes are not production changes. Debugger
pauses and screenshot readbacks must be excluded from timing intervals. The
bridge's rendered-frame counter counts submitted presentation commands, not
completed GPU frames, so it is not a measure of worst-case display latency.

## Where advanced interpreter techniques fit

| Area | Current implementation and next decision |
|---|---|
| Dispatch | Preserve independent sites first. Then benchmark direct threading with real opcode mixes and translated-cache footprint. |
| Handler calling convention | A tail-call interpreter can keep hot state in registers and reduce a monolithic loop's spills. It requires a supported mandatory-tail-call convention, stable handler ABI, and separate portability work for MSVC. It is an architectural experiment, not a flag to enable. |
| Superinstructions | Fuse frequent opcode pairs or sequences to remove dispatch and state materialization. Every partial budget, fault, conditional instruction and debugger exit still needs the original architectural state. Profile pairs before selecting fusion candidates. |
| Block chaining | The ASID-tagged translation cache and direct-mapped block L1 already avoid much lookup work. Chaining must respect invalidation, ASID reuse, self-modifying code and interrupt exits. Measure remaining block lookup time before adding links. |
| Memory | Scalar accesses already have inlined TLB fast paths; LDM/STM use page-local cursors. Investigate TLB collision rates and page-boundary slow paths only when samples show them. Increasing cache size blindly can worsen data-cache pressure. |
| Arithmetic/shifters | Common shifters and arithmetic helpers are already inlined. Earlier planning documents describing all shifters as indirect calls are stale. Retain independent flag/carry tests for further specialization. |
| VFP | Single-precision normalized finite operands have a host path; special values and zero often fall back. Extending to exact zero cases needs signed-zero, underflow and exception tests. The current fast path already omits cumulative inexact, so it must not be described as fully exception-bit-accurate. Double precision remains a separate profiling question. |
| Scheduler/timers | Guest idle waits already block rather than spin. Faster execution should increase sleep at the same guest frame target. Batching budget checks is unsafe around blocking SVCs, `stop()`, faults and single-step. Lowering emulated clock speed or dropping frames is not an equivalent optimization. |
| Graphics | Simulator software GLES makes present/upscale work expensive. Frame pacing, redundant presentation, command/state changes, shader translation and hardware Metal/ANGLE should be evaluated separately from CPU dispatch. Existing double buffering and simulator scale cap must be included in the baseline. |
| Audio/IPC/files | No dominant contribution was established in the sampled Rio scene. Trace sustained service retries, file reads, allocation and logging only when they appear in the affected interval. Removing service work without checking the Symbian client/server contract risks compatibility. |

The [Wasmi 2.0 implementation report](https://wasmi-labs.github.io/blog/posts/wasmi-v2.0/)
combines direct threading, compact instruction representation and other layout
changes. Its reported gains concern its own workloads; they do not transfer as
an expected percentage to dyncom. The useful lesson is to evaluate dispatch,
representation and memory locality together.

[Mandatory tail calls and interpreter calling conventions](https://blog.reverberate.org/2021/04/21/musttail-efficient-interpreters.html)
provide another route to reducing dispatch/state overhead. The
[Pulley design](https://github.com/bytecodealliance/rfcs/blob/main/accepted/pulley.md)
illustrates a larger alternative: compile into a bytecode designed for efficient
interpretation. Adopting that direction for ARM would require a new lowering
layer and precise guest-state reconstruction. Native executable-code generation
is not assumed available on ordinary iOS installations.

The [Ertl/Gregg interpreter research](https://www.complang.tuwien.ac.at/projects/interpreters.html)
is relevant to prediction and instruction fusion, but dynamic replication of
native handlers has different executable-memory requirements from static
superinstructions. These should not be treated as interchangeable techniques.

## Acceptance criteria for further work

For Snakes, compare CPU seconds per fixed period of active gameplay at the same
frame target, with paired runs and the same scene. For Rio, separately measure
level-entry and flight/collision frame-time distributions: median FPS hides
short stalls. Split guest CPU time, graphics CPU time and waits; on hardware,
include thermal state and GPU timing. A simulator-only result cannot establish
an iPhone energy or GPU result.

Only retain changes that improve the relevant metric without changing guest
behavior. Run the differential harness and the Release simulator regression
suite, visually check the affected game and a control, and validate physical
hardware before claiming the device stutter is resolved.

## Validation status of this candidate

The final source passed another 200,000 golden cases and 5,508 whole-program
cases plus the VFP and exception checks. The clean Release simulator app built
successfully after all temporary frame-export code was removed. Rio's 1-8
launch/collision path and Snakes' 3D path were visually checked in the candidate
with the diagnostic frame exporter; neither log showed a guest panic or access
violation during those checks.

The default Release regression suite passed all 12 checks, including Final
Battle's 90-second run, Calculator input/softkeys, N95 Calculator and strings.
Their screenshots were also reviewed. The separate Angry Birds touch suite
passed splash and menu checks, but `simctl screenshot` blocked for over a minute
at the episode-selection capture. That run was stopped and is incomplete;
previous screenshots were not accepted as evidence for its remaining checks.

The signed device Release build also succeeded, and its generated interpreter
contains the same 143 indirect branches. Physical-device profiling is still
required before claiming a device improvement: the iPhone Air reported that its
passcode was required. No commit or device performance claim is made from this
run.
