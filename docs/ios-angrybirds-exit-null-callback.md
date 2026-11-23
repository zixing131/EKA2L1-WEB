# Angry Birds in-game "X" exit — KERN-EXEC 3 (null callback)

Status: **root cause narrowed, not fixed.** This is a handoff/investigation log. Its
main value is the map of *dead ends* — several very convincing signals turned out to
be red herrings, and a whole feature (a Time Zone server HLE) was built and reverted
because it does not fix this crash.

## Symptom

On the Angry Birds main menu (RM-707 / X7, UID `0x20030E51`; Angry Birds Rio
`0x2003B21F` behaves the same), tapping the game's **own** "X" close button — not the
emulator shell's exit — pops the "Guest error" dialog:

```
Process: angrybirds_20030E51
Exit type: terminate
Category: KERN-EXEC
Reason: 3
```

100% reproducible:

```sh
xcrun simctl launch booted com.eka2l1.emulator \
  -LaunchROMCode rm-707 -LaunchAppUID 0x20030E51 -LaunchKeypadLayout fullscreen
# wait for the PLAY menu, then tap the in-game X (top-right of the guest band):
axe touch -x 398 -y 337 --down --up --udid <sim>   # iPhone 16 Pro, 402x874 pt screen
```

`terminate` + `KERN-EXEC` + `3` is emitted only by `cpu_exception_thread_handle`
(`kernel.cpp`) — i.e. the guest CPU took an unhandled fault (access violation). The
clean shell close path (`closeRunningApp` → `kill(kill,"Closed",0)`) is filtered out
by `guest_fatal_detail` and shows no dialog, so the dialog itself proves the guest
really faulted rather than being torn down.

## The crash instruction

Instrumenting the dyncom interpreter's decode-failure path to dump the register file,
and a memory watchpoint on the faulting store, pin the fault exactly:

- The guest PC is `0x00000000` — it executed a `BLX r12` with `r12 == 0` at AB code
  `0x700774D4`, branched to the null page, and a decoded garbage `STM` faulted writing
  `0xFFFFFFE0`.
- `r12` is loaded from `*(0x00400120)`, which is `0`.

Disassembling AB around `0x70077478` (bytes read live from the running process — the
on-disk `.exe` is byte-pair compressed) shows a textbook GCC **function-local static**
("magic static"), guard at `0x0040011C`, payload at `0x00400120`:

```
LDR  r0,[r4]            ; r4 = 0x0040011C  (guard)
TST  r0,#1 ; BNE skip
BL   __cxa_guard_acquire            ; drtaeabi ord 187 @ 0x804531C4
CMP  r0,#0 ; BEQ skip
ADD  r0,pc,#0x164                   ; r0 = &data @ 0x70077608
BL   0x70000A1C                     ; -> veneer -> slot 0xA20 -> drtaeabi ord 183
STR  r0,[r4,#4]                     ; *(0x00400120) = the BL's return value
BL   __cxa_guard_release            ; drtaeabi ord 188
skip:
LDR  r12,[r4,#4] ; ... ; BLX r12    ; call *(0x00400120)
```

A watchpoint on `0x00400120`/`0x0040011C` confirms, at exit:

```
00000001 -> [0x0040011C]  PC=0x804531CC   ; guard set by __cxa_guard_acquire
00000000 -> [0x00400120]  PC=0x700774A4   ; AB stores 0 into the payload
```

The payload is **never** written during boot, so the static initialises at exit, stores
`0`, and is then called — `BLX 0`.

The `BL 0x70000A1C` was resolved three independent ways to **`__cxa_end_catch`**
(drtaeabi ordinal 183, ROM `0x80451F72`): the AB import table (slot `0xA20` → ord 183),
the Belle SDK `drtaeabi.dso` `.dynsym` (index 183 = `__cxa_end_catch`), and disassembly
of `0x80451F72` (fetch `__cxa_globals`, decrement `handlerCount`). `__cxa_end_catch`
returns the handler count / a 0–1 flag — **not** a function pointer — and returns `0`
in the common last-handler / no-rethrow path. So the guest stores `__cxa_end_catch()`'s
`0` into a slot it then calls as a function pointer. Why a correctly-compiled game does
this, and why the value is `0`, is the open question (see "Where it stands").

## Dead ends (this is the useful part)

Every one of these looked like *the* cause and was disproven:

1. **"The last log line before the crash."** It differs run to run — `Trying to summon:
   baksrvs` / `Loaded eikbackupsrv.dll` one run, `TZSERVER` / `TZDB.DBZ` another. Pure
   correlation with lazy service startup; not causal.

