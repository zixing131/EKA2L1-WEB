# Dragon World slows down while waiting for its multiplayer peer

After repairing N-Gage Bluetooth selection, Dragon World reached its two-player
desert stage but ran at roughly 8–10 FPS between the iPhone Air and the Mac's
iOS simulator. The user confirmed 30 FPS in the same Air's single-player stage.
The investigation separated this from the earlier connection failures: device
selection and discovery happen before the continuous game-data connection.

## Observations

All comparisons used Release builds and the dyncom guest CPU interpreter.
Screenshots of the two-player stage, with both dragons and the 1P/2P HUD,
established actual gameplay rather than a successful socket connection.

| Path | Displayed FPS | Other evidence |
|---|---:|---|
| Air, single player | 30 | User's physical-screen observation |
| Simulator + Air over Wi-Fi | 8–10 | Simulator guest thread idle in 2460/3104 samples (79%) |
| Two simulators over loopback | 29–30 on both | Guest thread running in 1574/3033 samples (52%); both stages continued progressing |

The loopback pair used distinct discovery ports and virtual-port offsets so
that both processes could run on one host. It used the same N-Gage ROM and
Dragon World application. The final screenshots show enemies, projectiles,
changed scores and matching player scores exchanged between the two peers.
There were no game patches or debugger changes in this comparison.

A diagnostic breakpoint trace identified the recurring game-data pattern:
small four-byte writes, a receive request, completion with the peer's data,
and the next timer/iteration. The trace also found a 66,666-microsecond guest
timer in both single-player and multiplayer. That timer alone cannot explain
the reduction: single-player and loopback multiplayer both display 30 FPS.
The FPS overlay counts submitted screen frames, not guest timer callbacks.

Breakpoints themselves substantially slowed the program, so their timings
are not used as performance measurements. The sample profiles and screenshots
above were collected without an attached debugger.

The host's TCP statistics give supporting evidence, not a one-way network
latency measurement. During Air gameplay one sample reported an average RTT
of 46.34 ms with 43.81 ms variation, compared with 1.22 ms and 0.19 ms during
loopback gameplay. TCP acknowledgement behavior and peer scheduling contribute
to these numbers; they are not a measurement of Wi-Fi propagation alone.
Direct UDP virtual-address queries after the first warm-up reply mostly took
6–13 ms. Discovery round trips are not the same workload as game synchronization.

## Conclusion and limits

The game can sustain approximately 30 displayed FPS in two-player mode through
the emulator's socket path. The low-rate physical-peer run spends much more
time waiting for the next event instead of doing guest CPU work. Its repeated
send/receive dependency makes game progress sensitive to peer turnaround and
wireless-path delay. There is no evidence of a universal multiplayer frame cap
or a saturated simulator rendering thread.

This does not isolate every part of the physical peer's turnaround time. A
usable device-side Time Profiler capture was not available over the current
wireless developer connection, so Air scheduling and Wi-Fi behavior are not
separately quantified. There is no validated frame-rate fix in this change.

Nagle was also considered. Bluetooth sockets request TCP_NODELAY, the LAN
accept hook applies it to accepted handles, and a local Darwin socket check
confirmed that an accepted socket inherits the listener's enabled option.
Changing that option without further evidence would not explain the tested
LAN-server case. Likewise, changing the guest's timer or inventing early
receive completions would alter the game protocol rather than address a
proven emulator defect.
