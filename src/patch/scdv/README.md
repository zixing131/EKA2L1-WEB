This part of the project is the replacement for the screen driver comes with every phone (scdv.dll).

This project is neccessary to replace any references to communicate with the hardware through logical device driver,
and replace them with calls to emulator.

Because the implementation on Symbian OSS is licensed under EPL, this project reimplements all stuffs, hoping
to be faster.

Newer S60/Belle BitGDI clients request the premultiplied-alpha
`EColor16MAP` mode and the screen `MSurfaceId` interface. The C++ source now
implements both without depending on Symbian partner-only headers. The
checked-in general DLL is a full GCCE build from the Nokia Symbian Belle SDK;
its frozen DEF preserves the 31-entry export ABI used by ROM patch maps.
`surface_stub.S` and `scripts/build_scdv_belle_patch.sh` retain the earlier
ABI-preserving binary-patch path for historical and diagnostic use. Complete
build notes and validation are in `docs/ios-asphalt6-x7.md`.
