# Final Battle's intro subtitles fly past unreadably

> **One-line conclusion**: the intro is driven by a bare `RTimer::After(1us)` loop.
> EKA2 queues this on its nominal 64Hz tick queue; EKA2L1 completed it at a
> microsecond-scale minimum, making a callback-driven sequence run too quickly.

## Symptom

The Final Battle (`uid=0xA0003C62`, 5320/rm-409) plays a black-screen subtitle intro
after *Start game*. Every line ("Once upon a time...", "Naah, what a cliché.", "A long
time ago in a far away galaxy...") flashes by in well under a second; sampling the
screen twice a second only ever caught one line, and the game reaches the "What do you
do?" prompt a few seconds after the intro begins.

## Narrowing it down

**The game's time API surface.** `fbattle.exe` is deflate-compressed, so its import
table was read with the emulator's own `loader::parse_e32img`, and the ordinals
(stored as `code[offset]`, low 16 bits) were symbolised against the Belle SDK's
`euser.dso`. Everything time-related the binary can reach: `User::After`,
`User::TickCount`, `CTimer::After`, `CPeriodic::NewL/Start`, `HAL::Get`, plus
`MediaClientAudioStream` for sound. Nothing else — no clock service, no video.

**The emulator clock was not drifting.** `ntimer` reads a host wall-clock teletimer,
and `User::TickCount`, `HAL` `ESystemTickPeriod` and `timer::after_ticks` all agree on
`TICK_TIMER_HZ = 64`. A guest measuring elapsed time would have measured it correctly.

**What the intro actually executes.** Running the intro once with `log-svc: true`
produced ~10MB of SVC trace per second. Over a 6-second window the guest issued 160643
`timer_after` calls, matched one-for-one by `wait_for_any_request` and
`active_scheduler` — the intro is nothing but a timer → callback → timer loop, about
27k iterations per second. No other SVC comes close (the next entry is 1184 `heap`
calls).

**The requested interval.** A temporary probe in the `timer_after` bridge printed the
argument: 1.25 million calls during the intro, every single one
`RTimer::After(1us)` from the `FBattle` thread. The game asks for the shortest
possible timer and paces itself on how often the request completes.

**The source contract.** SymbianSource's `kernel/eka/euser/us_exec.cpp` and
`kernel/eka/kernel/stimer.cpp` show that `RTimer::After` reaches
`MicroSecondsToTicks(aTime, ETrue)`. The queue's nominal period is 15625us. Zero
becomes one microsecond, and the elapsed nanokernel time since the last queue tick
contributes to rounding. The queue itself is driven by millisecond timers with
rounding-error compensation. An ideal 64Hz grid is therefore a useful emulator
model, not an exact reproduction of hardware's sub-tick timing.

The original trace demonstrates that the emulator completed tiny timers at tens of
thousands of requests per second. This is inconsistent with a tick-queued timer and
explains why callback-driven subtitles advance too quickly. It does not establish
an exact hardware speed ratio or verify the game's timing on a physical phone.

**Two contracts missed in the first implementation.** EKA2 `RTimer::AfterTicks(n)`
sends `-n` through the same `Exec::TimerAfter` entry. Negative kernel arguments are
valid tick counts, even though negative public `RTimer::After` intervals panic in
the client. Converting this argument directly to `uint64_t` loses its meaning and
can turn a one-second tick request into a near-immediate completion.

`TTimer::AfterHighRes` first rounds the interval up to nanokernel ticks, then
`NTimer::OneShot` queues it relative to the next tick. Rounding the duration alone
misses this phase wait. In particular, zero still waits for the next nanokernel
tick; it must not fall through to the emulator's 30us minimum.

## Implementation

The timer now submits an absolute deadline to `ntimer`, so queue insertion does not
read the clock again and shift the computed boundary. Tick-based and high-resolution
requests bypass the legacy 30us minimum; other uses of `timer::after` retain it.

The EKA2 interval remains signed through decoding. Positive microseconds and
negative tick counts use the nominal global 64Hz grid, with wide arithmetic before
negation and multiplication. The EKA1 separate tick-count executor uses that same
model. The high-resolution path rounds the interval to milliseconds and counts
from the next millisecond boundary, including for a zero interval.

The EKA1 choice is a compatibility assumption consistent with the emulator's
reported tick period. The available EKA2 source does not independently establish
the exact EKA1 implementation. Likewise, the earlier Dragon World observation
(28-30 FPS becoming 24-26 FPS) is a behavioral change to investigate against hardware,
not proof of correct device timing.

`RTimer::At`, `RTimer::Lock`, and `User::After` keep their existing timer policies.
This change does not reproduce the hardware tick queue's millisecond rounding
jitter or change the existing completion/cancellation lifetime handling.

## Verification

The revised implementation has deterministic fixtures for zero and one-microsecond
requests, both sides of tick boundaries, EKA2's signed tick encoding, the 32-bit
interval limits, and zero/nonzero high-resolution intervals. Expected periods and
ABI encoding come from the original Symbian sources, not EKA2L1's constants.
The full host suite passed: 220 test cases and 5796 assertions, both on the fork and in the PR worktree.

Removing each behavior independently and recompiling the focused test executable
produced the expected failures:

| Behavior removed | Result |
| --- | --- |
| Ordinary timer tick grid | 2 test cases fail |
| EKA2 signed tick decoding | 1 test case fails |
| HighRes nanokernel queue phase | 1 test case fails |
| Absolute deadline preserved by queue insertion | 1 case fails |
| All restored | 4 test cases, 34 assertions pass |

The final Release simulator build passes the default 12-check suite (Final Battle,
Calculator, N95 Calculator, and string catalog). Screenshots confirm Final Battle
reaches its first choice prompt, and Calculator accepts input and opens/closes its
Options menu. Angry Birds passes all five touch checks. Dragon World on N-Gage
(nem-4) navigates title, main menu, difficulty, level selection, and rendered gameplay without a guest
panic; sampled title/menu FPS remains in the mid-twenties. The original report's subtitle durations and EKA1 FPS observations
are not measurements of this revision.

The upstream-based PR worktree also builds successfully for Release iOS. Runtime
simulator checks above used ios-next with the same timer changes.

Source references:

- [Client timer requests](https://github.com/SymbianSource/oss.FCL.sf.os.kernelhwsrv/blob/master/kernel/eka/euser/us_exec.cpp)
- [Kernel timer conversion and tick queue](https://github.com/SymbianSource/oss.FCL.sf.os.kernelhwsrv/blob/master/kernel/eka/kernel/stimer.cpp)
- [Nanokernel timer queue](https://github.com/SymbianSource/oss.FCL.sf.os.kernelhwsrv/blob/master/kernel/eka/nkern/nk_timer.cpp)

## Dead ends worth skipping

- Suspecting the emulator's notion of time (`TickCount`, tick period, HAL): the
  teletimer is real wall-clock and every constant already matched hardware.
- Suspecting the intro was voice-paced: the game does open 36 `.wav` files, but they
  are 0.4-1.6s effect samples opened in bulk at startup, not per-line narration.
- `simctl io ... recordVideo` is variable-rate — a 45-second capture is written as a
  ~10s file — so it cannot be used to measure how long a guest screen state lasts.
  Timestamped `simctl io screenshot` sampling can.
