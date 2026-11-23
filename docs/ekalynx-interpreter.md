# ekalynx ARMv5/v6 interpreter on iOS

The Thumb block interpreter described in the measurements was an experimental prototype and is no longer included in the emulator.

The experimental `ekalynx` backend keeps the guest integer and VFP registers, CPSR, FPSCR and thread ID in a single context. ARM and Thumb code pages are keyed by address space, page number and instruction set. Decoding produces data consumed by statically compiled handlers. The backend allocates no executable memory. At revision `78d7a9438` it stopped creating or stepping a Dyncom CPU. It shares Thumb instruction normalization and VFP arithmetic with the existing interpreter. An unhandled encoding raises a guest exception. The existing interpreter remains available while compatibility testing continues.

A page handler can retire several operations before returning to the scheduler. A fused pair still has two entry points, and a budget of one retires only the first instruction. A memory miss publishes the guest state at the faulting instruction before invoking the MMU. The exclusive monitor receives the scheduled core so it resolves the correct address space. Floating-point register transfers preserve raw bits, and the scalar arithmetic path preserves cumulative inexact status when guest code reads FPSCR.

## Fixed-work throughput

The following measurements used an arm64 Release executable on the iOS simulator at `9c3b374da`. A diagnostic build took guest register snapshots at scheduling boundaries during play. A separate reference interpreter recorded the initial and final contents of every touched page in private memory. Each replay started from the same registers and memory and retired 4,096 instructions. All three backends matched the expected architectural context and final memory at the instruction boundary. Measurements used the median of 36 warmed runs per snapshot and then the median across snapshots for each game.

The opt-in differential-test build also provides `arm_replay`. Pass one or more local snapshot files to run all three backends. The little-endian file contains a 32-bit instruction budget, the initial and expected `core::thread_context`, followed by records of a 32-bit page base and two 4,096-byte page images (initial and expected). Snapshots contain guest game and ROM data and stay local. The runner checks the instruction count, context and every page on every iteration, and returns a failure status on divergence.

The table gives the median of three verification runs for each backend and the range of per-run throughput ratios. The compact backend is the earlier Thumb block interpreter. Each run verified all 117 snapshot/backend combinations.

| Guest scene | Snapshots | Dyncom ns/instruction | Thumb block ns/instruction | ekalynx ns/instruction | Throughput vs Dyncom | Throughput vs Thumb block |
|---|---:|---:|---:|---:|---:|---:|
| Reset Generation, arcade | 7 | 4.150 | 3.733 | 2.187 | 1.90–1.91× | 1.69–1.76× |
| Angry Birds, first level | 17 | 5.249 | 5.239 | 2.543 | 2.05–2.07× | 2.05–2.06× |
| Glow Air Hockey, level 1-1 | 15 | 6.531 | 6.134 | 4.191 | 1.56–1.59× | 1.46–1.50× |

These samples repeatedly visit the same few game routines. The capture accepts intervals that complete without a syscall, exception or exclusive access. Boot, cold code translation and host rendering are outside the timed interval. All three sampled workloads passed the 1.45× fixed-work gate in each run. Whole-match CPU cost and compatibility with other games require separate measurements.

Reset Generation benefits from deferred Thumb N/Z extraction. Angry Birds repeatedly fills word arrays. The ARM page decoder recognizes bounded store loops when it can prove their register dependencies, memory permissions and physical non-aliasing. It limits batching at the guest instruction budget and page boundary. The mixed ARM/Thumb Hockey sample benefits from predecoded stack ranges, precise Thumb-1 calls, ARM register-list writeback and keeping predicated arithmetic on the decoded page.

## Running games

A simulator-only diagnostic build at `9c3b374da` switched between Dyncom and the independent interpreter on the next scheduling quantum. Each comparison used eight windows in B-A-A-B-A-B-B-A order in one running game (B = Dyncom). Entries below are the medians of four windows per engine. Guest throughput counts retired guest instructions per CPU second of the emulator thread inside `core::run`, including synchronous host callbacks. Process CPU time includes rendering and other threads and is divided by the number of submitted guest frames. The diagnostic wrapper copies the context at a Dyncom scheduling boundary. Both engines use the same frame limit. Small differences warrant repeated measurements on the physical device.

