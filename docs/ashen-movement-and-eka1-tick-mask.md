# Ashen movement timing and the Worms EKA1 tick mask

[Upstream issue 577](https://github.com/EKA2L1/EKA2L1/issues/577) is reproducible
on the N-Gage NEM-4 V 4.03 guest. The character continues moving after releasing
the direction key. The effect is much clearer with Dynarmic than with Dyncom.

The investigation used `ios-next` at `5d2134f0c`, Release simulator builds, a
60 FPS limit, and the original Ashen v1.06 MMC package. Its `6r21.app` SHA-256 is
`e4faf7c1e2304e1d5c64ed8249576bf1957e4d2fb87e6182d1613e26fbde4d6c`.

## The clock change fixes Ashen

`epoc::tick_count()` divides elapsed microseconds by 15625, then clears bit 0
for every EKA1 process. The counter therefore changes every 31.25 ms, in steps
of two, while retaining a nominal rate of 64 tick units per second. Closely
spaced reads can return the same value. This is reduced resolution, not a
clock running at half speed.

In four alternating forward/backward trials, each direction was held for one
second before release. Screenshots at fixed delays showed:

| Clock and CPU | Observed stopping time after release |
|---|---|
| Original mask, Dyncom | About 0.4 s in the initial control trial |
| Original mask, Dynarmic | About 1–1.5 s across four trials |
| Full counter, Dynarmic | By about 0.4 s in all four trials |

The first comparison changed only the native masking instruction through LLDB.
A source build removing the mask reproduced the improvement in another four
trials. Game files, physics, controls, frame limit, and the underlying time
provider were unchanged. Stationary pictures still contain small changes from
particles and the weapon; the bounds above describe visible scene motion, not
instrumented velocity measurements.

The candidate Release binary had SHA-256
`a64e75056de55babd5efdfa4386bedc47b1a75e5c067403d8a7ea694f3fcd7e0`.
The default iOS regression suite passed all 12 assertions, and Highway 3D on
NEM-4 also rendered its 3D scene. These controls did not establish EKA1 Worms
compatibility.

## Removing the mask exposes a separate Worms startup failure

The mask was introduced in `5967f828da8c70a65f96e432c326764b8b026367` as an
unverified Worms World Party workaround. Testing the EKA1 MMC package
(`6r39.app`, UID `0x101FD3F3`) confirms that the workaround still matters:
the full counter causes a startup `KERN-EXEC 3` panic. Temporarily restoring
even tick results during startup lets the title/demo run. The N-Gage 2.0
edition does not cover this path.

The tested `MOPHUN.DLL` SHA-256 is
`2a48c68ad6eba56ae6959d23f1fc9fb0f680869687022cc3bb7405b075978268`.
Its clock/yield helper at link address `0x1001b34c`:

1. Calls `User::TickCount()` and saves the result.
2. Tests bit 0.
3. For an odd result, reads the cached environment pointer at object offset
   `0xd0` and calls CONE export 126.
4. Returns the original tick count.

That export is `CCoeEnv::IsWservEventPending()`. The
[Symbian implementation](https://github.com/SymbianSource/oss.FCL.sf.mw.classicui/blob/master/lafagnosticuifoundation/cone/src/COEMAIN.CPP)
tests its active-object request status against `KRequestPending`. The N-Gage ROM
implementation starts by loading that status from `[r0 + 4]`.

The failing runtime call had guest PC `0x5091767c`, R0 `0`, LR `0xe011b36c`,
and a read fault at `0x4`. The stack passes through Mophun program loading and
VM execution. The later start/setup method obtains `CCoeEnv::Static()` and
stores it at object offset `0xd0` at link address `0x100117f0`; this has not
happened during the failing load. The cached pointer is therefore null when
the odd-tick check first uses it. Clearing the clock bit hides this path.

A diagnostic MMC copy replaced the cached-pointer load at `0x1001b364`
with a call to the existing `CCoeEnv::Static()` import. This avoided the null
call, but later repeated title-to-menu checks did not establish a reliable
repair. The diagnostic card is not part of the retained change.

The environment object and Mophun's cached pointer have different lifetimes.
The UI framework creates a `CEikonEnv`/`CCoeEnv` before constructing the
application UI. In the available Symbian sources, `CCoeEnv` publishes itself
to CONE's thread-local storage in its constructor, and `ConstructL()` then
creates the scheduler, server connections, screen resources, and event request.
`CCoeEnv::Static()` retrieves that thread-local pointer; it does not create an
environment. The successful early-`Static()` diagnostic therefore shows that
the environment was already available during Mophun loading, while the private
cache at `0xd0` was still null. The store at `0x100117f0` initializes that cache,
not the actual `CCoeEnv` object.

## What the ROM establishes, and what remains open

The examined N-Gage ROM SHA-256 is
`afc1cc8b693f2b68d23ca203aa4d4eb4266baad4ab718f013a33b915d39574ef`.
Its `User::TickCount()` export reaches SVC `0x70`. The kernel handler at
`0x50333d0c` directly reads the counter at `0x80000488`, and the tick processing
loop increments it by one at `0x50322548`. Neither path clears bit 0.
The SDK contract describes a hardware-dependent tick counter, usually 64 Hz;
it does not promise that results are even.

This static evidence does not measure which counter transitions a physical
N-Gage user thread observes under interrupt/DFC scheduling. It also does not
establish why the tested Mophun initialization order works on original hardware.
Those questions should remain distinct from the directly observed null call.
There was no same-package physical-device comparison. The diagnostic does not
by itself prove an intrinsic defect in the retail game, or exclude an emulator
startup/scheduling difference that makes this path reachable. In particular,
neither "the phone is slower" nor "physical ticks must be even" has been
established as the explanation.

There is no demonstrated shared-emulator API defect that would make passing
a null `CCoeEnv` to this ROM method valid. Weakening pointer validation or
returning success for that invalid call would not follow the ROM contract.
Likewise, changing frame pacing or timer frequency merely to avoid this branch
has not been validated as a general correction.

After restoring the original kernel for a baseline comparison, the
unmodified Worms card reached a rendered demo match. That baseline also logged
an `E32USER-CBase 45` panic in a `MOSND` audio thread while the main game kept
running. This separate baseline issue was not included in the clock change.

## The WWP compatibility patch

The shared tick API now returns the complete counter by default. A process has
an all-bits-set tick-count mask unless a compatibility hook changes it. At the
Mophun factory entry (`0x10011b74`), a hook sets WWP's process mask to
`0xfffffffe`. All User::TickCount calls made by that process retain the legacy
coarse readings, including calls outside Mophun. Other processes keep full
resolution, and a new WWP process starts with the default mask until identified.
No application UID or library-specific condition is embedded in the shared tick
implementation.

The hook requires process UID `0x101fd3f3` and Mophun code hash `0x086e4d44`
(codeseg XXH32, seed `0x5b001101`). Other games and unrecognized DLL versions
do not receive it. Native iOS and Lua-enabled desktop/Android builds use matching
hooks. The factory is a cold path; no callback runs on individual tick queries.
Original game code, event checks, initialization order and MMC files remain
intact.

This is an explicit WWP compatibility exception, not a demonstrated shared
scheduling correction. Initializing the private cache, bypassing the event poll,
and replacing the clock helper with guest ARM code did not reliably complete
menu transitions. A hot breakpoint that masked only the original helper's R0
result reached the menus but stalled while starting Quick Game. Therefore the
retained scope is the WWP process, reproducing the full legacy clock behavior
for its libraries instead of changing one helper's return value.

These experiments do not establish which scheduling or reentrancy difference
explains the retail game's behavior on hardware. The parity check is a recurring
time window rather than a framework-readiness flag; observed failures included
ticks `0x149` and `0x14f`.

## Tick-only validation

The final Release simulator binary, built on `5d2134f0c` with this working-tree
change, has SHA-256
`20ee59a197b48783ffc7e4e4181aa1295e24f233793bb6b1e520938020b1b5f5`.
The original WWP card reached the main menu, Single Game / Quick Game, and a
rendered battle in Dynarmic and Dyncom. Dynarmic was repeated, with a settled
battle screen checked after the initial spawn animation. Lua syntax and the
factory hook's UID/hash/mask arguments also passed an isolated execution check.
No rewritten guest code or hot clock breakpoint remains.

The default Release simulator regression passed all 12 assertions: Final
Battle (including its 90-second dwell), Calculator on 5320 and N95, and the
string catalog. The resulting guest screens were reviewed. No new access
violation, graphics halt, or regression-suite guest panic was reported. WWP's
`MOSND` thread `E32USER-CBase 45` panic remains present, as on the original
clock baseline; the checked battle continued running.

The earlier four full-counter Ashen movement trials and Highway 3D control
were reused. The default process mask retains every counter bit, and the hook
cannot match Ashen's UID. Movement stopping times were not remeasured on the
final binary.

After the passing suite, switching from N95 back to NEM-4 and restoring the
original WWP ZIP once aborted the host in libarchive's
`archive_read_format_zip_seekable_read_header` / `archive_string_ensure` /
`_realloc`, with an invalid-free report. Cold-launching the same Release binary
and mounting the same ZIP succeeded. The cause and any connection with this
change remain unresolved; this observation is outside the passing suite and
must not be treated as a proven pre-existing issue. The crash report remains
outside the repository.

## Legacy audio Stop follow-up

WWP's MOSND `E32USER-CBase 45` panic was independently reproduced with the
tick-only Release build. This is `EReqTooManyStops`, not the stray-signal panic
46: `CActiveScheduler::Stop()` decremented its active-loop count below zero.
A guest scheduler trace showed the same sound thread first stopping from
Mophun's `MaoscPlayComplete` callback (return `0xe010dda4`, error zero, loop
count 1), then stopping from its original stop `RunL` (return `0xe010cf14`,
loop count already 0).

The N-Gage ROM's original output-stream vtable at `0x505f0618` resolves `Stop`
(slot 40) to Thumb code `0x505efe18`. That function stops the stream and returns
queued buffers through `MaoscBufferCopied(KErrAbort)` without directly invoking
`MaoscPlayComplete`. The separate playback-completion active object, constructed
at stream offset `0x40` with priority 19, is armed through server request 4 in
`0x505efeec`; its `RunL` at `0x505efff0` forwards its completion status through
`MaoscPlayComplete`. Inspecting only the Stop function would miss this
asynchronous notification path.

An experiment dropping the legacy completion avoided WWP's panic but stalled
Ashen at its initial Loading screen. Replacing only that guest DLL with the
original DLL in the same native build restored Ashen's story screen. The
retained repair must therefore preserve the notification while deferring it.

The guest patch now schedules the legacy success completion with an owned
`CAsyncCallBack` at priority 19. It is allocated separately through `CBase` so
its active-object state is zeroed; embedding it in the non-CBase stream
properties object leaves that state uninitialized when the heap is reused.
Destruction deletes the callback, canceling any pending notification.
MMF retains its synchronous `MaoscPlayComplete(KErrCancel)`, matching the
available Symbian MMF source. Buffer-abort notifications remain intact. The
native dispatcher also releases audio-renderer occupancy before returning the
legacy callback-selection result; previously that return bypassed the release.

Both guest DLL variants were rebuilt with their private support library:
S60 2nd FP3 ARMI UREL for `_v81a`, and S60 5th GCCE UREL for `_general`.
The general image passed the E32 header verifier; the EKA1 image retained its
UIDs, flags, CPU, code base, import-library count and 37-export ABI. The final Release
simulator build passed WWP's menu / Quick Game / 20-second battle / exit
sequence in Dynarmic and Dyncom, with zero fresh panics or access violations
in either run. Ashen passed its full opening, chapter load, four alternating
one-second direction holds and exit. The scene was stationary in every
release +0.4-second versus +2-second comparison, and its fresh log contained
no panic or access violation. The default regression passed all 12 assertions,
including Final Battle's 90-second dwell and both Calculator devices; the
resulting guest screens were reviewed.

Audio follow-up Release provenance (built on `ios-next` `9b71b9fb4` plus the
above change):

- Executable SHA-256: `6e1c6e8fca62bab302ad00a65d76846ac9fc6b43be07b2575933fccf24b08489`.
- EKA1 DLL SHA-256: `61157793988695b825ee8b58f1129c10d2f72692b67c642f2e2bcffdae9bfe42`.
- General DLL SHA-256: `5a5287729542b434c156df3838cbff6f5620a03c3d636633e061be92be1b1eb8`.

The earlier one-off ZIP mount crash remains a separate unresolved observation.
