# Angry Birds Rio's main menu repeatedly starts TZSERVER

## Symptom

On the RM-707/X7, Angry Birds Rio (`0x2003B21F`) runs its central-play-button
main menu at about 13 FPS. The episode selection screen immediately returns to
59–60 FPS, and returning to the main menu restores the slowdown.

This is not the original Angry Birds (`0x20030E51`). An initial trace of that
title was discarded after the UID mismatch was noticed.

## Narrowing

The slow screen adds roughly 2,100 emulator log lines in ten seconds:

- about 53 failed attempts per second to start `TZSERVER`;
- one failed `TZ_GlobalMutex` lookup for each attempt;
- two failed opens of `C:\private\1020383E\TZDB.DBZ` per attempt.

The episode selection screen adds no log lines over the same interval. Host
sampling agrees with that boundary. On the main menu, the guest thread spends
most of its time in synchronous SVC/IPC work, `loader_server::load_process`,
`kernel_system::spawn_new_process`, and filesystem path resolution. On the
episode screen those stacks disappear and the samples instead contain the
expected EGL swap and draw work.

This rules out an expensive menu animation, shader compilation, and the OpenGL
backend as the primary bottleneck. The lower host CPU use on the 13 FPS screen
is also consistent with the guest being gated by synchronous service startup
rather than saturating the renderer.

The missing writable time-zone database is a secondary problem. Copying the
ROM database from Z: to the expected C: path removes the failed file opens and
roughly halves the log traffic, but the `TZSERVER`/`TZ_GlobalMutex` loop remains
at the same rate and the menu remains at about 13 FPS.

The Symbian source confirms the intended startup contract. `RTz::StartServer`
creates `TZSERVER`, rendezvouses with it, and waits for startup or process
death. The server constructs `CTzServer`, registers the server name, then
completes the rendezvous. Its localization database first calls
`OpenGlobal("TZ_GlobalMutex")` and creates the global mutex if it is absent.
Therefore one failed mutex open during the first launch is normal; seeing it
more than fifty times per second proves that the native server never becomes a
persistent, connectable service.

## Fix

Rio queries local time continuously on this particular menu. EKA2L1 does not
satisfy the native `TZSERVER` startup/lifetime contract, so every query follows
the synchronous start-server path again. Repeated guest process creation,
executable and filesystem lookup, IPC, and logging starve the game thread and
cap presentation near 13 FPS. The episode screen stops making the query, so
the retry storm ends and rendering returns to 60 FPS.

The fix supplies an HLE `!TzServer` on Symbian 9.5 and newer. It follows the
original `RTz` IPC contract rather than special-casing Rio:

- the host's system time-zone name is read at runtime;
- the selected ROM's `tzdb.dbz` maps that IANA name to the database-specific
  Symbian numeric ID, including linked names;
- local ID, UTC/local conversion, UTC offsets, DST state, encoded rules, and
  notifier/cancel requests use their original descriptor slots and
  serialization formats;
- time-change notification remains asynchronous and its retained IPC context
  is completed only while the requester thread is still alive;
- unsupported requests complete with an error instead of leaving a guest
  message pending.

Host tzdata provides the real UTC offset and DST transitions. A client can ask
for the whole Symbian year range (0–9999), as Rio does. Walking that range at
runtime initially made the HLE itself appear hung. The rule generator therefore
keeps the requested range in the `CTzRules` header but searches concrete host
tzdata transitions only from 1900 through 2100, the useful historical and
forecast window. This keeps the response bounded while retaining real rules
around all practical dates.

The service resolves the numeric ID on first connection rather than at HLE
construction time. This matters because the selected ROM's Z: drive is mounted
after custom services are created; resolving earlier saw the initial fallback
device and returned no ID.

## Result

On the test host the final Release build reports `Asia/Shanghai`, guest ID
2136, and UTC offset +480 minutes from the RM-707 database. Rio's main menu and
episode selection both hold 60 FPS. The main menu produces no `TZSERVER`
process-start attempts or `TZ_GlobalMutex` retries, and the service reports no
unimplemented requests.

The installed and no-reinstall iOS control suites both pass all 12 checks
across Final Battle, the 5320 Calculator, and the N95 Calculator. The original
Angry Birds touch regression passes all five checks, including play-button and
episode-carousel input. A macOS build of the shared `epocservs` target also
passes.
