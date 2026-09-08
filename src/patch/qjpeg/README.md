# qjpeg

A replacement for Qt's JPEG image format plugin that decodes on the host instead
of running libjpeg under emulation. Qt applications reach it through the normal
`QImageReader` plugin lookup.

It is installed onto the guest's C drive (`C:\sys\bin\qjpeg.dll` plus the
`imageformats` stub), not over the ROM copy, so Qt falls back to the ROM plugin
on any ROM that refuses ours. There is deliberately no `.map` file: the patch-map
mechanism resolves a patch DLL's imports during boot, when `qtcore.dll` is not
attached to a process, and every Qt import would resolve to zero.

See `docs/qt-jpeg-host-decode.md`.

## Building

Belle SDK (Qt 4.7.4), which produces a plugin the Qt 4.8.0 ROM accepts - the
build key is `Symbian full-config` in both, and Qt accepts a plugin whose minor
version is not newer than its own.

```
moc.exe -o src\qjpeghandler.moc src\qjpeghandler.cpp
cd group\general
sbs -b bld.inf -c armv5_urel_gcce4_4_1
```

`moc` output is generated next to the source and included by it, so it does not
appear in the mmp. The built DLL is committed as `group/qjpeg_general.dll`.
