# Installing a partial-upgrade SIS deleted the application it was patching

## Symptom

Installing a small patch package over an installed application removed the application.
The case that surfaced it was a 416-byte "Dungeon Hunter 2 HD Fix" SIS: a partial upgrade
(`EInstPartialUpgrade`) carrying the same package UID as the game and exactly one empty
file, `E:\private\2003B2CE\deleteme`.

After installing it:

* every file the base package had installed was gone — the 5 MB executable and all nine
  data packs, about 320 MB in total (only the savegames the game writes at runtime, which
  no package owns, survived);
* `c:\sys\install\sisregistry\2003b2ce\00000000.reg` had shrunk from 60,602 bytes to 299,
  and `00000000_0000.ctl` from 116,360 to 580 — the base entry had been replaced by the
  patch's;
* the game no longer appeared in the app list.

Augmentations (`EInstAugmentation`, the SIS flavour used for DLC) went down the same path,
so they had the same effect on their base package.

## Narrowing it down

`packages::add_package` already has a merge path for partial upgrades, keyed off
`no_new_package = is_installed && install_type == install_type_partial_update`, so the
first question was why it was not taken. A probe printed:

```
PKGPROBE add uid=0x2003B2CE install_type=2 is_installed=false no_new=false objects_for_uid=0
```

`install_type` was read correctly (2 = partial update), but the manager believed nothing
was installed — even though a second probe showed `load_registries()` had loaded the entry
fine at startup, well before the install:

```
line   40: PKGPROBE load_registries loaded 548 objects
line   40: PKGPROBE   uid=0x2003B2CE index=0 type=0 files=588 name='Dungeon Hunter 2'
line  766: PKGPROBE add uid=0x2003B2CE ... objects_for_uid=0
```

Something between those two lines dropped it, and it was the caller:

```cpp
void packages::traverse_tree_and_add_packages(loader::sis_registry_tree &tree) {
    if (package::object *obj = installed(tree.package_info.uid) ? package(tree.package_info.uid) : nullptr) {
        remove_stale_files(*obj, tree.package_info);
        remove_registeration(*obj);
    }
    add_package(tree.package_info, &tree.controller_binary);
    ...
```

This ran for every incoming package regardless of its install type. For a partial upgrade
it is doubly wrong:

* `remove_stale_files` deletes every file of the installed package that the incoming one
  does not also carry. A partial upgrade *by definition* only carries the files it
  replaces, so all 588 of the base package's files counted as stale.
* `remove_registeration` then erased the base entry, so `add_package` saw
  `is_installed == false`, skipped its merge path and wrote the patch's registry entry
  over index 0.

## What Symbian does

`Installer::UninstallPkg` (`secureswitools/swisistools/source/interpretsislib/installer.cpp`)
draws three distinct lines:

```cpp
// Check to see the SA is installed, otherwise RemovePkg() will throw an exception
if (iRegistry.IsInstalled(uid) && (installType == CSISInfo::EInstInstallation))
    iRegistry.RemovePkg(uid, true);          // SA: remove the installed package outright

if (installType == CSISInfo::EInstAugmentation)
    iRegistry.RemoveEntry(uid, aSis.GetPackageName(), aSis.GetVendorName());   // SP: only its own entry

// PU: nothing is removed
```

`CInstallationPlanner::ProcessFilesToRemoveL` says the same thing about the files — the
installed package's files are planned for removal only on an SA upgrade; on a partial
upgrade they are merely recorded as overwriteable:

```cpp
if (aApplication.IsUpgrade())
    aApplication.RemoveFileL(*description);          // SA
else
    iOverwriteableFiles.AppendL(desc);               // PU: "may be legally overwritten"
```

Controllers stack rather than replace. `SisRegistry::GenerateCtlFile` writes an SA's
controller at index 0 and a partial upgrade's at `NextSisControllerIndex()`, and refuses
the install if the base package has no controller to sit next to.

## Root causes and fix

1. **`traverse_tree_and_add_packages` displaced the installed package for every install
   type.** It now only does so for a full installation, and for an augmentation only
   against the augmentation entry carrying the same package and vendor name (a new
   `packages::augmentation(uid, name, vendor)` lookup). A partial upgrade displaces
   nothing, which lets `add_package`'s existing merge path run: the base entry stays and
   the patch's files, properties, dependencies and controller are folded into it.

2. **`controller_info::offset` was never initialised.** The offset is part of the
   controller file's name, and nothing assigned it on a normal install, so what the
   registry entry recorded was whatever had been on the stack. `add_package`'s
   "find a free controller index" scan compared against those garbage values, decided
   index 0 was free, and the partial upgrade's controller landed on top of the base
   package's `_0000.ctl`. The field now defaults to 0, the offset actually written is
   stored back into the entry, and — because entries written by older builds still carry
   the garbage — the free slot is looked for on disk, the way `NextSisControllerIndex()`
   does, rather than trusting the recorded offsets.

## Verification

Base package reinstalled from the original 320 MB SISX (588 files), then the partial
upgrade installed over it:

| | before fix | after fix |
| --- | --- | --- |
| `00000000.reg` | 60,602 → 299 bytes (replaced) | 60,602 → 60,734 bytes (merged, +1 file) |
| `00000000_0000.ctl` | 116,360 → 580 bytes (overwritten) | 116,360 bytes, untouched |
| `00000000_0001.ctl` | absent | 580 bytes, the patch's controller |
| game files | deleted | intact |
| app after install | gone from the list | boots and plays |

`ekatests` covers the two non-SA install types against SIS fixtures built with the SDK's
makesis (`assets/ifblock_pu.pkg`, `assets/ifblock_sp.pkg`, both sharing `ifblock.pkg`'s
package UID): a partial upgrade keeps the base package's files and registration, merges
into its entry and puts its controller in the next free slot; an augmentation gets an entry
of its own without disturbing the base, and reinstalling it replaces only itself. Both fail
on the old code at the "the base package's files are still there" assertion.

The 12-check iOS regression suite passes.
