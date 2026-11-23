# Case-insensitive path recovery re-walked whole ROM directories on every miss

## Symptom

Bowling Master reached its loading screen on a physical iPhone and stayed there, while the
same build on the simulator went straight into the game. There was no panic, no host crash
and no stall: the emulator log kept growing, just very slowly.

## How it was narrowed down

The device log ended part way through ecom's plugin scan, at
`Get entry of: Z:\System\Libs\Plugins\<name>.dll`. The simulator log from the same build and
the same app had logged 475 of those in the window the device managed 136. Nothing was
stuck; the emulator was doing the same work far more slowly.

`drives/z/rm-84/system/libs/plugins` holds 611 entries, all lowercase on disk, and the guest
asks for them with the ROM's own capitalisation (`Z:\System\Libs\Plugins\...`, and names like
`USBPNCLASSCONTROLLER.DLL`). macOS matches those without regard to case, so on the simulator
`physical_file_system::get_real_physical_path()` was answered by its first `exists()` check.
The iOS data container is case-sensitive, so every one of those lookups fell through to
`resolve_case_insensitive_path()`, which enumerated each path component's directory looking
for a case-insensitive match — including a full 611-entry walk for the leaf. Roughly
`n²` directory entries for `n` plugin probes, on top of the slower storage.

This is the same code path that
[ROM stub registration](./ios-rom-stub-wildcard-case-resolution.md) had already been caught
in; that fix removed wasted work per lookup, but each lookup still cost a full enumeration.

## Fix

`find_case_sensitive_file_name()` now keeps a folded index per host directory — lowercased
name to real name — rebuilt only when the directory's modification stamp moves, and dropped
outright whenever this module creates, removes or moves anything (a directory mtime only has
a second of resolution). The index stores names only; entry types are not needed for most
lookups and cost a `stat` per entry, so the one entry a caller actually resolves is typed on
its own.

Measured on a case-sensitive APFS volume, 600 case-mismatched lookups against a 600-entry
directory: 269 ms before, 7 ms after.

## Status

The improvement is measured on a case-sensitive volume through the native test suite; it is
not yet confirmed against the phone, which was offline when the fix was written. If Bowling
Master still stops at its loading screen on hardware, the next thing to check is whether the
installed `E:\System\Apps\bowling` tree kept its original spelling: an older build lowercased
the whole mounted tree on case-sensitive hosts, and Bowling Master compares the spelling that
directory enumeration returns. Renaming `Settings.dat` to `settings.dat` on the simulator
reproduces that variant, and it makes the game exit immediately rather than hang.
