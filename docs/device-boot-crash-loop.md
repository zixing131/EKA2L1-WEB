# A damaged ROM crashed the app once, then on every launch after

## Symptom

Importing a device from a ROM/RPKG dump that is damaged — or switching to one
that already is — takes the iOS app down. That much is unavoidable: guest memory
is mapped straight out of the image, so a bad one does not surface as an error,
it kills the process. What made it a real bug is what came next: every
subsequent launch crashed the same way, immediately, before the UI could be used
to delete the device or pick another one. The app was bricked until its data
container was wiped.

## Why the crash became permanent

`bootDeviceAtIndex:` persisted the selection up front:

```objc
_state->conf.device = static_cast<int>(index);
...
_state->conf.serialize();          // ← config.yml now points at the new device

sys->mount(drive_z, drive_media::rom, ...);   // ← the dump is first read here
sys->initialize_user_parties();
```

Everything that can fail on a bad dump happens *after* the write. So a device
that never finished booting was still recorded as the current one, and
`startWithDocumentsPath:` auto-boots `conf.device` on the next launch — straight
back into the same crash. Even a device that was never persisted was not safe:
the auto-boot falls back to index 0 when `conf.device` is unset, and a first
device that crashes on boot *is* index 0.

## The fix: persist only what has proven itself

Two halves, because deferring the write alone cannot help a process that dies
before it gets to write anything.

1. **`conf.serialize()` moved past the boot**, to a new `confirmDeviceBoot`.
2. **A boot-attempt marker** (`Documents/data/boot_attempt.txt`, holding the
   firmware code) written just before the dump is first touched and removed by
   that same confirmation. A marker still on disk at startup means the previous
   run died mid-boot: the named device is kept out of the auto-boot (the first
   device that is not the suspect one is booted instead), the frontend reports
   it once, and the marker is deleted so the device is only ever held against a
   single launch — the user can still boot it by hand, and a reinstalled dump is
   not locked out.

## Where the confirmation has to live, and the dead end on the way

The confirmation should be as late as possible: a dump can be intact enough to
mount and boot but still take the process down while the app list is scanned off
its ROM filesystem. The obvious home was the end of `rescanApps`, which every
boot path calls immediately afterwards — no call site has to remember anything.

That silently did not work. `rescanApps` runs on the main thread and therefore
takes `session_mutex` with `try_lock` only (blocking main while a boot holds
that lock deadlocks against the graphics thread's `dispatch_sync` onto the main
queue); when it loses the lock it returns the previously cached list. Losing it
is not rare — the app grid decodes its icons on background queues, and
`iconPNGDataForUID:sizePx:` takes the same lock *blocking*. So the scan that
follows a device switch routinely hands back a cached list without ever running,
and the marker was never retired: the device booted fine, and the next launch
quarantined it anyway.

The symptom was easy to misread as the marker not being written, because both
the missing `device:` update in `config.yml` and the leftover marker point at
the boot rather than at the confirmation. What separated the two was that the
*first* boot of a launch (the auto-boot inside `startWithDocumentsPath:`) always
confirmed and the *second* (a device switch) never did — the difference between
them being that by the time the second one finishes, there are icons on screen
being decoded.

The confirmation now sits in the Swift `rescanApps` wrapper, after the bridge
call, so it lands whether or not the scan itself got the lock: by then the
device has booted and the frontend is asking for its app list, which is the
proof that was wanted. `confirmDeviceBoot` no-ops while a boot is in flight
(`boot_in_flight`), so a rescan overlapping a device switch cannot sign off on a
boot that has not finished.

One boot deliberately does not take a marker: the reboot
`runLaunchAppWithUID:` performs before a relaunch. It re-runs a device that
already came up in this session, and nothing rescans the app list afterwards to
clear a marker — leaving one there would quarantine a healthy device.

## Also: the importer only accepts the right extension now

Foolproofing on the same screen: the ROM row's file picker is limited to `.rom`
and the RPKG row's to `.rpkg`, and the picked URL's extension is checked again
before it is accepted, since a provider that types everything as generic data
can slip a file past the picker's filter.

The filter cost one wrong turn worth recording. `UTType.types(tag: "rom",
tagClass: .filenameExtension, conformingTo: nil)` looks like it should be enough
on its own: for an extension nothing has registered, the system synthesises a
dynamic type (`dyn.age81e55r`), and the same extension always maps to the same
one. It filters nothing. Handed that type, the document picker greys out every
`.rom` file on the device — a picker matches *declared* identifiers only. The
extension has to be declared in `Info.plist` for the filter to work at all —
both as imported types, since neither format is defined or written by this app
(a raw ROM dump is whatever the phone's flash held, an RPKG comes out of the
dumping tools). Only after that does `N97.rom` become
selectable while the SIS and RPKG files beside it stay greyed. `types(tag:)`
still rides along in the allowed list, so a file typed by some other app's
declaration also comes through; extension lookup is case-insensitive either way,
so `N97.RPKG` picks in the RPKG row.
