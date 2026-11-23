# Installing a device straight from a .7z

Device dumps are shared as .7z archives, not as the loose ROM/RPKG pair the installer wanted. Everyone
who wanted to try a phone had to find a desktop unarchiver first, get the files onto the iPhone, and only
then use the importer — for a "ROM + Z drive dump" pack there was no importable file in there at all.
This is the work that made the archive itself the thing you hand to the importer.

## The two packagings, and why both had to be supported

The sample packs fall into exactly two shapes:

```
N70 (S60v2 - FP3).7z
  N70 (S60v2)/N70.rom
  N70 (S60v2)/SYM.RPKG

6760 slide RM-573 [ROM + Z drive dump].7z
  Extract into the EKA2L1 folder      <- an empty marker file, the whole instruction manual
  data/roms/rm-573/SYM.ROM
  data/drives/z/rm-573/System/...     <- ~6600 files
```

The first is the existing importer's input, just zipped: unpack the two files and hand them to
`install_rom`/`install_rpkg` as before.

The second is a device someone already installed, copied out of their own `data/` folder. Nothing needs
to be dumped out of the ROM for it — the files only have to land in the right place. It is also the shape
that had no path into the app at all, because the emulator's importer only ever took a ROM.

Rather than ask the user which one they have, the layout is worked out from the file list: a
`drives/z/<code>/` component anywhere in a path means the second shape (the `data/` wrapper is a
convention, not a guarantee — some packs nest it another level down), otherwise the largest `.rom` and
the largest `.rpkg` anywhere in the archive are the first.

## Things that only show up once you try it

**Drive Z has to be lowercased on the way in.** `install_rpkg` lowercases every path it extracts, and
everything downstream quietly depends on that: `determine_rpkg_symbian_version` looks for
`system/install/series60v*.sis`, the emulator's VFS resolves guest paths against lowercase names. The
6760 pack stores `data/drives/z/rm-573/System/install/Series60v3.2.sis`. Copied verbatim onto a
case-sensitive filesystem, the device installs and then reports the wrong Symbian version (the probe
falls through to its 9.4 default) — with no error anywhere. The N85 pack happens to be all-lowercase
already, so testing only with that one would have hidden this entirely.

**The firmware code in the folder name is a hint, not the answer.** It is taken from the dump itself
(`resource/versions/product.txt`, else `sw.txt`) exactly the way `install_rom` does, so a device
installed from an archive is indistinguishable from one installed the old way.

**…except for the display name.** A dump's `product.txt` can hold a factory string instead of a name: the
N85 pack's says `Model=N00`, while the `devices.yml` the packer shipped alongside says `N85`. So when the
archive carries a `devices.yml` with an entry for the code we detected, its model/manufacturer/machine-uid
win. The Symbian version deliberately does not — that stays with the probe, which reads the same files
the emulator will.

Drive C contents are ignored even when a pack ships them. C is shared between every installed device, and
writing into it is a side effect nobody asked for by pressing "install this phone".

## Why libarchive, and the two traps it brings

libarchive reads .7z through liblzma, and gets zip/tar/rar for free later. Both it and xz are submodules;
neither one behaves out of the box in this tree.

**Its feature probes lie on iOS.** libarchive decides which platform functions exist with
`CHECK_FUNCTION_EXISTS`, which is only truthful if the probe actually links. `cmake/ios.toolchain.cmake`
builds try_compile projects as static libraries (its `ENABLE_STRICT_TRY_COMPILE` default), so an
unresolved symbol is never noticed and *every* probe answers yes — starting with `_fseeki64`, whose
MSVC-only code path then gets compiled and fails the build. The fix is to set
`CMAKE_TRY_COMPILE_TARGET_TYPE` to `EXECUTABLE` around `add_subdirectory(libarchive)` and restore it
after; the rest of the tree is configured and working against the lenient answers and must not be
re-answered.

The tempting shortcut — leave the lenient mode alone and pre-seed the handful of results it gets wrong —
does not survive contact. `_fseeki64` is the only one that *breaks the build*, so it looks like a
one-line list; grepping libarchive's `CMakeLists.txt` for non-POSIX probes turns up three. Generating
`config.h` both ways and diffing them turns up more, and they are the quiet kind: `HAVE_FUTIMESAT` and
`HAVE_CLOSE_RANGE` (Linux-only calls that Darwin does not have) and `HAVE_LZMA_STREAM_ENCODER_MT` (whose
probe cannot link a not-yet-built target, and which our threadless xz does not provide anyway). Diff the
two configs before believing any hand-written list here.

**It finds the host's liblzma.** The first build linked `/usr/lib/liblzma.5.dylib` — the same trap the
freetype block in `src/external/CMakeLists.txt` warns about, and an iOS binary must not depend on it. The
obvious fix, a `FindLibLZMA.cmake` of our own early in `CMAKE_MODULE_PATH`, does nothing: libarchive
overwrites that variable wholesale on the third line of its `CMakeLists.txt`. What works is pre-seeding
the cache entries the stock module consults, so each of its steps short-circuits — `LIBLZMA_INCLUDE_DIR`
for the `find_path`, `LIBLZMA_LIBRARY` for the `find_library`, and the three `LIBLZMA_HAS_*` results for
the `check_library_exists` calls, which cannot link a target that has not been built yet. Setting
`LIBLZMA_LIBRARY` to a target name rather than a file is what makes CMake wire the dependency at generate
time. `otool -L` on the app binary is the check that this still holds.

One reader-level detail worth keeping: extraction uses `archive_read_data`, not
`archive_read_data_block`. The block form reports the offset each run belongs at and expects the caller to
seek, and `common::wo_std_file_stream::seek` has a long-standing bug where the `seek_where` argument is
ignored (`common::beg ? ...` tests the enumerator, which is 0, so every seek is relative). `archive_read_data`
zero-fills holes and hands back one contiguous stream, so no seeking is needed.
