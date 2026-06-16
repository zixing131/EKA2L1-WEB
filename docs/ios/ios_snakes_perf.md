# iOS Snakes / dyncom performance investigation (2026-06-14)

Goal: Snakes (uid `0x2000730F`, N95) ≥ 30 FPS on the iOS **simulator** while
staying on the dyncom CPU backend (dynarmic still SIGSEGVs Calculator). Baseline:
~22–23 FPS in active 3D gameplay, host CPU pinned.

## Method

Sampled the live simulator process (native Mac process) with `sample <pid> 5`
during active gameplay. Two hot threads.

## Findings

### 1. dyncom instruction-cache thrash (CPU thread, ~17% self)
`InterpreterTranslateInstruction → decode_arm_instruction` burns ~17% of the
guest CPU thread *during a hot game loop*, where a working translation cache
should be ~0%. Root cause: `dyncom_core::load_context()` calls
`clear_instruction_cache()` on **every** guest thread context switch
(arm_dyncom.cpp). The cache (`instruction_cache`, keyed by virtual PC) is only
truly invalid on an **address-space (process) switch**, not a same-process
thread switch. Snakes is IPC-heavy (window server + FBS glyph raster every
frame) → constant process switches → the cache is wiped and Snakes re-decodes
its render loop after every IPC round-trip.

Fix direction: **ASID-tag the instruction cache** so translations from different
address spaces coexist and survive process switches. asid = `int32_t`, ≤256 page
dirs, **recycled** on process exit (`rollover_fresh_addr_space`) → must clear the
whole icache on process teardown to stay correct (rare; never during gameplay).
Scheduler already anticipated this (commented `//run_core->set_asid(...)` at
scheduler.cpp:120).

### 2. CPU↔GPU serialization on a full-Retina software blit (both threads)
The iOS simulator runs GLES **in software** (`GLRendererFloat` /
`gldRenderFillTriangles`). The present blit (`submit_screen_frame`) upscales the
240×320 guest screen to the full native-Retina EAGL drawable
(`contentScaleFactor = UIScreen.main.nativeScale` = 3× → millions of textured
px/frame). The guest CPU thread blocks ~27% of wall time in
`submit_screen_frame → ogl_graphics_driver::wait_for` waiting for that blit.
Neither thread is saturated (graphics ~54% busy, CPU ~27% blocked) — they
ping-pong via the present handshake, so frame ≈ emulation + blit (serialized).

Fix directions (sim is a dev tool; device uses the real GPU so the blit is free
there): cap the present/drawable scale on the simulator, and/or pipeline so the
guest doesn't block on present.

## Fixes landed

### 1. ASID-tagged dyncom instruction cache
`armstate.h`, `arm_dyncom_interpreter.cpp`, `arm_dyncom.{h,cpp}`,
`arm_interface.h`, `scheduler.cpp`.

The translation cache is now keyed by `(asid << 32) | vpc` and is **no longer
wiped on every `load_context`**. The scheduler pushes the new address space via
`core::set_asid()` on each process switch (the previously stubbed hook); blocks
from different processes coexist and survive IPC round-trips. Because the
translation buffer is no longer reset per switch, a bump-allocator overflow guard
flushes everything if a fresh block could run past the 128 MB buffer end. SMC /
code reload is still covered by `imb_range` → `clear_instruction_cache`.

The dyncom interpreter embedded as a fallback inside other backends keeps the old
"clear every load_context" behaviour (it never receives an asid, flag-gated), so
dynarmic/12l1r are untouched.

Result: in a 5 s gameplay sample `decode_arm_instruction` /
`InterpreterTranslateInstruction` went from ~17 % of the CPU thread to **0
samples** — the re-decode thrash is gone. FPS alone only moved 23 → 24 because
the frame was now bound by the present blit.

### 2. Simulator-only render-scale cap
`EmulatorViewController.swift`.

`renderScale` caps the GL surface at 1.5× on `targetEnvironment(simulator)`
(unchanged native scale on device). The simulator has no GPU-backed GLES, so the
present blit is software-rasterized on the host; at full 3× Retina it dominated
the frame. The guest screen is tiny and the present is just an upscale, so the
capped surface is visually indistinguishable while cutting the software fill ~4×.
Touch-coordinate mapping uses the same `renderScale` so guest pointer input stays
aligned.

A double-buffered present — two ping-ponging `present_status` fences so the guest
runs a frame ahead of the vsync swap — was first prototyped, reverted over a
one-off Snakes stall, then **re-added on request**. With the later CPU-side
optimizations (ASID cache, inline mem, etc.) the stall did not reproduce across an
extended session (gameplay/death/restart/steady), and it lifts active 3D gameplay
to ~36–40 FPS (CPU ~127%, guest emulation and the present blit/swap now
overlapping instead of serialising).

