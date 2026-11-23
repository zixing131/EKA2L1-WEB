# The Clock application never draws: two servers that stop answering

## Symptom

On the 5320 (RM-409, S60 3rd FP2) ROM, opening the built-in **Clock** application
gives a permanently black screen at 0 FPS. The emulator itself stays alive and
responsive; only the guest application is stuck.

## First stop: the alarm server swallows a request

The log tail is unambiguous:

```
E services/src/alarm/alarm.cpp:71 [Service.Alarm]: Unimplemented opcode for Alarm server 0x9
```

and nothing follows it. `alarm_session::fetch` had a `default:` branch that logs
and then simply `break`s — the IPC message is never completed. Clock issues this
one through `RASCliSession::GetAlarmIdListForCategoryL`, which is a plain
synchronous `SendReceive`, so the application thread parks forever inside the
kernel. That is the black screen: Avkon had already asked AknCapServer to blank
the screen during startup (`akns_blank_screen`), and the app died before it could
unblank and draw anything.

Opcode 9 is `EASShdOpCodeGetAlarmIdListForCategory` (`ASShdOpCodes.h`, counting
from `EASShdOpCodeFirst = 0`). Reading both sides of the contract shows it is the
same shape as opcode 11, which EKA2L1 already implemented: the server streams the
ID array into its transfer buffer and writes the buffer's byte size back through
slot 1, then the client fetches the buffer with opcode 21. The category /
state argument only filters a queue this emulator never populates, so the three
list requests (9, 11, and 12 = `GetAlarmIdList`) collapse into one handler.

A useful intermediate step, but *not* a fix: completing the unknown opcode with
`KErrNone` without writing slot 1. The client then reads an uninitialised size
and the app dies outright — which at least proved the hang was here and revealed,
in a single run, that opcode 9 was the only alarm opcode Clock needed.

## Second stop: Clock waits for a NITZ module that can't start

With the alarm request answered, Clock got further and then exited *cleanly* —
back to the app list, no panic. A probe in `process::kill` confirmed
`exit type kill, reason 0, category None`, i.e. `E32Main` returned normally, and
a stack scan at thread-kill time showed only the ordinary `E32Main` return chain.
Neither told us why.

What did tell us was an IPC trace tagged with the calling process
(`log-ipc: true`, plus a custom `log-filter` that keeps `Service.Track` at
`info` — the iOS frontend otherwise downgrades the debug preset, which pins
`Service.Track` at `error` and hides every `Calling service:` line). Clock's last
actions before teardown:

```
!AlarmServer 9 / 21                      from Clockapp
Can't open object: ClkNitzMdlStartSemaphore   (x6)
Loader::LoadProcess -> ClkNitzMdls.exe   from Clockapp
Can't open object: ClkNitzMdlStartSemaphore
!EtelServer 24011                        from ClkNitzMdls   <- unimplemented
!EtelServer 22022                        from ClkNitzMdls   <- unimplemented
!EtelServer 22008                        from ClkNitzMdls   <- unimplemented
```

Clock starts `ClkNitzMdls.exe` (the network-time module) and waits for it to
signal `ClkNitzMdlStartSemaphore`. That module opens the phone and immediately
asks for network time — and all three requests hit the same never-completing
`default:` branch in `etel_phone_subsession::dispatch`. The module never reaches
the point where it signals, so Clock gives up and quits.

The opcodes (`etelmmcs.h`) are:

| opcode | request | shape |
| --- | --- | --- |
| 24011 | `EMobilePhoneGetCurrentNetworkNoLocation` | sync fetch, network info in slot 0 |
| 22008 | `EMobilePhoneGetNITZInfo` | sync fetch, `TMobilePhoneNITZ` in slot 0 |
| 22022 | `EMobilePhoneNotifyNITZInfoChange` | async notification |