| Scene | Live throughput vs Dyncom | Dyncom → ekalynx process CPU ms/frame | Change | Dyncom → ekalynx frames/s |
|---|---:|---:|---:|---:|
| Reset Generation, arcade board | 1.56× | 29.49 → 22.57 | -23.5% | 23.79 → 23.48 |
| Reset Generation, directional input | 1.79× | 45.49 → 34.09 | -25.1% | 21.66 → 23.46 |
| Angry Birds, slingshot ready | 1.07× | 22.13 → 21.73 | -1.9% | 56.55 → 57.13 |
| Angry Birds, repeated launch | 1.08× | 23.91 → 23.04 | -3.7% | 55.17 → 56.40 |
| Glow Air Hockey, level 1-1 | 1.10× | 24.05 → 23.99 | -0.2% | 37.97 → 39.96 |

Reset Generation used a fresh arcade board for the directional-input comparison. Each eight-second window sent left, down, right and up at fixed offsets. The random board differs between the two Reset rows. A host build overlapped the beginning of the directional-input run, so the exact percentages for that board warrant a quiet-host repeat. The Angry Birds launch comparison restarted level 1-1, issued the same slingshot swipe and measured approximately 6.6 seconds of launch and flight each time. One guest screenshot per timed launch confirmed the bird in flight. Both engines include the cost of that capture. Glow Air Hockey ran on the Nokia 5800 (S60 5th Edition), Reset Generation on the 5320 (S60 3rd Edition) and Angry Birds on the X7. Frame counts come from guest submissions. Host display pacing was outside this measurement.

The fixed-work gate and the live measurements describe different workloads. The 4,096-instruction samples omit syscalls. Each running game includes its mix of threads, system services, graphics and input. Only Reset Generation exceeded 1.45× during play in these measurements. Physical energy and compatibility across untested games remain open.

A second Angry Birds launch comparison used the ordinary Release app at `eec36a03c`, with each backend selected before boot. Four repeat launches per backend produced median process CPU costs of 23.15 ms/frame with Dyncom and 22.64 ms/frame with ekalynx (2.2% lower), at 56.81 and 57.50 submitted frames/s respectively. Both sets of four screenshots showed the bird approaching the same structure.

### Reset Generation on iPhone Air

The iPhone Air ran the 5320 firmware and the N-Gage 2.0 version of Reset Generation with JIT off at CPU revision `b586501a2`. Each backend played two freshly generated arcade boards with Plumber against Aggressor. The order was ekalynx, Dyncom, Dyncom, ekalynx. Each board contributed three approximately eight-second windows. Left, down, right and up were sent at seconds 1, 3, 5 and 7. Screenshots after the windows showed the board. All windows were recorded while charging at 50% brightness with Low Power Mode off and the thermal state serious at both ends.

| iPhone Air, six windows per backend | Process CPU ms/frame | Rendered frames/s | Process energy mJ/frame |
|---|---:|---:|---:|
| Dyncom | 39.90 | 23.62 | 57.33 |
| ekalynx | 12.11 | 30.24 | 14.43 |

The values are medians over windows. Energy is the delta of the process energy counter divided by rendered frames. Dividing by elapsed wall time instead gave median process power of 1.33 W on Dyncom and 0.44 W on ekalynx. Board layout and the timing of game turns varied between launches. This measures short intervals under thermal pressure and does not establish a whole-device battery-life change.

In a separate standard Release simulator run on the M1 Max, eight directional-input windows per backend gave 37.06 versus 27.45 process CPU ms/frame and 23.04 versus 23.48 rendered frames/s (Dyncom versus ekalynx). Across individual boards the CPU medians were 44.73 and 28.55 for Dyncom, 23.53 and 31.59 for ekalynx. Board selection materially affected this comparison. At this revision the seven existing fixed-work captures measured 1.91× throughput versus Dyncom by summed execution time.

## Live workload profiles

