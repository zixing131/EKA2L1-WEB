# Restoring EKA1 services and filesystem semantics for N-Gage games

## Symptoms

Four S60/N-Gage titles exposed failures that initially looked unrelated:

- Bowling Master opened but never reached a playable lane.
- Killer Virus stopped during startup.
- Jungle King stopped before gameplay.
- Cybersaurus was present under `E:\System\Apps` but was missing from the
  application list. Once registered, it could reach its menus, then crashed
  when starting a new game. Its graphics also had a strong cyan/green cast.

The useful pattern was that these were EPOC6/EKA1 applications. They exercise
old kernel executor calls and S60 1st/2nd Edition services that newer EKA2
software no longer uses.

## How the failures were narrowed down

The S60 2nd FP3 SDK was used as the ABI reference. In particular, the POSIX
request block is 68 bytes and its `stat` result is a 60-byte 32-bit structure.
These details matter on a 64-bit host: substituting host `open` flags or
`struct stat` layouts appears to work until the guest reads a field at a
different offset. The camera investigation additionally found two incompatible
revisions of Nokia's client ABI in S60v1/N-Gage and S60v2 FP3.

The application-list failure had two independent filesystem causes. Registry
loading queued all paths for one drive but reconstructed the drive from the
path index, so an entry discovered on E: could be recorded as another drive.
The physical VFS also lowercased the complete mounted tree on case-sensitive
hosts. That made lookup convenient but destroyed the spelling returned by
directory enumeration. Bowling Master enumerates `Settings.dat` and later
compares that spelling; an installed `settings.dat` therefore made its data
set look incomplete even though case-insensitive open succeeded.

Service traces then separated the launch failures:

- Jungle King creates a named global semaphore in both its main and audio
  paths. EKA1 `CreateGlobal` must return `KErrAlreadyExists`; creating two
  independent objects leaves the threads waiting on different semaphores.
- Bowling Master and Cybersaurus use the S60 POSIX server. It had been removed
  from EKA2L1, and the old implementation used host ABI types. Restoring it
  exposed path handling, file-flag, duplicate-descriptor, and lifetime issues.
  Cybersaurus finally reached its new-game path, then sent opcode 22 (`unlink`)
  repeatedly; an unsupported completion is converted by the client library
  into a `POSIXIF -5` panic.
- Killer Virus opens the legacy `CameraServer`. Disassembly of the SDK client
  fixed the opcode and both argument contracts. The N-Gage client creates the
  FBS bitmap and passes its global handle; the FP3 client passes an output
  descriptor and duplicates the server-created bitmap after completion.
- Several titles also reached missing EKA1 process kill/terminate/panic
  executors, a missing thread-flags SVC registration, and the full EPOC6
  loader-info package. These were contract gaps rather than game-specific
  behavior.

The Cybersaurus colour problem was a separate regression. A previous X-Plore
workaround globally changed a ROM-derived `EColor4K` screen into
`EColor64K`. Both modes use two storage bytes per pixel, but their channel
layouts differ: XRGB4444 versus RGB565. Cybersaurus writes the former directly,
so treating it as the latter necessarily distorts the colours. The correct
fix is to preserve the ROM's 12-bit logical display mode.

## Fixes

The physical VFS now resolves path components case-insensitively while keeping
the host entry's original spelling. Installation no longer lowercases N-Gage
trees, directory filters retain their case, and append access observes drive
write protection. Rooted Symbian paths such as `\log.txt` keep the current
drive instead of losing it.

EKA1 application loading now records the drive that was actually scanned.
Named global semaphore creation detects an existing object. The missing EKA1
process executors and SVC registration are present, and the EPOC6 loader-info
layout matches the SDK-sized IPC package.

The restored POSIX service is process-scoped and uses guest constants and
guest-width structures. It supports the file operations exercised here,
including shared open-file descriptions for `dup`, correct create/truncate/
append behavior, defensive guest pointer validation, working-directory
updates when AppRun attaches an `.app`, and `unlink`. Process-exit callbacks
drop lookup state without freeing a server while guest session handles still
refer to it.

The legacy camera service now creates the SDK-specified VGA/EColor16M or
QQVGA/EColor4K bitmap and fills it asynchronously through the existing camera
collection. iOS uses AVFoundation on hardware and the deterministic camera
backend in the simulator; Android uses its existing CameraX backend. Failure,
cancellation, and session teardown release an unclaimed FBS bitmap without
racing a queued host callback.

Finally, the global EColor4K-to-EColor64K remapping was removed. X-Plore's own
12-bit surface table bug is not a valid reason to change the hardware pixel
format for every title.

## Result

All four games reach actual gameplay on the N-Gage ROM. Cybersaurus survives
new-game cleanup, loads its 3D level, and renders with the expected red,
orange, brown, and green palette. Bowling Master reaches the lane, Killer
Virus reaches combat, and Jungle King reaches its playable scene. The fixes
are shared EKA1, VFS, loader, and service behavior; none branches on a game
UID.