`GetCurrentNetworkNoLocation` is literally `GetCurrentNetwork` with the
location-area slot omitted — the SIM TSY implements it as
`return GetCurrentNetwork(aReqHandle, aPckg1, NULL)` — so it routes to the
existing handler. (That handler also read a slot-2 pointer it never used;
dropping the dead read is what makes sharing it safe.) `GetNITZInfo` fails with
`KErrNotFound`, which is how a TSY reports "the network has not broadcast a time
frame", and the change notification is parked in a `notify_info` so its cancel
opcode behaves.

### Dead end worth avoiding

Blanket-completing the etel `default:` branch with `KErrNotSupported` looks like
the obvious general fix and is worse than the disease: `ClkNitzMdls` re-issues
`GetCurrentNetworkNoLocation` the instant it fails, so the guest spins — 1.7
million retries in 30 seconds, starving the renderer. A stubbed request that a
client polls has to return a *plausible answer*, not an error. The same reasoning
is why the alarm server's `default:` branch was left alone rather than made to
complete: turning "hangs" into "fails" is not automatically progress.

## Fix

- `alarm_session` handles opcodes 9, 11 and 12 with one `stream_alarm_id_list`.
- `etel_phone_subsession` handles 24011 (via `get_current_network`), 22008 and
  22022 / 22522.

Clock now boots to its Time view with the current date and "(no alarms set)".

## Epilogue: the six seconds it then spent doing nothing

Once it worked, Clock took visibly longer to appear than any other application on
the same ROM. Measured on a Release simulator build (RM-409), time from launch to
a drawn UI: Clock 13.1 s, Calculator 5.4 s. Subtracting the ~4 s the emulator
spends booting the ROM leaves 8.7 s of application startup against Calculator's
2 s.

Timestamping the log (`spdlog::set_pattern` with `%H:%M:%S.%e`, temporarily)
placed it exactly:

```
20.611  Can't open object: ClkNitzMdlStartSemaphore
21.613  Can't open object: ClkNitzMdlStartSemaphore     ... six times, 1.00s apart
25.623  Can't open object: ClkNitzMdlStartSemaphore
26.626  Trying to summon: ClkNitzMdls.exe
```

Six seconds of it, plus one more waiting for `TZ_GlobalMutex` after the ROM's
TZSERVER is summoned. A probe on the `after` SVC showed the guest itself asking
for `User::After(1000000 us)` and the emulator delivering 1.002 s, so this is not
a timer-granularity problem: Clock polls for the NITZ module six times at 1 Hz,
gives up, starts `ClkNitzMdls.exe` itself, and proceeds without waiting for it.
A semaphore-creation probe caught the module publishing the object microseconds
*after* Clock's last probe read it — the poll was pure loss.

On real hardware that loop costs nothing, because `ClkNitzMdls.exe` is already
running: the S60 System Starter (`z:\sys\bin\startup.exe`) launches it at boot.
EKA2L1 runs no boot sequence at all — every process exists only because some
guest asked the loader for it — so any application that probes for a system
daemon pays its own timeout on every single launch.

HLE'ing the module was considered and rejected. Its surface is genuinely small
(one server, `ClkNitzMdlServer`; six messages during a whole Clock startup:
connect ×2, a synchronous opcode 1 ×2, an asynchronous opcode 3 ×2), but the
semantics are undocumented — the S60 Clock and its NITZ model are not in the
Symbian Foundation tree — so opcode 1 would have to be reverse-engineered out of
an 8.8 KB ROM binary, and the real module already works here. HLE would trade
working guest code for guesswork and buy nothing that being started earlier does
not.

So `handle_open_object` now starts the daemon when its start object is probed and
missing, from a small table of ROM system daemons. The spawn is guarded against
a second copy by scanning live processes for the same executable, which matters
because the probe that triggers the spawn is followed 2 ms later by another one.
Startup drops from 13.1 s to 6.5 s; the NITZ block itself goes from 6 s to 2 ms.
The remaining second is the TZSERVER mutex poll, which this mechanism cannot help
with — that daemon is already starting when the client probes for it.