On an arm64 Release iOS simulator, all 17 Angry Birds replay captures followed exactly the same sequence of 4,096 guest instructions (942 distinct instruction addresses). The 15 Glow Air Hockey captures followed four sequences (2,382 distinct addresses in total). Sampling the running game at randomized guest-instruction intervals gave the following coverage. The two 5320 rows use the later offset captures described below. The comparison uses both the guest address and ARM/Thumb state. A hit means the address appeared somewhere in the replay set, even if the surrounding path or data differs.

| Active scene | Guest instruction samples from the game process | Thumb samples | Samples at an address in the replay set |
|---|---:|---:|---:|
| Angry Birds, bird in flight | 250,368 | 7.8% | 35.5% |
| Glow Air Hockey, puck in play | 9,472 | 66.3% | 42.7% |
| 5320 Snakes, first 3D level | 867,840 | 0.9% | 91.0% |
| N-Gage 2.0 Hooked On, casting from shore | 1,159,422 | 0.4% | 24.8% |

Separate timed builds selected the real interpreter before boot. They measured CPU time on the emulation thread around each scheduling quantum and around synchronous SVC dispatch, and process CPU time across all threads. PC sampling was disabled for timing. With the smaller graphics batches, the independent interpreter accounted for 19.9% of process CPU during a bird launch, including 2.4% of that interval inside synchronous SVCs. Excluding those SVCs, throughput was 162 versus 150 million guest instructions per CPU second with Dyncom. The replay's repeated path overstates the speedup even within guest execution. Hockey spent 2.4% of process CPU inside the interpreter, including 56% of that interval in synchronous SVCs. Eliminating all remaining guest execution at the same workload would save only about 1% of process CPU. Graphics-thread stacks entered the simulator OpenGL implementation; a seven-second Hockey sample submitted 75,460 rectangle draws and 7,391 bitmap draws.

Five graphics lists per Angry Birds frame averaged 34 commands each. Reserving the logical flush limit for every new list initialized 12,800 slots regardless of use. Growing from 32 on demand reduced the average final allocation to 62 slots per list and lowered synchronous SVC CPU from 0.51 to 0.11 ms/frame during launch. Hockey submitted two lists per frame averaging 716 commands, with an average final allocation of 1,040 slots. Its SVC time changed little. These measurements alternate allocation policies within the same running game. The first two Angry Birds ekalynx windows overlapped host build activity and are excluded from its CPU comparison.

In a separate eight-window Hockey comparison, each frame triggered about 88 bitmap change checks. Seeding a single XXH3 pixel hash with the bitmap metadata reduced hashing CPU from 0.319 to 0.161 ms/frame. Process CPU was 21.70 and 21.95 ms/frame respectively. Graphics submission and rendering still dominate this scene.

### Expanded Angry Birds capture

The 17 initial Angry Birds replays followed the same path from a scheduler entry. A second Release simulator diagnostic selected a pseudorandom retired-instruction boundary inside the running core, copied the current game thread's registers and mapped guest pages, and ran a reference interpreter on that private state. Capture excluded intervals interrupted by a syscall, exception or exclusive access. These copies were used only for correctness and fixed-work timing. Live CPU timing used separate builds without snapshot capture.

Sixteen recording windows in Poached Eggs levels 1-1 and 1-2 yielded 384 different 4,096-instruction paths and 19,176 unique address/instruction-set pairs. The windows included waiting at the sling, pulling back, firing, structural collapse and the cleared-level screen. Several windows from the first attempt at a manual pull remained at the sling; a later verified swipe supplied the level 1-1 collision. Guest screenshots distinguish these cases.

| Live instruction samples | Samples | Addresses covered by original replay set | By expanded set |
|---|---:|---:|---:|
| Level 1-1, separately launched PC-sampling run | 682,496 | 41.1% | 93.7% |
| Level 1-2, including a second launch | 690,944 | 40.5% | 94.2% |

The first level's captures alone covered 90.3% of sampled addresses during the second level's impact. This is frequency-weighted address coverage. It does not measure the fraction of host CPU time or imply the same coverage on later levels. With the interpreter at `f794b5f34`, the ratio of summed Dyncom time to summed ekalynx time was 1.062× across the new equal-length intervals. The median per-interval ratio was 1.098×. The original approximately 2× result characterized one guest path.

