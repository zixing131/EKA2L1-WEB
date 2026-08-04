# Symbian^3 File manager exits itself because the domain manager is missing

## Symptom

On the X7 (rm-707, Symbian^3/Anna), launching the built-in **Files** application
(`filemanager.exe`, UID3 `0x101F84EB`) shows nothing: the emulator view pops straight
back to the app list about a second later. No panic dialog, no access violation, and
with the default log preset the log holds nothing but the usual stubbed-service
warnings. The same app works on the 5320 (S60v3 FP2), so it is not the generic
"file manager cannot start" problem that `Dll::FileName`'s ROM fallback fixed earlier.

## Narrowing it down

`extensive-logging: true` (plus `log-ipc`) in `Documents/data/config.yml` turns the
kernel trace back on and shows how the app dies:

```
Thread GFLM work thread forcefully killed with category: None and exit code: 0
Thread Files forcefully killed with category: None and exit code: -1
```

Exit code −1 (`KErrNotFound`) with `entity_exit_type::kill` is `E32Main` returning an
error, i.e. `EikStart::RunApplication`'s TRAP result — not a panic. Every `Leave
started!` in the log is followed by `Leave trapped by trap handler`, so nothing escapes
uncaught; the interesting question is which leave supplies that −1.

Two temporary probes answered it:

* In `thread_kill` (svc.cpp), dump PC/LR/SP and walk the guest stack, resolving every
  word that falls inside a codeseg's text range via `get_codeseg_from_addr`. The Files
  thread's stack was pure `apparc`/`eikcore`/`filemanager.exe` frames — confirmation
  that the app returned from `E32Main` rather than being killed from outside.
* In `leave_start`, resolve LR the same way and dump the leaving thread's stack. That
  turns each anonymous "Leave started! Guess leave code: -1" into a call chain.

The last two leaves before the exit were:

```
leave -1 from euser.dll (User::LeaveIfError)
  <- filemanager.exe + 0x15e7 / 0x1645      (right after: Create session to unexist server: !DmDomainServer)
  <- eikcore.dll ... apparc.dll             (app UI construction)

leave -1 from euser.dll
  <- filemanagerbkupengine.dll + 0x8619     (right after: Property not found: category = 0x101f75b6, key = 0x10202792)
  <- filemanagerengine.dll <- filemanager.exe <- apparc.dll <- eikcore.dll
```

Both leave with −1, both are trapped, and app execution visibly continues after each
of them (icons load, the GFLM worker thread starts, drives get enumerated), so neither
one *looks* fatal from the log alone. To break the tie, make the two error paths
distinguishable: temporarily return `-1000` instead of `KErrNotFound` when a session is
created to `!DmDomainServer`. The app then exited with `-1000`. That pins the exit code
on the domain manager connection; the backup-property leave is a second, independent
defect on the same startup path (removing it does not stop the app from exiting, and
fixing it alone does not keep the app alive).

Dead end worth skipping: the `Volume size stubbed with 1GB` storm right before the exit
looks like the failure point but is just the GFLM worker thread enumerating drives on
its own thread — adding the thread name to the IPC trace makes that obvious.

## Root causes

1. **No domain manager.** EKA2L1 implements neither `!DmDomainServer` nor
   `!DmManagerServer`. Symbian's domain manager publishes each domain's state as a
   publish-and-subscribe property and lets members subscribe to transitions;
   `RDmDomain::Connect` (domaincli.dll) creates a session, sends `EDmDomainJoin` and
   attaches to the state property. The Symbian^3 File manager connects during app UI
   construction and propagates the `KErrNotFound` all the way out of `E32Main`.

2. **The secure backup and restore state property was never defined.** Clients read
   `KUidSystemCategory` (`0x101f75b6`) / `conn::KUidBackupRestoreKey` (`0x10202792`) to
   learn whether a backup or restore is running. `filemanagerbkupengine.dll` reads it
   while constructing and leaves with `KErrNotFound` when it does not exist.

## Fix

* A minimal member-side `!DmDomainServer` (`services/domain/domain.cpp`): `EDmDomainJoin`
  defines the state property under category `0x1020E406` with the key packing the client
  computes (`(hierarchy << 8) | ((domain << 8) & 0xff0000) | (domain & 0xff)`) and
  publishes `EPwActive`; acknowledge/notification-request/cancel opcodes complete with
  `KErrNone`, and deferral is rejected with `KErrNotReady`. The emulator has no power or
  startup state machine, so a domain never transitions — a notification that never fires
  is the correct behaviour, not a stub. Registered for EKA2 devices only.

* Define the backup/restore property at system-property init time with
  `EBURNormal | ENoBackup`, the steady non-backup state a real device publishes.

Both are shared emulator code, so Qt and Android get them too.

## Verification

X7 Files now opens, lists C/D/E/Z plus "Backup and restore", and drilling into C: shows
the directory listing. 5320's File manager still works, and the standard regression
suite (Final Battle, Calculator, N95 Calculator) plus the Angry Birds touch suite pass.
