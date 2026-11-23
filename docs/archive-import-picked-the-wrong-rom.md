# Importing an N-Gage QD dump as a `.7z` installed an 8 KB ROM and crashed the app

## Symptom

`N-Gage QD Dev (S60v1).7z` imported through **Install device → archive** without any error, and
then the app died. Other packs (N70, N85, 6760, X7, …) imported and booted fine.

## Narrowing it down

`install_archive` itself was innocent: driving it from a host harness over the same archive returned
`device_installation_none` and produced a device (`RH-29`, 1186 files on drive Z). The give-away was
in the installed tree, not the return code:

```
roms/rh-29/SYM.ROM   8192 bytes
```

The archive's ROM folder holds three files, not one:

```
N-Gage QD Dev (RH-4)/Data/roms/rh-4/BOOT-101FB2B1.dmp    8 KB
N-Gage QD Dev (RH-4)/Data/roms/rh-4/ROOT-101FB2B1.dmp   15.8 MB
N-Gage QD Dev (RH-4)/Data/roms/rh-4/SYM.ROM             19.0 MB
```

These are the raw sections the dumper pulled off the phone, left next to the image assembled from
them. `determine_layout` collected *everything* under `data/roms/<code>/` into `rom_files` and
`install_archive_data_dump` installed `rom_files.front()` — archive order, so the 8 KB boot block.
(The ROM+RPKG branch of the same function does not have this problem: it already picks the largest
`.rom` in the archive.)

The crash that follows is the second half of the story. Booting the device produced:

```
E loader/src/rom.cpp:259 [Loader]: Can't read number of directories in root directory!
T kernel/src/kernel.cpp:1365 [Kernel]: Rom mapped to address: 0x108404000
...
EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x40
  eka2l1::loader::rom::burn_tree_find_dir(...)
  eka2l1::loader::rom::burn_tree_find_entry(...)
  eka2l1::rom_file_system::open_file(...)
  eka2l1::hle::lib_manager::load(...)
  eka2l1::hle::lib_manager::load_patch_libraries(...)
  eka2l1::system_impl::initialize_user_parties()
  -[EKA2L1Emulator bootDeviceAtIndex:trackAttempt:]
```

Nothing in a ROM header identifies the file as a ROM, so `load_rom` happily "parsed" the boot block:
it read `rom_base = 0xE795F001`, `rom_size = 0xE1A01104` out of ARM instructions, seeked to an
offset derived from those, failed to read a root directory count, and still returned a `rom`. The
first thing that then asks the burn tree for a file hits `root.root_dirs[0]` on an empty vector.

## Fix

Two changes, one per layer.

`src/emu/system/src/installation/archive.cpp` — pick the image instead of taking the first entry:
prefer a file named `sym.rom` (the name the emulator writes itself), then any `.rom`, then the
largest file in the folder. The single-`SYM.ROM` packs that already worked are unaffected (verified
against the N85 and N70 archives: same firmware code, same drive Z file counts as before).

`src/emu/loader/src/rom.cpp` — a ROM image that parses into zero root directories is not a ROM;
`load_rom` now returns `std::nullopt` for it, and `burn_tree_find_dir` returns `nullptr` rather than
indexing an empty vector. A bad dump now fails the boot cleanly (the frontend's "device could not be
started" path, which already exists for exactly this case) instead of taking the process down, and
`install_rom` reports `rom_file_corrupt` instead of installing a device from garbage.

## Worth knowing

- The pack's own `devices.yml` sits beside `Data/`, not inside it, so `apply_packaged_device_name`
  never sees it. It would not have helped anyway: it keys on the firmware code, the file says
  `RH-4`, and the dump's own `system/versions/sw.txt` says `RH-29`. The machine UID still comes out
  right, because `device_manager::load_devices` fills a zero UID from `DEVICE_UID_MAP`
  (`rh-29` → `0x101FB2B1`).
- The device is named "G 04.10 Game Developer SW Variant" because that is line 1 of `sw.txt` and the
  dump ships no `model.txt`. Cosmetic, and editable in `devices.yml`.