## Result
Snakes active 3D gameplay: **~22–23 FPS → ~30–32 FPS** on the iPhone 16 Pro
simulator (lighter scenes ~40; transient dips on explosion particle effects /
text-heavy level intros). `scripts/ios_regression_test.sh` PASS=8/8 (Final Battle
90 s in-game + Calculator, no guest crash). Both landed changes non-regressive.

## Post-fix hotspot map (Snakes gameplay, `sample` 6 s)
With the decode thrash gone and the blit cheap, the frame is now firmly
**CPU-bound on the guest os_thread (~99 % on-CPU in `InterpreterMainLoop`)**; the
graphics thread is ~80 % idle. os_thread leaf weights (of 3746 samples):

- **~78 % irreducible ARM interpretation** — dispatch + inline handlers (the
  `InterpreterMainLoop` body ~2115), ALU/operand decode (`AddWithCarry` 123,
  `DataProcessingOperands*` ~290), addressing + block transfer
  (`LnSWoUBImmediateOffset` 142, `LdnStM*` ~180), `CondPassed` 61. Only a JIT
  (dynarmic) structurally cuts this, and it's off-limits (SIGSEGVs Calculator).
- **~9.5 % present `wait_for`** (356) — the guest CPU thread blocking on the
  single-buffered present (the reverted double-buffer's target).
- **~12 % guest memory access** — `ReadMemory32` 161 / `WriteMemory32` 112 /
  `ReadCode` 90 / `WriteMemory8` 35 + the mmu page-walk lambda 64. Data
  loads/stores already use the dyncom 512-entry TLB fast path; **`ReadCode`
  (instruction fetch) is the one accessor that bypasses the TLB** and always
  page-walks (`armstate.cpp:264`), even though `tlb_entry` already carries an
  unused `execute_addr`. Routing `ReadCode` through the TLB execute slot is the
  main remaining bounded dyncom win (~2-3 %); note the data TLB is also flushed on
  every process switch so it re-warms after each IPC.

## 2026-06 follow-up: dispatch-reduction is the wrong lever; sim is render-bound

After the double-buffer was re-added, Snakes 3D gameplay steadies at **~38 FPS
(36–40, ±2 noise)** on the iPhone Air simulator (Release/O3). A fresh `sample`
shows the frame is now **co-limited**: the guest interpreter thread is saturated
*and* a second thread is busy in the simulator's **software GLES**
(`glDrawElements → gldRenderFillTriangles` in `GLRendererFloat`). The
double-buffered present makes the guest thread wait on the GPU fence, so the
software rasterizer caps FPS — CPU-side interpreter wins cannot surface in the
sim for this scene. (On a real device GLES is hardware-accelerated, so that fence
is not the limiter and the interpreter is the more likely bottleneck.)

Two CPU optimizations were measured against this, both via controlled A/B on a
single Release build:

- **CMP + B_COND_THUMB super-instruction fusion** (the data-driven #1 guest
  pair). Correct (8/8 regression on real Thumb code) but FPS-neutral
  (~38.8 ON vs ~38.3 OFF = noise). The interpreter's cost is in instruction
  *execution* (the `LdnStM*`/`LnSWoUB*` load-store helpers, `DataProcessing*`),
  not threaded-dispatch overhead, so removing a dispatch hop changes nothing.
  **Reverted.**
- **LDM/STM block-cursor** (`armstate.h` `block_cursor` +
  `ReadMemory32Block`/`WriteMemory32Block`): a register-list transfer touches a
  contiguous run that almost always sits in one guest page, yet the naive loop
  paid a full dyncom-TLB lookup per word. The cursor resolves the host page once
  and reuses it, re-resolving on page crossings; semantics are identical to
  `ReadMemory32`/`WriteMemory32`. Validated by 1,000,000 difftest cases (5 seeds,
  golden + self-A/B) and 8/8 regression. FPS-neutral in the (render-bound) sim,
  but it cuts real interpreter work in the hottest functions and should help on
  device / in CPU-bound apps. **Kept.**

Conclusion: the dyncom interpreter is at the point of diminishing returns for the
simulator; further FPS work on Snakes needs either hardware-accelerated rendering
(device, or a Metal/HW-GL sim path) or a JIT (off-limits on iOS). Remaining bounded
CPU ideas (route `ReadCode` through the TLB execute slot; block linking) are
predicted neutral *in the sim* for the same render-bound reason.
