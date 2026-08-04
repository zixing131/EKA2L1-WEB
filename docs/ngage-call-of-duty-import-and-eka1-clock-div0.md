# N-Gage Call of Duty: uppercase AIF import, then an EKA1 clock divide-by-zero

Getting Call of Duty (the classic N-Gage card game, app UID `0x101FD3F5`,
folder `System/Apps/6R48`) to run surfaced two independent bugs: the folder
importer refused a perfectly valid card, and once it imported, launching the
game panicked during startup. Both turned out to be general emulator behavior,
not title-specific quirks.

## Part 1 — "registration file not found" on a valid card

Selecting the Call of Duty folder failed with `ngage_game_card_no_game_registeration_info`
(error 3), even though the registration file was plainly present. Ashen, which
had been fixed earlier, imported fine.

The difference was one byte of casing. `find_singular_ngage_game()` locates the
per-app registration file by building `<appfolder>/<appfolder>.aif` and calling
`common::exists()`. Ashen ships `6R21/6R21.aif` (lowercase extension); Call of
Duty ships `6R48/6R48.AIF` (**uppercase** `.AIF`). On the case-insensitive
simulator/host filesystem `exists()` resolves the mismatch, which is why the
simulator never reproduced it — but on a real iOS device (case-sensitive APFS),
and on any case-sensitive filesystem, the hardcoded lowercase `.aif` never
matches the on-disk `.AIF`.

The surrounding installer already resolves `System` and `Apps` case-insensitively
via `find_case_sensitive_file_name()`; the AIF lookup was the one spot that
didn't. Fixed by falling back to the same case-insensitive lookup for the
registration file's real name when the exact-case path is absent.

## Part 2 — KERN-EXEC 3 during view activation

With the card imported, launching the game raised an unhandled `EExcType 1`
(`EExcGeneral`) → KERN-EXEC 3, a few log lines after the guest opened its ETel
Voice1 line. The ETel line and the flood of missing `NULL.wav` opens (the game's
"no sound" sentinel) were both red herrings.

### Narrowing it down

The panic is a guest `User::RaiseException`, so a temporary hook chain in the
dyncom interpreter walked the real path:

- `set_exception_handler` was never called and `exc_handler == 0` at the raise —
  so the game raises `EExcGeneral` with no handler installed. On real hardware
  this is also KERN-EXEC 3, meaning the raise is an *error/abort path*, a
  symptom, not the cause.
- A stack scan first suggested a game "assert dispatcher" frame, but a PC hook
  at that address never fired — it was a **stale stack frame**. Stack scanning
  on ARM without frame pointers is noisy; only PC hooks that actually fire (and
  return addresses whose preceding instruction is a real `bl`) can be trusted.
- Hooking euser's raise stub and its caller showed the raise comes out of
  euser's integer-divide helper: the game does `bl __divsi3` with divisor `0`,
  and this euser's `__udivsi3` raises `EExcGeneral` on a zero divisor.

Reading the live registers at the faulting divide gave the whole story:

```
dividend = 100000       ; frameDelta(=1) * 100000
divisor  = 0            ; elapsedMs
t_prev   = 63953940922001362   (µs TTime)
t_now    = 63953940922001526
elapsed  = 164 µs
```

The game's startup rate calc is `frameDelta * 100000 / (elapsedUs / 1000)`. It
guards on `elapsedUs != 0`, but then divides by **milliseconds**. When
`0 < elapsedUs < 1000`, the millisecond divisor truncates to `0` and the guard
never catches it → divide-by-zero → `EExcGeneral`.

### Why real N-Gage survives and EKA2L1 didn't

The two timestamps are `User::UTCTime`/`HomeTime` reads. On **EKA1** (Symbian
6.1 / Series 60 v1, the N-Gage's kernel) the system clock is only refreshed from
the 64 Hz tick ISR, so `HomeTime` advances in whole tick periods (15.625 ms).
Two reads within one tick return the *same* value → `elapsedUs == 0` → the
game's guard skips the divide; reads across ticks are ≥ 15625 µs → milliseconds
≥ 15. The value is never in the fatal `(0, 1000)` µs window.

EKA2L1's `universal_time()` is `base_time_ + timing_->microseconds()` — a
continuous host-derived microsecond clock. It therefore hands out sub-millisecond
deltas (164 µs here) that slip past the guard. This is not an EKA2 concern:
Symbian 9 / ^3 / Anna / **Belle** back the clock with the fast counter and are
genuinely fine-grained, so their behavior already matches EKA2L1.

### Fix

Quantize the guest-visible clock to the tick period, gated on `is_eka1()`:

```cpp
std::uint64_t kernel_system::universal_time() {
    const std::uint64_t raw = base_time_ + timing_->microseconds();
    if (is_eka1()) {
        const std::uint64_t tick_us = common::microsecs_per_sec / epoc::TICK_TIMER_HZ;
        return raw / tick_us * tick_us;
    }
    return raw;
}
```

This matches real EKA1 hardware and keeps the frame-to-frame delta at either 0 or
one full tick, so the whole class of "game divides by a sub-tick elapsed time"
bugs disappears on EKA1 devices while EKA2 is left untouched. Call of Duty now
boots to its main menu.
