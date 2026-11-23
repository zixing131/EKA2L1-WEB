# A camera for the iOS simulator, and the ECam buffer bug it exposed

## Why a synthetic camera

The iOS simulator has no `AVCaptureDevice`. `collection_ios::count()` therefore
returns 0, `CCamera::CamerasAvailable()` reports 0 to the guest, and no camera code
ever runs. That is not merely a missing feature: the Symbian^3 Camera app
dereferences the controller it never built and dies with `KERN-EXEC 3`, so the whole
ECam path was unreachable outside real hardware.

`collection_simulator` (`drivers/camera/backend/ios/camera_simulator.mm`) fills that
gap. It exposes the same back/front pair a device does and feeds a synthesized test
pattern. Two decisions matter:

* **Only the frame source is faked.** The pixel packing, the JPEG encoder and the
  advertised capabilities were lifted out of `camera_ios.mm` into
  `camera_pixel_ios.{h,mm}` and are shared, so a frame reaching the guest went
  through exactly the code a real capture uses. Faking the whole backend would have
  made the simulator prove nothing.
* **It is selected only when the real backend finds nothing**, and the file compiles
  to nothing outside `TARGET_OS_SIMULATOR`. If some future simulator does surface a
  capture device, the real backend still wins, and no device build carries the
  fixture.

The pattern is built to make pixel-path defects obvious rather than to look nice:
colour bars catch a swapped channel order, a corner marker (red on the back camera,
blue on the front) catches a flip, a 32-pixel grid shears if the scanline stride is
wrong, a grey ramp along the bottom shows truncated colour depth, and a bar that
sweeps with the frame counter proves frames are advancing. The front camera mirrors
the bar order, so a guest that picks the wrong index is visible too.

The 5320 (rm-409) Camera now runs on the simulator: viewfinder at 15 FPS, marker in
the top-left corner, grid unsheared, softkeys and zoom slider live.

## The bug it exposed: `E32USER 23` on capture

Pressing the shutter shows "Saving image", then panics with `USER 23`. Per Belle's
`e32panic.h` that is `ETDes8Overflow` — a `TDes8` was given a length beyond its
maximum. It is not a simulator artefact; the code path is identical on a device.

A temporary probe in `thread::kill` dumping PC/LR plus every stack word that resolves
into a codeseg gave the frame that mattered:

```
pc=euser.dll+0x36BC  lr=euser.dll+0xC95F  r2=0x17
stack -> ecam_general.dll+0x657
stack -> ecam_general.dll+0x7AF
...  camcorder.exe frames below
```

So the panic came from inside our own ECam patch DLL, not the app. Dumping that
codeseg and disassembling it identified the two offsets:

* `+0x7A0` is `CCameraImageCaptureObject::RunL`: it reads `iObserverVersion` and
  branches to `HandleCompleteV2` — `+0x7AF` is the return address of that call.
* `+0x654` is the shared `blx r3` that both completion paths jump to. The error path
  reaches it with `r1 = 0`/`r2 = -4`; the *success* path at `+0x76E` branches there
  with `r1 = buffer` and `r2 = 0`. `+0x657` is that call's return address, so the
  capture succeeded and the panic happened inside the app's `ImageBufferReady`,
  while it was reading the buffer we handed it.

Which is exactly what the source predicts:

```cpp
TDesC8 *CCameraImageBufferImpl::DataL(TInt aFrameIndex) {
    ...
    return &iDataBuffer->Des();
}
```

`HBufC8::Des()` returns a `TPtr8` **by value**. Taking its address hands the caller a
pointer into a stack frame that dies with the call, and the app then reads a length
field that is whatever the next call left there. A large enough value makes the
following copy blow past its destination and panic `ETDes8Overflow`.

The same buffer had a second defect. Image bytes are written straight into the heap
descriptor's data area through `DataPtr()` / `HBufC8::Ptr()`, and nothing ever set the
descriptor's length — so `FrameSize()` and `DataL()->Length()` both reported zero. Even
without the dangling pointer, a client that trusted them would have saved an empty
file.

## Fix

In `src/patch/ecam`:

* `CCameraImageBufferImpl` keeps a `TPtr8 iDataDes` member with the lifetime of the
  buffer, and `DataL()` returns its address. It leaves with `KErrNotReady` rather than
  handing back a descriptor over no allocation.