2. **`Unreferencing an already-released IPC message (id 54, function 0x3)`
   (`ipc.cpp:96`).** Appears in every crash and is a genuine double-unref, but it is
   logged *after* the guest has already branched to the null page — it is a teardown
   *effect*, not the trigger. (The unref guard also makes it side-effect-free.)

3. **Missing `C:\private\1020383E\TZDB.DBZ`.** The time-zone DB exists in ROM at
   `Z:\rm-707\private\1020383e\tzdb.dbz` but the native TZ server opens the C: copy,
   which isn't staged. Copying it Z→C removed the file error — **crash unchanged.**

4. **`Can't open object: TZ_GlobalMutex` (`svc.cpp:1924`).** Looks like a failure, but
   the Symbian source (`tzlocalizationdb.cpp` `ConstructL`) does open-then-create:
   `if (OpenGlobal(KTzMutexName)!=KErrNone) User::LeaveIfError(CreateGlobal(...))`. The
   failed open is expected and does not leave.

5. **"A `User::Leave` at exit drives the magic-static into its catch path."** This was
   the leading theory for a long time and is **wrong**. Hooking `__cxa_throw`
   (`0x80452004`, where `User::Leave`/`XLeaveException` goes — verified: boot-time
   leaves do fire the hook) shows **zero throws at exit** once the TZ queries succeed.
   The payload is stored `0` with no exception in flight. So the crash is not
   leave-triggered; the TZ activity around it was a red herring.

## The Time Zone HLE (built, then reverted)

Because empty/failed TZ responses *moved* a boot-time leave into the rules parse, it
looked like completing the TZ contract would fix the exit. A full `!TzServer` HLE was
implemented against the Symbian source
(`~/Developer/symbian/sources/oss.FCL.sf.mw.appsupport/tzservices/tzserver`) and does
get AB to the menu without any TZ leave:

- Opcodes are capability-offset (`KCapabilityNone=0`, `WriteDeviceData=1000`,
  `ReadUserData=2000`, `WriteUserData=3000`).
- `EGetLocalTimeZoneId` (op 0) must return a **serialised `CTzId`**
  `{TUint iReferenceId=2592 (Europe/London), TInt32 size=0}` — `id 0` is
  `KUnacceptableTzId` and makes `CTzId::NewL` panic `TzServer 8`.
- `EGetLocal/ForeignEncodedTimeZoneRulesSize` (op 3/5) → `TInt 8` to arg 3;
  `EGetLocal/ForeignEncodedTimeZoneRules` (op 6/8) → an 8-byte `CTzRules`
  `{int16 startYear=1970, endYear=2100, offset=0, count=0}` to arg 1 (a zero-rule
  ruleset is valid — the client actualises it into one default UTC rule).
- The change notifier (op 10) and its cancel (op 11) must both complete `KErrNone`;
  leaving the notifier pending hangs boot.

**It was reverted.** It does not fix the reported crash (AB already booted fine before
it — the native TZ server only leaves at *exit*, which this proves is irrelevant to the
fault), and it is a shared, all-platforms change that returns "everything is UTC"
placeholder data. The full opcode/encoding details are captured in the agent memory
note `angrybirds-exit-null-fnptr` if the native-TZ-leave gap is ever worth closing on
its own merits.

## Where it stands

The fault is a C++ exception-handling construct, not a Symbian service problem: AB's
exit-time magic-static stores `__cxa_end_catch()`'s return (`0`) as a callback and calls
it, with no active exception. Candidate explanations, all unverified:

- an emulator mismatch in the per-thread `__cxa` exception globals / `handlerCount` vs.
  real hardware (so `__cxa_end_catch` returns `0` where the device returns non-zero);
- a compiler-generated catch landing pad whose `return 0` is clobbered by
  `__cxa_end_catch`'s return value, which happens to be benign on-device;
- a throw that reaches the handler via `__cxa_rethrow` (ord 190) or the unwinder's
  personality routine, i.e. *not* through the `__cxa_throw` we hooked.

Most direct next step: a GDB-stub **write watchpoint** on `0x00400120` (EKA2L1 ships a
GDB stub — `enable-gdb-stub: true`, one-shot `accept()` at boot, connect the first
client with `lldb`'s `gdb-remote localhost:24689`), take a backtrace at the store, and
add hooks on `__cxa_begin_catch` (ord 180) / `__cxa_rethrow` (ord 190) to reconstruct
the real EH flow around `0x700774A0`.
