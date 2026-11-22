# Thumb block interpretation on iOS

The Thumb block interpreter described in the measurements was an experimental prototype and is no longer included in the emulator.

Reset Generation exercises a mix of Thumb integer arithmetic, pixel blending, table lookups and short branches. Its arcade round chooses a different board at each launch. The CPU cost of two boards can differ by more than the gain from changing interpreters.

## Measurement

The Release simulator ran the RM-409 firmware with JIT disabled and a frame limit of 60. A diagnostic build alternated the ordinary Dyncom interpreter, the compact interpreter at `2fcdeb267` and a candidate on the emulation thread. Each switch cleared the translated blocks and direct links before decoding with the new mode. The game, board, emulated RAM and rendering configuration persisted across the switch. Each of two arcade rounds received the same directional input sequence. The tutorial dialogue supplied a quieter control.

Each mode had four eight-second windows per scene, with 1.8 seconds for the switch and cache warm-up. The order was counterbalanced and changed between rounds. CPU time per submitted frame uses process user and system time between the first and last frame in each window. Frame rate and the 95th percentile of inter-frame intervals use those same boundaries. These metrics describe simulator execution. The device energy measurement requires an independent power recording.

A 15-second Time Profiler recording on the previous compact interpreter put 22.7% of process sample weight in its compact execution function, 42.5% in the rest of the interpreter including dispatch, and 34.8% outside it. The split varies with the board. In particular, a sample in the shared dispatcher cannot by itself identify which guest opcode was running.

## Interpreter experiments

Extending blocks across conditional branches and narrow backward edges improved some samples relative to Dyncom, but lost to the previous compact interpreter in same-round comparisons. On the two measured rounds the new prototype took 45.66 and 53.32 ms of CPU per submitted frame, versus 45.45 and 51.17 ms for the previous compact path. Its tutorial cost also rose, from 33.96 to 35.64 ms. Branch handling stays in the shared dispatcher.

Keeping blocks straight-line allows a data TLB hit to defer PC and flag synchronization. A slow access can invoke a guest exception handler, so it must observe the faulting instruction's PC and previously produced flags. A single-register LDMIA or STMIA updates its base before the callback like the existing interpreter. Empty, overlapping and multi-register lists use the existing path. PC-relative loads align the guest instruction address before applying the immediate displacement.

Six common arithmetic and shift pairs share execution handlers with their individual operations. The translator keeps both operand records and checks the remaining budget between the two register writes. A thread switch or single-step can therefore observe the intermediate register and flag state. Neither the decoder nor the pair selection depends on a game UID or code address.

## Simulator measurements at `acf39c7d3`

Each cell gives process CPU milliseconds per submitted frame, submissions per second and the 95th-percentile frame interval in milliseconds.

| Scene | Dyncom | Previous compact interpreter | Final compact interpreter |
|---|---:|---:|---:|
| Arcade round 1 | 29.54 / 23.81 / 61.33 | 28.14 / 23.90 / 61.37 | 26.17 / 23.77 / 61.04 |
| Arcade round 2 | 28.97 / 23.70 / 60.69 | 28.25 / 23.65 / 60.93 | 25.78 / 23.70 / 61.41 |
| Tutorial dialogue | 35.67 / 24.05 / 58.14 | 33.90 / 23.83 / 59.94 | 33.65 / 23.47 / 60.68 |

Relative to Dyncom, the two arcade rounds used 11.4% and 11.0% less CPU per frame and 11.6% and 11.0% less process CPU per second. The largest observed submission rate decrease was in the tutorial, at 0.58 frames/s. The fixed-input color-blending replay ran 24 million guest instructions per window on the development Mac. Median cost was 3.632 ns per instruction with Dyncom, 2.619 ns with the previous compact backend and 2.419 ns with the final backend. Frame-level results include rendering, scheduling and host services.

The iOS settings switch selects the experimental interpreter for the next guest launch. Dyncom remains the default while device energy and a wider range of workloads await evaluation. The release build omits the diagnostic mode switch and per-frame timing instrumentation.

## Operand forwarding, flag calculation and block exits

In a separate arcade-round recording on `acf39c7d3`, a simulator-only probe counted 99.48 million fully executed pairs. A later instruction read the previous result in 78.1% of them. In 68.0% of all pairs it also overwrote the same register. The table counts pairs actually executed, excluding pairs interrupted after the first instruction.