* `SetDataLength()` publishes how many bytes actually arrived. `HBufC8::Des()` yields
  an `EBufCPtr` descriptor, so setting the length through it also updates the heap
  descriptor's own length — one call fixes both `FrameSize()` and `DataL()`. Both
  completion paths (`MCameraObserver` and `MCameraObserver2`) call it after a
  successful receive.
* The no-free-buffer path used to call `observer->ImageBufferReady(*buffer, ...)` with
  `buffer == NULL`, turning an out-of-memory report into a `KERN-EXEC 3`. It now passes
  an empty stand-in buffer.
* `~CCameraImageCaptureObject` had its `delete` guards inverted (`if (!iDataBuffer)
  delete iDataBuffer;`), so it leaked whenever there was something to free.

### Rebuilding the patch DLL

`ecam_general.dll` is a checked-in prebuilt binary, so the fixes above only take effect
once it is rebuilt, and `src/patch/ecam/group/target.inf` asks for
`S60_5th_Edition_SDK_v1.0` for good reason. The other two SDKs cannot compile it at
all: **S60 3rd FP2** has no `CFbsBitmap::BeginDataAccess` / `EndDataAccess`
(Symbian 9.3+), and **Belle** only forward-declares `MReserveObserver` and has no
`TReservedInfo`, `TECamReserveStatus` or `KECamUidEvent2RangeBegin/End` in its public
headers at all — those are S60 5th ECam extensions, not a matter of include paths.

Three things matter when building it, none of them obvious:

* **Use the SDK's own CSL Arm Toolchain (GCCE 3.4.3), not CodeSourcery 4.4.1.** With
  4.4.1 first on `PATH` (where a Belle build leaves it), `priv.lib` fails on
  `VA_START`: this SDK defines `VA_LIST` as Symbian's own `typedef TInt8 *VA_LIST[1]`,
  which 4.4.1's `__builtin_va_start(__va_list&, ...)` will not accept.
* **Build `src/patch/priv` first.** `ecam_general.mmp` links `priv.lib` statically, and
  a fresh SDK has none.
* **Wipe `EPOC32\BUILD\...\ecam` between toolchains.** Objects left by another compiler
  fail linking with "file format not recognized" rather than being rebuilt.

Header fields of the resulting image match the old one exactly (`uid3=0xEE000006`,
`flags=0x1200002B`, `exports=43`); only the size moves.

## Three more defects on the path to a saved photo

With the descriptor fixed, the capture completed but the app still would not finish
saving. Each of the following was found by following what the guest did next:

* **`KPSUidDiskLevel` was never published.** `SysUtil::DiskSpaceBelowCriticalLevelL()`
  looks for the critical free-space thresholds in patchable sysutil data, then in these
  properties, then in central repository, and fails if none answers — so the app's
  pre-write space check spun. Now defined at system-property init with Symbian's own
  `sysutil.iby` defaults (64KB RAM disk, 256KB other).
* **The epoc93 exec table was missing `0xC2` (`property_set_int`)**, sitting between
  `get_bin` and `set_bin`. Same shape of off-by-one as the epoc10 table missing
  `property_delete` at `0xBE`.
