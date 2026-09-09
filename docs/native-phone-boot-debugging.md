# Native phone boot regression

The target is the ROM's real home screen, with working input, on both Nokia
5320 and 5800. A Nokia splash, a window group, or a live process alone does not
prove that the phone UI has booted.

Keep `smoke.html` available through `buildscript/serve_regression.py`.
The existing ROM aliases allow testing with:

- `smoke.html?rom=rom/SYM.ROM&rpkg=rom/SYM.RPKG&phone=1` (5320)
- `smoke.html?rom=rom/SYM5800.ROM&rpkg=rom/SYM5800.RPKG&phone=1` (5800)

Use a fresh browser profile for each clean-boot regression, record the page's
`EKA2L1_BUILD_ID`, and capture both the canvas and the process/thread dump.
`wasm_set_leave_probe(1)` records guest exception backtraces, but significantly
slows startup. `wasm_debug_dump()` identifies pending native IPC and thread
waits. Compare screenshots only with logs from the same build and run.

## Verified initialization failures

- CDL was initialized before Z: was mounted, leaving its reference table empty.
  Refreshing it on the first client connection loads the ROM resources.
- AknCapServer asks CDL for the drive of `101fe2aa.dll`. Matching only
  `z:\101fe2aa.dll` returned `KErrNotFound`; matching the filename stem lets
  Avkon initialize its layout and start EikSrv. The native implementation also
  derives the ECom UID from the filename stem: [CdlEcomReg.cpp](https://github.com/SymbianSource/oss.FCL.sf.mw.uiresources/blob/master/layouts/cdl/CdlServer/src/CdlEcomReg.cpp).
- CDL cancellation must complete the outstanding notification with
  `KErrCancel`. Otherwise the client's active-object teardown waits forever:
  [CdlSession.cpp](https://github.com/SymbianSource/oss.FCL.sf.mw.uiresources/blob/master/layouts/cdl/CdlServer/src/CdlSession.cpp).
- `CRepository::Set` creates settings that do not exist. Returning
  `KErrNotFound` aborts native UI initialization. The nontransactional path now
  creates typed entries using the repository's default metadata. Existing
  values of another type remain unchanged. See the documented contract in
  [centralrepository.cpp](https://github.com/SymbianSource/oss.FCL.sf.os.persistentdata/blob/master/persistentstorage/centralrepository/cenrepcli/centralrepository.cpp).

The CenRep regression cases are in
`src/tests/epoc/services/centralrepo/set.cpp`. Run `ekatests '[centralrepo]'`
from the built tests directory.

## Native service ownership and window requests

Build `83244544a70c` on both ROMs reached `LoadBitmapCursorsL` after:

- Leaving `!ViewServer` and `KeySoundServer` to EikSrv. Pre-registering the HLE
  view service makes `CVwsSessionWrapper::StartViewServer` return
  `KErrAlreadyExists`. Native view and key-sound threads now start.
- Publishing C32's `ECoreComponentsStarted` (10) after the HLE socket and serial
  servers are created. `StartC32` waits for this property even with those
  servers present. This does not claim full native CPM configuration (32).

Both ROMs then waited inside ws32 export 34, `SetCustomTextCursor`, on window
client opcode 28. The unsupported command never completed. It now returns
`KErrNotSupported`; ws32 returns that error before submitting sprite members,
and EikSrv intentionally ignores cursor-registration failures. Bitmap text
cursor rendering remains unimplemented.

Build `c5a259d8f420` passed that wait on both ROMs. It displayed the ROM's Nokia
splash on 5800 and a native system-error dialog on 5320. Neither is the home
screen. Two subsequent waits/errors were identified:

- AknCap calls `ClearHotKeys` (ws32 export 8, client opcode 1) during startup.
  It must complete successfully when the HLE's system-hotkey set is empty.
- EikSrv's idle callback starts `!Notifier`. A pre-existing HLE with that name
  causes a `KErrAlreadyExists` leave and the modal system-error dialog.

Reference implementations are in SymbianSource's
[RWS.CPP](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/master/windowing/windowserver/nonnga/CLIENT/RWS.CPP),
[eiksrvui.cpp](https://github.com/SymbianSource/oss.FCL.sf.mw.classicui/blob/master/uifw/EikStd/srvuisrc/eiksrvui.cpp),
and [c32root.cpp](https://github.com/SymbianSource/oss.FCL.sf.os.commsfw/blob/master/commsprocess/commsrootserverconfig/rootsrv/c32root.cpp).

Full desktop operation remains unverified. Continue checking native service
ownership and pending requests; do not replace the ROM UI with browser markup
or declare the splash screen a successful desktop boot.

## Integer startup properties and skin inheritance

The category/key form of `RProperty::Set(TInt)` reached
`property_find_set_int`, which incorrectly called the binary packaging overload
`property::set(value)`. It returned success and notified subscribers while the
integer value remained unchanged. This explains the 5800 Startup property
`100058F4:1` remaining zero despite the ROM writing `EStartupAppStateWait` (1).
The SVC now calls `set_int`, matching the handle-based SVC. The property test
exercises this actual entry point, checks values observed by change callbacks,
and checks missing keys and wrong types.

Skin initialization now imports the base Series60 skin before the selected
theme. The 5800 selected theme does not contain every required Avkon item.
Merged filename IDs receive a separate base for each imported file; replacing
an item preserves hash-chain links and uses the correct v1/v2 record stride.
The regression uses three colliding IDs and a theme override, checking both
item data and retained bitmap filenames. Native test invocation
`ekatests '[property],[skin-merge]'` passes 50 assertions across three cases.

Additional idle telephony subsessions allow 5320 Telephone to pass the observed
PhEng panics: priority client ownership, custom API, conference status, and
new idle calls. This is an offline emulator service; these changes do not
implement modem operation or establish working phone calls.

The 5320 reached the native country/date/time dialogs in build `6d93333b8b81`.
Direction keys changed the selection, China was selected, and accepting the
remaining dialogs made Startup publish UI phase 104 and exit normally. The
remaining image was the theme background, so this still was not a home-screen
success.

5800 Mediator's `ProcessResourceEventsL` explicitly accepts `KErrPathNotFound`
for an absent `z:\private\10207449\events\` directory. File-service DirOpen
returned `KErrNotFound` instead, aborting Mediator and then Telephone. Returning
`KErrPathNotFound` lets both processes remain alive (`8fc450541b56`); Telephone
then waits on `DosServerStartupSem`.

That handshake exposed another ABI error: `User::CommandLine(TDes16&)` creates
an internal `TPtr8`, calls `Exec::ProcessCommandLine`, then halves its returned
length. The SVC had been changed to assign a TDes16 length, halving every native
command line a second time. DosServer requires the complete 16-byte `TSignal`
and otherwise exits with `KErrGeneral` without signalling its startup semaphore.
The SVC now copies raw UTF-16 bytes with a byte length, preserving embedded NULs.
This is confirmed both by the 5800 ROM wrapper at `0x802A25C4` and
`kernel/eka/euser/us_exec.cpp` in SymbianSource's kernelhwsrv repository.

### Native executive ABI and handset feature provider (2026-09-09)

- Confirmed the command-line byte-descriptor fix end to end: both ROMs now keep
  `DosServer` / `SAEThread` alive and pass `DosServerStartupSem`.
- Implemented `RLibrary::Type` (v9.3 executive 0x62; v9.4/v10 0x63). The 5800
  previously panicked `Common phone 41` because its plug-in UID validation read
  uninitialized output from that missing executive.
- Implemented `MessageConstructFromPtr` with the native `RMessageU2` write extent:
  reconstruct handle, function, four arguments, spare1 and session pointer;
  preserve the fields outside that structure and zero disconnect arguments.
- The 5800 then exposed `CONE 14`: `phoneuiutils.dll` reads resource 0x1099B02D
  from `callhandlingui.r01`, but its feature 1715 query had skipped loading that
  file. The native `StaticFeatures.dll` provider contains 439 supported IDs,
  including 1715. `featreg.cfg` alone is not the handset's full feature list.
  Read the immutable provider table through the existing E32 decompressor and a
  bounded recognition of its Thumb lookup function. Unknown layouts retain the
  existing fallback. This eliminates that resource panic in the 5800 run.
- The 5320's synchronous DevSound UID initializer (legacy opcode 1) was left
  pending forever. Complete unsupported legacy requests and route the UID
  initialization overload through the software audio backend's state initializer.
  The overload's end-to-end validation is still in progress.
- Native regression subset currently passes 85 assertions in 7 cases
  (`[featmgr],[ipc-message],[property],[skin-merge],[centralrepo]`).

The goal is not yet met: the 5800 advances into phone-engine audio initialization
but leaves and faults in cleanup, while Home screen / Menu still exit during
startup. Country/date/time dialogs alone are not proof of a working home screen.

### Native theme enumeration and audio routing dependencies

The common Home screen / Standby / Menu `Leave(19)` came from Xn's native theme
query, not an executive opcode mismatch. Its directory mask is `0x1000003F`:
`KEntryAttAllowUid` with directory attributes and a null UID filter. The file
server incorrectly removed directories whenever AllowUid was present, and the
physical VFS rejected directories and short files while trying to read a UID.
Keep directories and short files for a null filter, and apply actual UID
filtering only when at least one requested UID is nonzero. Build `b6db20db7c5e`
reads the native theme tree and keeps the 5320 Standby/Menu processes alive.
The regression checks directory/short-file inclusion and non-null UID matching;
the native subset passes 92 assertions in eight cases including `[dir-uid]`.

The DevSound UID initializer now completes and the phone reaches its next native
dependency, `telephonyaudioroutingserver`. This service is a DLL-hosted thread
normally started by hardware adaptation. Its ordinal 1 creates a scheduler and
server, using `RThread().Name()` as the server name. Bootstrap the ROM thread as
`telephonyaudioroutingserver` with an absolute foreground priority, so it does not
inherit the compatibility SysStart parent's low priority. Both ROMs now connect to this native server and complete its initialization IPCs.

With the theme query unblocked, 5320's mode observer repeatedly reissued ETel
`EMobilePhoneNotifyModeChange` (20086), which returned `KErrNotSupported` on every
call. Hold the notification while the offline radio mode is unchanged, implement
its cancellation (20586), and cancel phone notifications when the subsession is
destroyed. A genuine responsive home screen remains the acceptance condition.


### Home-screen runtime follow-up

The 5320 now renders its native Standby screen and Menu icon grid. Keep this
distinct from full acceptance: launching applications from the home-screen
softkeys is still under investigation.

- Provide the offline USSD subsystem (`S21`), packed-string / MO / MT API
  capabilities, pending receive/release observers, and explicit NotReady for
  sends without a radio. Native Phone Server requires those API capabilities
  while constructing its client services.
- Register Voice1, Voice2, Data and Fax as independent idle ETel lines. Correct
  module enumeration to count entries of the requested type, rather than using
  their absolute position in the mixed phone/line list.
- Key captures now honor signed priorities, modifier masks and capture type,
  remove registrations on cancellation/owner destruction, and deliver each
  raw/translated event to its winning owner once.
- Binary property gets return the stored byte count, including zero for an
  empty value, and preserve bytes beyond that length. Integer gets accept -1.
  Initialize the audio policy client-list property to its empty 88-byte package;
  otherwise the 5800 FRCP plug-in walks an uninitialized list beyond its stack.
- The 5800 exposed a CPU MMU discrepancy: kernel-side reads use the global ROM
  directory, but CPU reads used only the current process directory. Processes
  created after the ROM was mapped therefore fail a cold read at 0x800000BC
  in drtaeabi's exception-index lookup. Route multiple-model CPU pointer and
  page-info lookup through the same global/local selection as the controller.
  A regression creates the ROM mapping before the process and verifies CPU MMU
  resolution plus continued isolation of unmapped local data.

These changes still require final cold-boot and interactive acceptance runs.

### Final cold-boot acceptance (2026-09-09)

Build `0796c772b03f` completes the native 5320 and 5800 shell path in the web
frontend. The 5320 renders Standby and Menu, launches Gallery from the home
screen, and moves the Gallery selection with the directional keypad. The 5800
completes its country/date/time setup, renders Home screen, accepts directional
and menu-key input, and remains responsive to browser control and diagnostic
dumps. Home screen, Telephone, Menu, AppArc, MPXPlaybackServer and
MPXCollectionServer remain alive. A single native `System error` notification
shown during 5800 startup can be dismissed with the left softkey; no access
violation, panic, or memory-allocation failure follows it.

The final 5800 failures were in the multiple memory model and the web build's
address-space limit:

- Shared code chunks were never attached because `attach_chunk` returned before
  recording a foreign chunk. Attach and detach now use one page-table stride
  (1 MiB with 4 KiB pages), so DLLs spanning multiple tables are visible at the
  same addresses in every importing process.
- CPU reads now resolve global regions through the controller. A process created
  after the ROM mapping therefore sees the existing global ROM pages.
- The EKA2 RAM-code allocator now ends at `0x80000000`. Its previous wrapped
  range could allocate code through the ROM region.
- A host reservation failure left `max_size_` set while `page_tabs_` was empty.
  Destroying that partially initialized chunk walked the empty vector and could
  clear page-table 0, which contains the ROM header. Destruction now decommits
  only after host memory and page tables both exist.
- The web module keeps its 256 MiB initial allocation and may now grow to 2 GiB.
  A complete S60 5th Edition boot reserves more than the old 1 GiB ceiling for
  process heaps even though most guest pages remain uncommitted.

The focused native regression command passes 154 assertions in 15 cases:
`[mem-global],[etel],[key-capture],[dir-uid],[featmgr],[ipc-message],[property],`
`[skin-merge],[centralrepo]`. It includes ROM lookup from a late process, shared
two-table code attachment, RAM-code bounds, and safe destruction of a chunk
whose host reservation failed.
