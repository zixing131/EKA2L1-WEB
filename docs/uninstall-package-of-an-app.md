# Uninstalling an app the package manager never fully knew

## Symptom

Long-pressing Opera Mobile on the iOS app list and picking *Uninstall* answered
"Failed to uninstall Opera Mobile." Nothing was removed: the binaries, the app
registration and the SIS registry entry all stayed. Other apps (Angry Birds,
Final Battle) uninstalled fine.

## Why the frontend could not find the package

`uninstallAppWithUID:` looked the package up by the app's UID3, on the assumption
that a single-app SIS carries the app's UID. Opera Mobile does not:

| | value |
|---|---|
| app UID3 (`operamobile.exe`, `operamobile_reg.rsc`) | `0x2002AA96` |
| SIS package UID (`sisregistry/2002aa97/`) | `0x2002AA97` |

Symbian never required the two to match — nothing in the SIS format ties a
package to the UID3 of an app it installs. So the lookup found nothing, and the
N-Gage fallback (`removeUnpackagedAppWithUID:`) declined as well: it only handles
EKA1-style registrations living in `\system\apps\<app>\`, while Opera's sits in
`\private\10003a3f\import\apps\`.

Finding the package therefore has to start from the app's executable. Two dead
ends on the way there:

- **Matching the app path against the package's file list.** applist does not
  store the path the package installed: `read_mandatory_info` rebuilds it from
  the registration as `<drive>:\system\programs\<name>.exe` (or
  `<drive>:\<name>.exe`), so for Opera it says `E:\system\programs\OperaMobile.exe`
  while the package holds `E:\sys\bin\OperaMobile.exe`. Only the drive and file
  name are dependable, and a file name is not an identity — two packages can ship
  the same one.
- **Matching by secure ID, which is exactly the app UID3.** Right in principle,
  but every registry on disk held `sid = 0`. `fill_controller_registeration` reads
  the SID out of the E32 image, and it runs *before* the files are extracted, so
  `open_file` always failed and the SID was silently left at zero. That code had
  never worked for a real install — only for a stub SIS describing files already
  in ROM.

So the SID had to be made real first: `resolve_missing_executable_sids` fills in
the missing SIDs after extraction, once the files are actually on the drive.
Lookup by SID is now the primary path, with the drive+file-name match kept as a
fallback for registries written before this (they will never have SIDs, and
reinstalling is the only way to get them).

## Why removing the package still left files behind

Three more layers, each of which had to be peeled off before an uninstall was
actually clean:

1. **Files below a conditional were never registered.** Only the controller's own
   install block fed `file_descriptions`; `ss_interpreter::interpret` installed
   the files under `IF`/`ELSEIF` blocks without recording them. Opera's UI
   variant, its plugins and — importantly — `OperaMobile_reg.rsc` are all
   conditional, so uninstall left the registration behind and the app kept a
   (now unlaunchable) icon in the list.
2. **`FILENULL`-style entries with an undefined operation were skipped.**
   `uninstall_package` deleted files whose operation was `install` or `null`,
   but the interpreter installs `undefined` entries too — that is what SIS uses
   for language-dependent files. `OperaMobile.rsc`, `OperaMobileModel.rsc` and
   `locale-ri.rsc` survived every uninstall because of it.
3. **`remove_registeration` never deleted the registry file, and read `pkg`
   after erasing it.** The path was computed into a `vpath` local that was then
   unused, and `pkg` is a reference to the object inside `objects_`, so the
   `pkg.uid` reads after `objects_.erase()` were use-after-free — which is why
   the `sisregistry/<uid>/` folder sometimes survived as well. A surviving
   registry comes back as an installed package on the next `load_registries()`,
   for files that are no longer there.

Nothing here is iOS-specific except the entry point: the Qt and Android package
managers uninstall by package UID (the user picks the package, so they never hit
the lookup problem), but they leak the same files.

## Fix

- `packages::package_owning_executable()` / `packages::package_owning_file()` —
  find a package from the app it installs, by SID first, then drive+file name,
  declining when the name is ambiguous.
- `resolve_missing_executable_sids()` after extraction, so registries record
  which executables (and so which apps) a package owns.
- Register the files inside conditional install blocks.
- Delete `undefined`-operation files on uninstall, like the interpreter installs
  them.
- Delete the registry file in `remove_registeration()`, and copy `uid`/`index`
  off `pkg` before the erase invalidates it.
- iOS: fall back to the executable-owner lookup, and drop a registration the
  package did not own once its binary is gone (for packages installed before the
  conditional-block fix).

Verified on 5320 (rm-409): Opera Mobile uninstalls, its 104 files and the
`sisregistry/2002aa97/` folder are gone, and the app list drops from 19 to 18.

## What else SWI does that we did not

Reading `installationservices/swi/` (SymbianSource `oss.FCL.sf.mw.appinstall`)
for the contract turned up five more gaps, all of them shared-code, none of them
iOS-only:

1. **ROM and non-removable packages were uninstallable.**
   `CPlanner::UninstallPackageL` opens with `IsInRomL()` and `RemovableL()`,
   leaving `KErrNotSupported` on either. We had the `in_rom` / `is_removable`
   fields and checked neither, so a ROM stub package could be "uninstalled" —
   its registration deleted, its files untouchable in ROM, and the stub
   registering it again on the next boot.
2. **Embedded packages were left behind.** Uninstall recurses into
   `EmbeddedApplications()` (`CUninstallationProcessor::DoStateProcessEmbeddedL`).
   We registered embedded packages as their own entries at install time and then
   never removed them with their parent, orphaning both entry and files.
3. **An upgrade never removed the old version's files.**
   `CInstallationPlanner::ProcessFilesToRemoveL` plans every non-ROM, non-`EOpNull`
   file of the installed version for removal before laying the new one down. We
   only replaced the registration — the code said as much in a TODO. Because our
   files are already extracted by the time the tree is registered, the fix deletes
   the difference (what the old version owned and the new one does not) rather
   than the old list wholesale, which would take the just-installed files with it.
4. **Private directories survived.** `CProcessor::RemoveFileL` collects the SIDs
   of executables it removes and `DoStateRemovePrivateDirectoriesL` deletes
   `<drive>:\private\<sid>\` on every writable drive — skipping a SID whose
   executable only eclipsed a ROM one, since that ROM executable still owns the
   directory. We kept the guest data forever.
5. **Targets were taken on trust.** `SecurityCheckUtil::CheckFileName` rejects a
   path that is not drive-qualified, has doubled separators, escapes with `..`, or
   names a non-ASCII executable under `\sys\bin`. We fed SIS targets straight to
   `delete_entry`, so a package could name a path it had no business naming and
   have it deleted on uninstall. SWI fails the whole installation on a bad target;
   we skip the entry instead, so a target we merely parse differently costs one
   file rather than the installation.

Not applicable: `\sys\hash\` cleanup (no hash checking here), running `EOpRun`
files on uninstall (no install-time FR support at all), and integrity-services
transaction rollback.
