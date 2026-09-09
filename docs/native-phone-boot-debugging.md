# Native phone boot regression

The target is the ROM's real home screen, with working input, on both Nokia
5320 and 5800. A Nokia splash, a window group, or a live process alone does not
prove that the phone UI has booted.

Keep `smoke.html` available through `buildscript/serve_regression.py`.
The existing ROM aliases allow testing with:

- `smoke.html?rom=rom/SYM.ROM&rpkg=rom/SYM.RPKG&phone=1` (5320)
- `smoke.html?rom=rom/SYM5800.ROM&rpkg=rom/SYM5800.RPKG&phone=1` (5800)

Use a fresh browser profile for each clean-boot regression, record the page's
`EKA2L1_BUILD_ID`, and capture both the canvas and the process/thread dump.
`wasm_set_leave_probe(1)` records guest exception backtraces, but significantly
slows startup. `wasm_debug_dump()` identifies pending native IPC and thread
waits. Compare screenshots only with logs from the same build and run.

## Verified initialization failures

- CDL was initialized before Z: was mounted, leaving its reference table empty.
  Refreshing it on the first client connection loads the ROM resources.
- AknCapServer asks CDL for the drive of `101fe2aa.dll`. Matching only
  `z:\101fe2aa.dll` returned `KErrNotFound`; matching the filename stem lets
  Avkon initialize its layout and start EikSrv. The native implementation also
  derives the ECom UID from the filename stem: [CdlEcomReg.cpp](https://github.com/SymbianSource/oss.FCL.sf.mw.uiresources/blob/master/layouts/cdl/CdlServer/src/CdlEcomReg.cpp).
- CDL cancellation must complete the outstanding notification with
  `KErrCancel`. Otherwise the client's active-object teardown waits forever:
  [CdlSession.cpp](https://github.com/SymbianSource/oss.FCL.sf.mw.uiresources/blob/master/layouts/cdl/CdlServer/src/CdlSession.cpp).
- `CRepository::Set` creates settings that do not exist. Returning
  `KErrNotFound` aborts native UI initialization. The nontransactional path now
  creates typed entries using the repository's default metadata. Existing
  values of another type remain unchanged. See the documented contract in
  [centralrepository.cpp](https://github.com/SymbianSource/oss.FCL.sf.os.persistentdata/blob/master/persistentstorage/centralrepository/cenrepcli/centralrepository.cpp).

The CenRep regression cases are in
`src/tests/epoc/services/centralrepo/set.cpp`. Run `ekatests '[centralrepo]'`
from the built tests directory.

Full desktop operation remains unverified. Continue checking native service
ownership and pending requests; do not replace the ROM UI with browser markup
or declare the splash screen a successful desktop boot.