| Pair | Executions (millions) | Read and overwrite first destination |
|---|---:|---:|
| LSR → AND | 1.34 | 12.7% |
| LSL → LSR | 51.67 | 83.7% |
| MUL → LSR | 7.41 | 84.0% |
| SUB → MUL | 17.78 | 52.9% |
| ADD → AND | 12.25 | 68.6% |
| AND → LSR | 9.03 | 1.9% |

Of 451.82 million completed compact blocks, 40.5% ended before a conditional branch or SVC, 23.3% before a low-register CMP, 15.4% before an unconditional short branch, and 11.7% before a register-controlled LSL. Immediate and register-offset LDRB/STRB together accounted for 0.20%. These counts describe the instruction following a completed block, excluding isolated instructions and attempted spans too short to decode. Profiling and timing used separate builds.

For the LSL → LSR in-place candidate, translation selected a distinct handler when the first destination was both the second source and its destination. A full pair kept its intermediate result in a host register. A partial pair committed the first result and flags. A separate arithmetic candidate used signed and unsigned overflow builtins for ordinary ADD, SUB and CMP. Apple Clang's arm64 Release build folded both checks to one 32-bit ADDS or SUBS followed by two CSET instructions. The original path emitted separate widened sums and a comparison.

Timing the two candidates separately gave the following results. Each cell gives CPU ms per frame / submitted frames per second. Each comparison used the same board and four windows per mode.

| Scene | Compact baseline | Arithmetic flags alone | Shift forwarding alone |
|---|---:|---:|---:|
| Arcade round 1 | 28.24 / 23.78 | 27.76 / 23.78 | 28.21 / 23.43 |
| Arcade round 2 | 28.64 / 23.59 | 27.70 / 23.99 | 28.07 / 24.03 |
| Tutorial | 33.24 / 23.87 | 33.44 / 23.68 | 33.81 / 23.51 |

A shift-heavy synthetic loop ran at 2.099 ns per instruction on the baseline and 2.010 ns with forwarding. The measured blending replay changed from 2.422 to 2.409 ns. The arithmetic candidate ran at 2.164 and 2.434 ns on the same loops.

On two additional arcade boards the baseline, flags-only and combined candidates took 41.84 / 41.12 / 42.06 and 49.82 / 49.00 / 49.12 ms per frame. The latter comparison uses three windows per mode: screenshots show the last group crossed into the end-of-round animation. Including that group gives 44.28 / 47.92 / 49.03 ms per frame and compares different scenes. The corresponding tutorial values were 33.78 / 33.33 / 33.14 ms. The arithmetic specialization was retained. Forwarding showed a gain in isolation but added no reproducible arcade improvement over flags alone.

Across the four matched arcade comparisons, CPU time per frame decreased by 1.7%, 3.3%, 1.7% and 1.7% with the retained change. Multiplying frame cost by submission rate gives a smaller change in process CPU per second (approximately −1.7%, −1.6%, +0.4% and −0.3%). The game sometimes uses the spare execution time to submit additional frames.

A follow-up timing build tested two extensions to straight-line blocks. Decoding register CMP changed arcade CPU time per frame from 43.52 to 44.76 ms on one board and from 45.51 to 44.39 ms on another. Also decoding register LSL gave 45.29 and 44.54 ms. Frame submission rates improved in these runs, but the changes did not consistently reduce CPU cost across boards.

## Verification

At `acf39c7d3`, two host differential runs covered 300,000 single-instruction cases in total and 5,722 and 2,958 whole-program cases against Dynarmic, respectively. Neither run used the reference backend's interpreter fallback. The focused cases exercise aligned PC-relative addressing, source/destination overlap, stopping between fused instructions, and the state visible during a failed or retried writeback transfer. The uninstrumented iOS Release build passed the default regression suite (12 checks), the N-Gage 2.0 suite (6) and the Angry Birds suite (5).

At `e4d75741d`, the two differential runs passed 300,000 golden cases and 8,720 whole programs against Dynarmic with zero interpreter fallback. The additional cases cover carry and signed overflow for register ADD/SUB and immediate ADD/SUB/CMP, including 0xffffffff + 1 and 0x80000000 − 1. The final uninstrumented Release simulator build passed all 12 default regression checks. Reset Generation reached the same tutorial dialogue through the ordinary app launch path on both backends, with no guest panic or access violation in either log. The iOS settings switch was checked in both directions through the simulator UI.