The live samples repeatedly reached an ARM dispatch sequence ending in a PC-relative, register-indexed conditional load to PC. Nine instruction addresses in this sequence accounted for 4.3% and 5.3% of guest instructions in the two collision windows. Several slow replays instead spent roughly 8% of their instructions in signed long multiplication, and another group repeatedly used multiply-accumulate. The instruction mix changes with the sampled path even when the visible scene is similar.

Keeping indexed loads to PC, immediate AND/TST, MLA and SMULL on the decoded ARM page reduced the time spent on these paths. The remaining slow physics replay contained 821 floating-point stores and 499 loads in 4,096 instructions. Predecoding the register span and addressing mode for VFP transfers lets execution continue on the same decoded page when the entire transfer fits in a cached guest mapping. Cross-page and uncached accesses still use the ordinary memory path.

| Release simulator, 384 fixed-work paths | Summed Dyncom / ekalynx time | Median path speedup | Slowest path vs Dyncom |
|---|---:|---:|---:|
| Previous independent interpreter | 1.062× | 1.098× | 0.565× |
| Integer and jump-table changes | 1.122× | 1.176× | 0.587× |
| Including VFP transfers | 1.177× | 1.215× | 0.950× |

Across all captures the new interpreter completed the same work 1.085× as fast as the previous independent interpreter. Dyncom was timed in each replay run, so the ratios against Dyncom have their own timing variance. Actual level 1-1 flights on the M1 Max simulator went from 173.1 to 181.6 million guest instructions per emulation-thread CPU second, 21.56 to 21.66 process CPU ms/frame, and 57.58 to 57.16 frames/s (medians of ten baseline and five final flights). The faster guest instruction stream did not reduce the whole process's CPU time per frame in this scene.

On iPhone Air, a Release diagnostic repeated the same first-level shot with JIT disabled. Each window covered about 6.5–6.8 seconds from the shot through impact. The denominator was the emulator's rendered-frame counter. The table includes only windows with the phone charging, brightness at 55%, Low Power Mode off and thermal state fair at both ends. It excludes a separate unplugged Dyncom run and three nominal-temperature baseline windows.

| iPhone Air, level 1-1 | Windows | Process CPU ms/frame | Frames/s | Process energy mJ/frame (median and range) |
|---|---:|---:|---:|---:|
| Dyncom | 4 | 8.71 | 59.65 | 4.38 (3.88–4.82) |
| Previous independent interpreter | 5 | 8.94 | 59.41 | 1.78 (1.65–3.62) |
| Including integer and VFP changes | 4 | 9.08 | 59.40 | 1.63 (1.62–1.74) |

The process energy counter (`proc_pid_rusage`, `ri_energy_nj`) and process CPU time moved in different directions. Most of the observed energy difference against Dyncom was already present in the previous independent interpreter. The old and new independent interpreter's energy ranges overlap. The short, serially collected windows do not establish a battery-life improvement or energy use over a full level. The standard device Release at `b586501a2` launched Angry Birds on iPhone Air with ekalynx selected and JIT disabled. The diagnostic is absent from this build. All 384 expanded replays and 75 earlier game replays matched the reference state, memory and instruction count. The Release simulator passed the 12 default and five Angry Birds regression checks.

### Nokia 5320 gameplay

Snakes used the native game (UID `0x2000730F`) on RM-409 and the opening 3D grid. Hooked On used N-Gage 2.0 (UID `0x2000AFBC`) on the same ROM. Repeated confirmation selected Instant Play, the existing save and the fishing site. Timed windows started after the rod and casting HUD appeared and included cast/reel input. These are separate game launches. The fishing animation, time of day and exact work per frame can vary.

Both games spent about 78–79% of process CPU in the interpreter before the arithmetic changes. Snakes spent 10.9% of sampled guest instructions in a software division loop containing shifted reverse subtraction, predicated subtraction and add-with-carry. Hooked On spent 18.5% in a region containing shifts, coordinate clamps and indexed loads. Predecoding the common arithmetic forms at `4955c9421` increased live throughput from 203.0 to 228.5 MIPS in Snakes and from 218.8 to 246.9 MIPS in Hooked On. Median process CPU went from 49.44 to 44.75 ms/frame and 31.20 to 28.50 ms/frame respectively, in six four-second windows per scene and build.

