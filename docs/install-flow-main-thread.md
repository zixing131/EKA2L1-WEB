# What blocks while a device installs, and the rule the Settings screen broke

A review of the device-install flow, prompted by "can it sit there frozen?"
Short answer: it could, in three places, none of them lock contention — plus one
genuine deadlock hazard that lives next door rather than in the install itself.

## The install path does not deadlock

`installDeviceWithRomPath:` takes `session_mutex` and `pause_loop_and_lock` for
the whole dump, which is minutes on a large firmware. That is safe as long as
nothing on the main thread blocks on the same lock, and nothing did: the
main-thread entry points (`rescanApps`, `guestScreenModeSnapshot`,
`guestFrameLimitForAppUID:`) all take it with `try_lock` and fall back to cached
answers. Background callers that block (`iconPNGDataForUID:sizePx:`) just queue
up behind the install, and the import sheet covers the app grid anyway. The
lock order is consistent throughout (`session_mutex` → `device_manager::lock`),
and the install path touches no graphics, so it never waits on the main queue.

`pause_loop_and_lock` also does not stall: with `cpu_load_save` the os thread can
be parked in the scheduler's idle wait while holding `loop_mutex`, but
`common::event` latches (`sync.cpp`), so the `stop_cores_idling()` that precedes
the lock releases a wait that has not happened yet just as well as one that has.

## The three stalls that were real

**The staged copy ran on the main thread.** `stage()` did `FileManager.copyItem`
straight in the `fileImporter` completion — the whole dump, hundreds of MB,
possibly still being downloaded from a cloud provider, with the UI frozen for the
duration and not so much as a spinner (the `installing` flag only goes up later,
when Install is tapped). The staging copy is gone entirely now: the installer
reads the picked file in place (`install_rpkg` extracts it, `install_rom` copies
the ROM onto the device folder itself), so the sheet only holds the picked URL
and opens its security scope around the install call, the way the SIS importer
already did. Picking is instant and nothing is duplicated in the sandbox.

**No progress, no cancel.** Both installers take a `progress_changed_callback`
and a `cancel_requested_callback` — Qt passes both — and the iOS bridge passed
`nullptr, nullptr`. A multi-minute unpack showed one motionless "Processing"
spinner behind `interactiveDismissDisabled`, indistinguishable from a hang. Both
are wired now: a progress bar with a percentage, and a Stop button.

Two wrinkles worth knowing. The `(done, total)` pairs change scale between
phases (`install_rom` reports bytes during the dump, then 1/3, 2/3, 3/3), but the
*ratio* stays monotonic, so the frontend is handed `done / total` and nothing
else; the callback fires per chunk, so the bridge drops moves under half a
percent rather than waking the UI thousands of times. And a cancel surfaces as
whatever error the aborted step happened to return — `install_rom` turns it into
`device_installation_rom_file_corrupt`, which would be a lie on screen — so the
bridge answers from the flag the user set, as a separate `Cancelled` result.
Cancel is honoured inside the installer only: once it has returned success the
device is already in `devices.yml` with its drive Z populated, and the trailing
resident-ROM copy runs regardless rather than leaving a half-installed device.

**Staged files leaked.** Cancelling the sheet after picking a file left the copy
in `Documents/import_tmp` forever; only a finished install cleaned up. Reading
the dump in place retired the folder along with the problem — Settings' data
wipe still removes an `import_tmp` left behind by an older build.

## The hazard next door: main-thread blocking on `session_mutex`

`SettingsView.deleteROMs` and `clearData` called `deleteDevice` /
`resetDevicesState` directly from the main thread, and both block on
`session_mutex`. Deleting a ROM posts `eka2l1DevicesChanged`, which makes the
home surface boot another device on a background queue — and a boot holds
`session_mutex` while the graphics layer attach bounces onto the main queue via
`dispatch_sync` (`context_eagl.mm`, `context_angle.mm`). Two swipe-deletes in a
row is enough: the second blocks the main thread on the lock the first delete's
reboot is holding, and that reboot is waiting for the main queue. Both now run on
a background queue with the section gated while they do.

The rule this leaves, worth stating once: **anything that takes `session_mutex`
blocking must not run on the main thread.** `rescanApps` says so in a comment;
the Settings screen had quietly grown two exceptions.
