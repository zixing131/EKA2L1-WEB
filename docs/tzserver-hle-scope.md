# Why the time zone server is HLE'd, and why only from Symbian^3 up

`!TzServer` is provided by EKA2L1's own implementation for `epocver::epoc95` and
newer, and by the ROM's `TZSERVER` binary everywhere else. That split looks
arbitrary. It is not, and both halves of it took measuring to establish.

## The HLE exists because the ROM server is transient by design

The commit that introduced it (`a5123e6be`) recorded the symptom — Angry Birds
Rio on the X7 held its menu at ~13 FPS instead of 60, because querying local time
restarted `TZSERVER` about 53 times a second — but attributed it to the server
"never remaining connectable". Instrumenting the kernel with probes on process
exit, file open, `RServer2` creation and `CreateSession` shows that description is
wrong on every count:

```
03.549  angrybirds connect '!TzServer' -> MISSING
03.549  Trying to summon: TZSERVER
03.592  Can't open object: TZ_GlobalMutex
03.595  TZSERVER creates server '!TzServer'
03.595  angrybirds connect '!TzServer' -> FOUND      <- the session is established
03.601  TZSERVER exits (kill, reason 0, category None)
03.601  angrybirds connect -> MISSING                <- next query, from the top
```

The server starts, initialises, publishes `!TzServer`, accepts the session,
serves it, and then exits of its own accord six milliseconds later. From
`timezoneserver.cpp`:

```cpp
void CTzServer::SessionClosed() const
    {
    --iSessionCount;
    if (iSessionCount == 0)
        {
        CActiveScheduler::Stop();
        }
    }
```

There is no shutdown delay at all — not even the couple of seconds most transient
Symbian servers grant themselves. The last session closing stops the scheduler,
`E32Main` returns 0, the process dies. Angry Birds reaches `RTz` through Open C's
`localtime()`, which opens and closes a session per call, so *on hardware too*
every query restarts the server. Hardware can afford it; EKA2L1 cannot. Measured
here: 46 ms for the first spawn, 13 ms for each subsequent one once the codeseg
cache is warm. At 53 queries a second that is the entire frame budget.

So there is no startup bug to fix. The three ways out are: make guest process
creation an order of magnitude cheaper (13 ms is already the cached figure, and
the remainder is process/chunk/thread setup plus the server's own init — four
file opens, a DBMS connection, a 30 KB chunk copy); keep a session open so the
server never sees a zero count (only a guest thread can hold one, so the host
side cannot); or replace the server. The HLE is the third.

Note that this is specific to clients that connect per query. On the 5320 the ROM
`TZSERVER` stays up perfectly well, because Clock and the NITZ module hold their
sessions open for the lifetime of the application.

### Two dead ends worth not repeating

`TZSERVER` logs `Trying to open a non-existent file: C:\private\1020383E\TZDB.DBZ`
on every start. That is a red herring: `readonlytzdb.cpp` tries the flash copy
first and falls back to the ROM one by design —

```cpp
TInt error = CopyDatabaseToRam(KTzDbFileNameFlash);   // C:\private\1020383E\TZDB.DBZ
if (error != KErrNone)
    User::LeaveIfError(CopyDatabaseToRam(KTzDbFileNameRom));  // Z:\...\TZDB.DBZ
```

— and the file probe confirms the server goes on to open the `Z:` copy every
time. Seeding a writable copy on `C:` would change nothing.

The other dead end is the "never remained connectable" reading itself. The
session probe shows the connect succeeding on every single cycle; chasing a
connection failure means chasing something that does not happen.

## It cannot simply be lowered to S60v3

The 5320's Clock pays one second per launch polling `TZ_GlobalMutex` while the
ROM `TZSERVER` comes up, which the HLE would remove. Lowering the condition to
`epocver::epoc93fp1` does remove it — the server is never summoned and the poll
disappears — and breaks the Clock outright:

```
TZPROBE opcode 21 from ClkNitzMdls[100059a8]0001
Unimplemented TZSERVER opcode 21                 <- answered KErrNotSupported
Can't open object: ClkNitzMdlStartSemaphore      <- the module died; Clock restarts it
Trying to summon: ClkNitzMdls.exe
  ... seven rounds of this ...
Unimplemented opcode for OOM AKNCAP server: 0x2   <- Clock tears itself down
```

Opcode 21 is the only thing the FP2 ROM sends to `!TzServer` during a Clock
startup, and it is not in EKA2L1's table. Adding it would be guessing on top of a
misaligned table: EKA2L1's opcodes were written against the Symbian^3
`CTzServer::TFunctionCode`, where 21 is `ESwiObsBegin`, a software-install
observer entry point. Even under the obvious "FP2 lacks the two Olsen entries, so
everything from index 4 shifts down by two" hypothesis, 21 lands on `ESwiObsEnd`.
A NITZ module calling either is nonsense — what it would plausibly want is
`ESetTimeZone` (1000), `EEnableAutoUpdate` (1001), `ENotifyHomeTimeZoneChanged`
(1002) or `EAutoUpdate` (16). The FP2 opcode space is simply not the Symbian^3
one, and what is missing is not an opcode but a validated mapping.

The enum cannot be looked up either. `CTzServer::TFunctionCode` lives in
`timezoneserver.h`, an `@internalComponent` server header that ships in none of
the S60 3rd FP2, S60 5th or Symbian Belle SDKs — `findstr /s` for
`EGetLocalTimeZoneId` across all three `epoc32\include` trees returns nothing, and
so does a search for `Olsen`, including in Belle where those entries certainly
exist. All three SDKs do ship a WINSCW `tzclient.dll`, but Symbian DLLs export by
ordinal (the name table holds only `_E32Dll`) and these are stripped to an
external PDB, so there is no symbol to anchor a disassembly to and the constants
are not recoverable by inspection.

Recovering FP2's numbering would mean disassembling the `ServiceL` switch out of
the ROM's own `tzserver.exe` and keeping a second opcode table keyed by
`epocver`. Until someone does that, `>= epocver::epoc95` is the honest bound, and
the 5320 keeps its one-second poll.
