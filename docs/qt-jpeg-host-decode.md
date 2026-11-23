# Decoding Qt's JPEGs on the host

## Why

Talking Tom animates by decoding a JPEG sequence, and its black transition
frames turned out to be the guest skipping its scene draw when the next frame
was not decoded in time (see
[AtomShift shows a level only after you touch the screen](./openvg-frame-publish-and-composite-pacing.md)).
Sampling the guest PC every 5ms while the app ran put a number on it:

| | share of all samples | share of executing time |
| --- | --- | --- |
| `<not running>` (waiting / scheduling) | 63.5% | — |
| **qjpeg.dll** | **30.2%** | **~83%** |
| euser / qtcore / qtgui / rest | ~5% | ~14% |

Four fifths of everything the guest actually executed was JPEG decoding.

## Finding the right seam

The first guess — Symbian's Image Conversion Library — was wrong. Dumping the
import table of every code segment as it loaded showed Talking Tom is a **Qt
application** (QtCore, QtGui, QtMultimediaKit, phonon) that never touches
`imageconversion.dll`. Its decoding goes through Qt's own plugin:
`Z:\resource\qt\plugins\imageformats\qjpeg.qtplugin` → `Z:\sys\bin\qjpeg.dll`,
which carries its own libjpeg. ICL is used by non-Qt applications and would be a
separate patch.

So the seam is Qt's image format plugin interface, which is small
(`QImageIOPlugin` + `QImageIOHandler`) and public, unlike ICL's decoder plugin
internals.

Two things had to hold for a replacement built with the Belle SDK (Qt 4.7.4) to
load into this ROM (Qt 4.8.0), and both did:

- `qlibrary.cpp`'s check accepts a plugin whose minor version is **not newer**
  than the host's, with the same major.
- The build key must match exactly. The SDK's `QT_BUILD_KEY` is
  `"Symbian full-config"`, and the ROM's QtCore.dll contains that same string.

## What was built

`src/patch/qjpeg` is a Qt image format plugin whose `read()` hands the encoded
bytes to the host and fills a `QImage` with the result:

- Two new dispatch calls, `eimage_decode_info` (0xB0) and `eimage_decode`
  (0xB1), implemented in `src/emu/dispatch/src/image.cpp` on top of the
  in-tree `stb_image`. The decode writes rows at the destination's own stride
  and in the destination's byte order, so the guest needs no second pass.
- The plugin reaches them through the same `swi 0xC10000` stub the other
  patches use.

The loader substitutes it for `qjpeg.dll`: `qjpeg.dll.map` declares a
`replace` patch, so whenever an application loads the ROM's `qjpeg.dll`,
`lib_manager` loads `qjpeg_general.dll` under that path instead. Nothing is
written to the guest drives, and every frontend gets it. The map requires the
original in ROM with Qt's plugin UID, and `qt-plugin` makes the loader compare
both images' `QT_PLUGIN_VERIFICATION_DATA` the way `QLibraryPrivate::isPlugin()`
does (same major version, minor not newer, same build key and debug flag); a
ROM whose Qt would reject ours keeps its own plugin. The plugin carries the ROM
plugin's UID3 (0x2001E61B) and capabilities (`ALL -TCB`).

## Result

With the plugin active, on the X7 (rm-707) running Talking Tom:

| | before | after |
| --- | --- | --- |
| qjpeg.dll share of samples | 30.5% | **0.3%** |
| guest executing at all | 37.3% | 26.8% |

Frames are identical (the BGRA ordering matches `QImage::Format_RGB32` on a
little-endian guest). Regression suite 12/12, touch/GLES suite 5/5, AtomShift
still enters its level untouched.

The guest still allocates and copies the decoded image twice — once when the
host fills the `QImage`, once when the guest uploads it to OpenVG — so this
removes the decode, not the data movement. Talking Tom's own black transition
frames remain (a few per action, unchanged within noise): its frame supply is
not limited by decoding alone.

## Traps worth remembering

- **Plugin entry ordinals are 1 = `qt_plugin_query_verification_data`,
  2 = `qt_plugin_instance`.** Qt resolves by name first and falls back to these
  ordinals, which is what Symbian actually uses. With them swapped the plugin
  loads, gets rejected silently, and Qt moves on to the ROM one — the symptom is
  simply that nothing changes.
- **Export routing cannot serve a plugin that links Qt.** Routed patch DLLs
  are loaded during boot and their imports are resolved once, at a point where
  `qtcore.dll` is not attached to any process, so every Qt import resolves to
  zero (78 `Invalid ordinal` lines, all from our DLL). A `replace` patch is
  only loaded when the original is, inside the application process.
- `add_symbian_patch` copies `group/` into the build output but never deletes;
  a `.map` removed from the source tree stays in `bin/patch` and in the app
  bundle, and keeps being applied. Clear both when removing one.
- The mmp needs `STDCPP` (Qt pulls in `<string>`), the stlportv5 include path,
  and `EPOCALLOWDLLDATA` (`Q_EXPORT_PLUGIN2` caches the instance in a static).
- `utmctl exec` eats the quotes in `set "VAR=value"`, leaving the variable
  unset; write `set VAR=value&next-command` with no quotes and no space before
  the `&`.

## Scope

This covers Qt applications only. Non-Qt titles decode through ICL
(`imageconversion.dll` + `iclextjpegapi.dll`), which is untouched and would need
its own patch.
