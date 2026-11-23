# The N-Gage card import that split the E drive into `system` and `System`

## Symptom

Importing a classic N-Gage game card (System Rush, a folder dump with `System/Apps/6R63`
and `System/Apps/6R63_1`) behaved differently on the two iOS targets:

- **Simulator**: the import bailed out with the banner "N-Gage import failed (error 5)"
  — `ngage_game_card_general_error`. Nothing at all was written to the E drive.
- **Device**: the import "succeeded", but the drive was left with *two* folders,
  `system` and `System`, and the launcher's app list then showed only System Rush.
  Every previously installed game had disappeared from it.

## Narrowing it down

Error 5 is returned from exactly two places in `system_impl::install_ngage_game_card()`,
and only one of them is plausible: `common::copy_folder()` returning false. There was no
log line to say which, so the first change was to add one.

Reproducing the copy outside the app is much faster than rebuilding it. The emulator's
static libraries for the simulator are already on disk, so a ten-line harness linked
against `libcommon.a` and run with `xcrun simctl spawn booted` calls the real
`copy_folder()` on the real card folder. Copying into an empty directory worked. Copying
into a clone of the actual E drive failed, which pinned the fault on *merging* into
existing content rather than on the card dump or on sandbox access.

A `fprintf` at each `return false` inside a private copy of `fileutils.cpp` (linked ahead
of the archive, so the linker prefers the instrumented object) named the failing file:

```
copy_file failed dst=…/e/System/Apps/6R63_1/6R63_1.RSC errno=2 (No such file or directory)
```

The destination directory had never been created. Instrumenting `create_directory()`
explained why:

```
mkdir('…/e/System/')            = -1 errno=17 (File exists)
mkdir('…/e/System/Apps/')       = -1 errno=2  (No such file or directory)
mkdir('…/e/System/Apps/6R63_1/')= -1 errno=2  (No such file or directory)
```

`mkdir` says `System` exists, yet nothing can be created *inside* it. The contradiction is
resolved by the last probe, a five-line C program run twice — once as a macOS binary, once
through the simulator:

```
=== HOST ===   stat('…/e/System') = 0   isdir=1
=== SIM ===    stat('…/e/System') = -1  errno=2
```

**The iOS Simulator resolves paths case-sensitively even though the host volume is
case-insensitive APFS.** It deliberately mimics the device filesystem. `mkdir`'s final
component still lands on the real volume, so it reports `EEXIST` for a name the simulator
will not then look up — a name that exists for `mkdir` and does not exist for everything
else. Note in passing that `common::is_path_case_insensitive()` cannot see this: it asks
`pathconf(_PC_CASE_SENSITIVE)`, which answers for the underlying volume and returns
"insensitive" inside the simulator. Anything that keys behaviour off that probe silently
takes the wrong branch there.

That single fact explains both symptoms. `copy_folder()` reproduces the source tree's
spelling, and a card dump spells the folder `System` while everything else on the E drive
uses `system`:

- On the simulator the copy dies at the first file, because the `System` it tried to
  create was refused as already existing and is then invisible.
- On a device — genuinely case-sensitive — `mkdir` succeeds, and the card lands in a
  brand-new `System` next to the existing `system`. Both are real, and a case-insensitive
  lookup can only ever see one of them. `applist` scans `E:\System\Apps\`, capitalised,
  and `physical_file_system::get_real_physical_path()` tries the literal spelling before
  folding case — so after the import it matched the new `System/Apps`, which holds nothing
  but the game that was just imported. Hence an app list of exactly one entry.

## Fix

`copy_folder()` now keeps the destination as an absolute path that has already been
resolved against what is on disk, using the existing `resolve_case_insensitive_path()`.
A source directory or file whose name differs only in case from something already at the
destination is folded onto it instead of created beside it. When the spelling matches —
every case-insensitive host, and most paths on a case-sensitive one — the resolver's first
`exists()` hits and costs one `stat`.

The N-Gage installer had three more paths that assumed a spelling rather than resolving
one: the `system` folder it creates on the E drive, the duplicate `.aif` registration it
deletes from the non-`_1` app folder (which is what keeps a card game from appearing twice
in the list), and the destination it copies `System/Libs` and `System/Programs` into. All
three now resolve case-insensitively, so they find `6R63/6R63.AIF` under its real name.

A drive an older build already split has to be repaired by hand: the resolver keeps
matching the literal `System`, because there it really does exist. Deleting the stray
`System` folder and importing the card again is enough.

## Verification

- Simulator: importing System Rush now reports "Installed N-Gage game: System Rush", the
  drive keeps a single `system` folder, and the app list goes from 19 to 20 entries with
  the other games intact. (Whether the game itself runs is a separate matter — 6R63_1.app
  loads and then goes no further; that is a pre-existing gap for this title, not something
  the import path controls.)
- Case-sensitive APFS volume, created with `hdiutil create -fs "Case-sensitive APFS"` and
  driven by the same harness: copying the card into a drive that already holds `system`
  and other games creates no second folder, keeps those games, and folds the card's
  `GAMEUTILS.DLL` onto the existing `gameutils.dll` rather than adding a twin.
- `scripts/ios_regression_test.sh` on a Release build: 12/12.