* **Absolute paths without a drive letter were never searched by drive.** A path like
  `\sys\bin\LocationUtilityServer.exe` has a root directory, so `try_search_and_parse`
  took the "open it verbatim" branch, where no drive is mounted at `\` and the load
  fails silently. It now walks every drive, like the no-root-directory branch already
  did. (`open_and_get` also tested `io_->exist()` on the captured `lib_path` instead of
  its own argument.)

## Verification

* 5320 Camera on the simulator: viewfinder renders the pattern correctly (channel
  order, orientation, stride, frame advance all good) at 15 FPS.
* Capture no longer panics. The photo lands in `C:\data\images\100_YYYY\` at
  1600x1200 with intact EXIF (`manufacturer=EKA2L1`, `model=EKA2L1 Camera`,
  `orientation=upper-left`, a real timestamp), and the decoded image shows the pattern
  with correct channel order, orientation and stride.
* Standard regression suite 12/12, Angry Birds touch suite 5/5, on a Release
  simulator build.

Probes used during the investigation (access-violation and panic PC/LR/stack dumps in
`kernel.cpp` / `thread.cpp`, a codeseg dumper, a leave-site resolver in `leave_start`,
a spawn-failure trace and a camera-count trace) were removed.

## Still open: the app does not leave the save screen

After the photo is written the app stays on "Saving image" and stops responding to the
Back softkey. Narrowed down with three throwaway probes, all worth repeating:

1. **A host thread dumping every kernel thread every few seconds.** (An ntimer event
   does not survive `kernel_system::reset()`; a detached `std::thread` taking the
   kernel lock does.) Every live thread sits in `wait_fast_sema` at the same
   `User::WaitForAnyRequest` PC — the whole system is idle, so nothing is slow, some
   request simply never completes. `SqlSrv`, `TZSERVER` and `AGSVEXE.EXE` are absent
   from the dump entirely: they start and exit.
2. **Every uncompleted `ipc_msg`.** Comparing viewfinder against stuck shows only one
   addition, `ContentListingFrameworkServer -> !BackupServer fn=32`, which is the same
   long-lived backup notification `!ecomserver` also holds. So no EKA2L1 service is
   sitting on an unanswered message — the usual suspect is not the culprit here.
3. **The guest active scheduler, diffed across dumps.** `active_scheduler::dump()`
   already exists (the panic path uses it); adding the AO's vtable pointer to
   `active_object::dump()` makes the entries identifiable. Exactly one AO becomes
   pending when the app gets stuck, and its vtable resolves — via the ROM file tree,
   since XIP DLLs have no codeseg — to **`Z:\Sys\Bin\avkon.dll`**.

The AO diff turned out to be a red herring: the one AO that becomes pending is
`CAknStatusPaneDataSubscriber` (identified from its typeinfo — Itanium ABI puts the
typeinfo pointer at `vptr[-1]` and the mangled class name at `typeinfo[1]`, and Symbian
ROM DLLs do carry RTTI). A status-pane subscriber waiting on a property is normal, and
the AO that actually blocks was already pending before the shutter.

Two probes got further:

* **Scanning the stuck Camera thread's stack**, resolving each word through the codeseg
  list and then the ROM file tree, gives a consistent chain:
  `camcorder.exe` → `MGXMediaFileApi.dll` → `MediaCollectionManager.dll` →
  `CLFUtils.dll` → **`HarvesterClient.dll`** at the deepest frames. The app is blocked
  adding the new photo to the media library.
* **Logging every thread exit** (raise `thread::kill`'s trace to error level) shows what
  the thread dump could not, because dead threads leave the list:

  ```
  Thread CLF DB work thread0 killed, exit code -3
  Thread !SQL Server killed, exit code -2145668412
  ```

  `-2145668412` is `0x801BB2C4` — a ROM code address, not an error code. That is the
  signature of an unimplemented exec call: EKA2L1 logs it, returns without touching r0,
  and the caller reads whatever was left there. And indeed, `Unimplement system call:
  0xE1!` sits immediately before the SQL server dies.

`0xE1` is `EExecGetModuleNameFromAddress`. Symbian's `exec_enum.h` numbers
`EExecLeaveEnd = 223`, which matches EKA2L1's epoc93 table exactly (`0xDF`), so the
enum's numbering *is* the epoc93 numbering and `GetModuleNameFromAddress = 225 = 0xE1`;
epoc94 and epoc10 sit one and two higher. All three tables were missing it. Unlike
`Dll::FileName`'s exec (`0x14`), this one returns `KErrNone`/`KErrNotFound` and callers
branch on it — `TExtendedLocale::GetLocaleDllName` does, and so does the SQL server on
startup. Implemented on top of the existing `get_dll_full_path()` (which already has the
XIP ROM fallback) and registered in all three tables.

That fixed a real cascade. With the SQL server alive, the metadata server survives
too: it used to die right after with `KErrPathNotFound`, because losing SQL pushed it
onto a fallback path that opens `C:\Private\200009F3\schema.mde`, a directory that
does not exist. Wiping every media database and re-running from scratch now leaves both
servers running and rebuilds `[200009f3]metadata.sq` and `[200009f5]blacklistdb.sq`
correctly.

A note on a fix that was written and then thrown away: creating a process's own private
directory on demand in `new_file_subsession` looked like the obvious answer to that
`KErrPathNotFound`. Once the exec call worked, the branch never executed again — so it
could not be verified, and auto-creating directories quietly changes `RFile::Open`
semantics for every caller. Reverted. The lesson is the ordering: fix the cause, then
re-check whether the symptom's "fix" is still reachable at all.

The app still does not leave the "Saving image" screen.

Tracing every IPC the Camera process sends and every completion it receives — including
HLE-server completions, which go through `ipc_context::complete` rather than the
`Exec::MessageComplete` path and so are invisible to a probe on the latter — gives a
clean picture of the end of the flow:

```
send fn=7  to CLF   (ECLFGetUpdateEvent — arm the "update finished" notification)
send fn=8  to CLF   (ECLFUpdateItems — hand over the new photo)
complete fn=8 code=0
send/complete -1, 1, -2 to !Loader     (connect, LoadProcess, disconnect — all fine)
complete fn=7 code=0                   (the update-finished event arrives)
```

Every request completes, including the loader's. After that last completion the process
goes completely silent: it sends nothing more, re-arms nothing, and the whole system
idles. So it is not waiting on IPC, and it is not waiting on a timer either — a pending
timer would have fired and woken something. It subscribes to only three P&S keys, all
under AknCapServer, so it is not waiting on a property.

Logging every exec call the Camera process makes (not just the IPC ones) shows exactly
what it does last:

```
0x7E session_create / 0x4C session_send_sync   (the loader)
0x69 handle_close / 0xA0 library_detach
0x7A process_logon        <- arm a notification on the process just loaded
0x15 process_resume       <- start it
0x800000 wait_for_any_request   ... and nothing ever again
```

That is Symbian's standard server-startup handshake, and the probe on `process_logon`
confirms the flag: `rendezvous=1`. So the app is not waiting for a process to *exit*, it
is waiting for a freshly started server to announce it is ready via
`RProcess::Rendezvous`.

Logging both sides — who arms a rendezvous and who signals one — pairs every single
startup in the boot:

```
LOGONPROBE by Camera            on HarvesterServer          rendezvous=1
LOGONPROBE by HarvesterServer   on SqlSrv / TZSERVER / AGSVEXE / LocationUtilityServer / MdSServer
RVPROBE    SqlSrv signals ... TZSERVER ... AGSVEXE ... LocationUtilityServer ...
RVPROBE    FLOGSVR signals ... MdSServer signals (waiters=1)
```

Every server signals — `ecomserver`, `cdlserver`, `ContentListingServer`, `baksrvs`,
`SqlSrv`, `TZSERVER`, `AGSVEXE`, `LocationUtilityServer`, `FLOGSVR`, `MdSServer`.
**`HarvesterServer` never does.** Its own dependencies all come up, and then it stops:
its last visible activity is loading `MessageMonitorPlugin`, `FileMonitorPlugin`,
`MMCMonitorPlugin` and `ComposerImagePlugin`, and opening `C:\private\200009F5\mappings.db`
and `restoredone` (both absent). It never finishes construction, never signals
rendezvous, and Camera waits on it forever — which is the whole hang.

So the question is no longer "what is Camera waiting for" but "where does
HarvesterServer stall during startup".

Tracing the harvester's own exec calls the same way puts its last action on a loader
request: `session_create`, `session_send_sync` to `!Loader` with `fn=11`, and then
`wait_for_any_request` forever. Opcode 11 is `ELoadFSPlugin`, which EKA2L1's loader does
not register — and `service::server::process_accepted_msg()` logs an unimplemented call
and *returns without completing the message*, exactly the shape that wedged the guest at
the accessory server's opcode 0. That looks like the answer, and completing the message
with `KErrNotSupported` is almost certainly the right thing for the framework to do
regardless.

It is not committed, because it could not be verified: on every subsequent run —
including one with all media databases wiped — the harvester never sends `fn=11` again
and the unimplemented-call path is never reached, so the app hangs with the fix in place
just as it does without it. Either that request only appears under a state this
environment no longer reproduces, or the hang has a second cause behind it. Handing out
an unverified change to shared IPC dispatch is not worth it; the observation is recorded
here instead. Ruled out along the way: the disk-level property
poll, `LocationUtilityServer.exe`, the SQL and metadata servers (all alive and signalling
now), `CAknStatusPaneDataSubscriber` (the AO-diff red herring), and any uncompleted IPC
on either side.

### The metadata server dies, and takes the harvester's rendezvous with it

Before anything else: **two log classes swallow the probes you are most likely to
write.** The iOS frontend forces the normal-use preset
(`src/emu/config/include/config/config.h`), which contains `Kernel:Warn` and
`Service.Track:error`. So `LOG_INFO(KERNEL, ...)` never reaches the log at all --
silence there is indistinguishable from "the code path never ran", and that is how the
first hour of this round was spent. Worse, `Service.Track:error` hides
`LOG_WARN(SERVICE_TRACK, "Unimplemented IPC call: ...")`, which is exactly the
breadcrumb the previous round grepped for before concluding the `ELoadFSPlugin` lead was
unreproducible and reverting the fix. The path had been running the whole time. Use
`LOG_WARN` or higher for kernel probes, and do not treat a zero grep count on a
`Service.Track` warning as evidence of anything.

With the probes at warn level, the chain resolves completely. Dumping every process's
active-scheduler queue when its thread blocks in `WaitForAnyRequest`, and resolving each
AO's vtable through the Itanium RTTI layout, gives readable state; pairing that with
`process::logon` / `process::rendezvous` / `process::finish_logons` gives the lifecycle:

```
Trying to summon: MdSServer
logon on 'MdSServer'  by 'HarvesterServer'  (rendezvous=true)
Trying to open a non-existent file: C:\Private\10281E17\[200009f3]metadata.sq
finish_logons 'MdSServer' reason=-12 logons=0 rendezvous=1
```

`MdSServer` exits with **-12**, and its one rendezvous waiter -- `HarvesterServer` -- is
completed with that code. So the harvester was never stalling on its own; it was handed
a dead dependency and responded by never completing its own construction. Camera then
waits on the harvester forever.

A wrong turn worth recording: -12 was read as `KErrNoMemory`, which sent the
investigation through the guest heap (`RChunk::Adjust` on `$HEAP` sat at 8-12 KB against
a 1 MB max, so nothing was exhausted), through SQLite (`SqlSrv` returns `KSqlErrGeneral`
on the first open of the missing database, but receives no failing call itself), and
into disassembling the XIP image to find the leave site. **`KErrNoMemory` is -4;
`KErrPathNotFound` is -12.** The stack walk was still useful -- it put the leave at
`MdSServer.exe + 0xF485`, immediately after a `FLogger` write, on the return value of an
imported call -- but the error code alone named the cause.

### Four defects in a row

The `KErrPathNotFound` came from `C:\Private\200009F3\schema.mde`. Confirmed with a
zero-code experiment: create that directory by hand, and MdS falls straight through to
`Z:\Private\200009F3\schema.mde` (the ROM copy), builds the schema, and survives.
On a device the private directory exists, so the guest sees `KErrNotFound` and its
fallback runs; EKA2L1 only materialises the directory once something writes to it, so
the guest saw a different error and the fallback never fired. Fixed by creating the
caller's *own* private directory on demand in `new_file_subsession` -- narrow enough not
to change `RFile::Open` semantics for any other path. (This is the same fix that was
written and thrown away last round for being unreachable; it was reachable, just behind
a symptom that only appears once a capture actually runs.)

With MdS alive and signalling, the harvester got further and stalled again -- this time
on its own last action, `session_send_sync fn=11` to `!Loader`. Opcode 11 is
`ELoadFSPlugin`, which EKA2L1 does not register, and
`service::server::process_accepted_msg()` logged the unimplemented call and *returned
without completing the message*. A synchronous `SendReceive` never returns from that,
which is precisely the shape that wedged the accessory server on opcode 0. Completing
with `KErrNotSupported` let the harvester through to `Fs::MountPlugin` (108) -- and the
file server has its own dispatch with the identical hole, so that had to be fixed too.
`Fs::NotifyDiskSpace` (80) is deliberately left pending there: a real server only
answers it when free space crosses the client's threshold, and completing it with an
error would spin any client that re-arms.

Past that, the emulator segfaulted: `Exec::MessageGetDesLength` dereferences the
translated descriptor pointer without checking it. The slot is typed as a descriptor,
but a client may still pass a null or unmapped address. It and
`MessageGetDesMaxLength` now answer `KErrBadDescriptor`.

### Result

The 5320 Camera returns to the viewfinder after a capture, and the harvester indexes the
photo: on a clean drive both `[200009f3]metadata.sq` and `[200009f5]blacklistdb.sq` are
rebuilt and then updated when the new file is added. Regression suite 12/12 and Angry
Birds touch suite 5/5 on a Release simulator build.

Still open from this file: the Symbian^3 Camera stops at `CCameraAdvancedSettings`,
which the ECam patch does not implement.