A second set of fixed-work captures advanced a private copy of each selected scheduling context by a pseudorandom offset of up to 2,047 instructions before recording. The 18 captures per title followed 18 distinct paths in each game. Their address sets covered 91.0% of a separate live Snakes instruction sample and 24.8% in Hooked On. Selection still requires completing 4,096 instructions without a syscall or exception. The slowest Snakes captures at the arithmetic revision ran at 0.71–0.73× Dyncom and repeatedly executed `LDRSH`, `MUL` and `STRH` with post-incrementing sample buffers. The two Snakes loop regions at 0x70045644–0x70045663 and 0x70045964–0x700459a3 together accounted for only 0.14% of the live instruction sample. High address coverage alone does not establish representative frequencies. Several slow Hooked On captures repeatedly multiplied and loaded halfwords.

Keeping immediate halfword transfers and unflagged MUL on the decoded page at `83366a058` improved the slowest Snakes replay from 0.71× to 0.94× Dyncom. It added 1.1% live throughput in Snakes and 8.2% in Hooked On over the arithmetic revision. In the final comparison, Dyncom and ekalynx used the same instrumented Release binary on an M1 Max, with JIT disabled and each engine selected before boot. The table uses the median of six four-second windows per game and backend. Replay ratios are the median of paired per-snapshot speedups.

| 5320 active scene | Replay vs Dyncom | Dyncom → ekalynx live MIPS | Live ratio | Dyncom → ekalynx process ms/frame | Dyncom → ekalynx submitted frames/s |
|---|---:|---:|---:|---:|---:|
| Snakes, first level | 1.14× | 210.9 → 231.0 | 1.10× | 47.90 → 44.72 (−6.6%) | 15.96 → 15.97 |
| Hooked On, fishing | 1.20× | 224.4 → 267.2 | 1.19× | 30.58 → 27.28 (−10.8%) | 26.57 → 29.80 |

The older independent interpreter, measured with the same graphics changes, used 49.44 ms/frame in Snakes and 31.20 in Hooked On. The two CPU changes together reduced that cost by 9.6% and 12.6%. Snakes retired about 7.87 million guest instructions per frame on both final engines. Hooked On retired 5.43 million with Dyncom and 5.53 million with ekalynx. Neither new scene reached the 1.45× live target. All 75 replay snapshots matched expected context, retired count and memory on all three interpreters. The final simulator replay medians versus Dyncom were 1.89× for Reset Generation, 2.08× for Angry Birds and 1.55× for Hockey.

The ordinary Release app was also checked before and after the graphics and shifted-arithmetic changes. Across four Angry Birds launches, process CPU went from 23.00 to 22.64 ms/frame and guest submissions from 56.56 to 55.92/s. Two ten-second Hockey windows per build gave 23.69 to 23.45 ms/frame and 39.78 to 36.96 submissions/s. These separate launches show a small CPU difference and no demonstrated frame-rate improvement for either game. The final halfword revision passed all 12 default Release simulator regression checks and all five Angry Birds input checks.

## Compatibility checks

The arm64 Release simulator build at `eec36a03c` passed the default 12 checks, five Angry Birds checks, six N-Gage 2.0/Snakes Subsonic checks and nine Asphalt 6 checks including the Nassau race HUD. The independently selected backend also ran the Reset Generation arcade board and Glow Air Hockey level 1-1. The later picker availability correction at `f80e6c6a1` changes only the SwiftUI disabled condition. On the final Release simulator app, selecting ekalynx through Settings persisted across a full relaunch, and selecting Dyncom switched the stored backend back.

The CPU revision `36fb408a7` passed two host differential runs with different seeds, each covering 200,000 single-instruction cases and 6,041 program comparisons. An additional run instrumented the independent interpreter with AddressSanitizer and UBSan and passed the differential suite and all 39 game snapshots. A Release simulator replay at that CPU revision measured 1.90×, 2.01× and 1.56× versus Dyncom for Reset Generation, Angry Birds and Glow Air Hockey respectively.
