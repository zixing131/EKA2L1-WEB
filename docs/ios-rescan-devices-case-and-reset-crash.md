# iOS "Rescan devices" — case-sensitivity data loss and a reset() crash

Adding an iOS title-menu "Rescan devices" button (mirroring the Android/Qt
feature) surfaced two latent bugs in the shared `system::rescan_devices()`
path — one silently destructive, one a hard crash — neither of which had ever
been exercised on iOS before because no bridge method called it.

## Bug 1: case-sensitive drive/ROM path probes never match the lowercase install layout

`system::rescan_devices()` (epoc.cpp) walks `drives/<X>/` looking for
already-unpacked device dumps to re-register, using
`drive_to_char16(romdrv)` for the drive letter. That helper always returns an
**uppercase** letter (`static_cast<char16_t>(drv) + 'A'`), so the scan probed
`drives/Z/`. Every installer (iOS `installDeviceWithRomPath:`, Android
`launcher.cpp`, Qt `device_install_dialog.cpp`) writes to **lowercase**
`drives/z/`. On Windows' typically case-insensitive NTFS this silently works;
the iOS Simulator's data volume is case-sensitive, so the probe found nothing.

That alone would just be a no-op, except `rescan_devices()` unconditionally
does `dvcmngr_->clear()` before scanning and `dvcmngr_->save_devices()` after
— regardless of whether anything was found. A failed (case-mismatched) probe
silently wiped `devices.yml` on first tap.

Fixing only the drive letter moved the bug rather than closing it: with the
drive letter lowercased, the scan found the real `drives/z/<code>/`
directories, correctly parsed product info from them, then checked
`roms/<code>/SYM.ROM` for the resident image using `firm_name` **as returned
by `determine_rpkg_product_info`** — which is not lowercased (e.g. `RM-707`)
— against `roms/rm-707/SYM.ROM`, which `install_rom`/`install_rpkg` always
write under `common::lowercase_string(firmcode)`. Second case mismatch, same
shape: `common::exists(rom_file)` came back false, and the function's
"broken device" cleanup path (correctly, for a genuinely-broken dump) then
**deleted** the just-scanned `drives/z/<code>/` directory. First test run on
the dev's own regression-fixture simulator wiped all 5 installed devices'
unpacked ROM trees this way before the second fix landed.

Fixed both call sites in `rescan_devices()` to lowercase before touching the
filesystem, matching the writer side exactly. Both fixes are in the shared
`epoc.cpp`, not iOS-only code — Android and Qt hit the identical mismatch on
any case-sensitive filesystem (Linux ext4, real-device APFS); Windows NTFS
happened to mask it.

## Bug 2: rescan's own auto-boot crashes on a never-booted symsys

Once the case bugs were fixed, `rescan_devices()` correctly found and
re-registered all 5 devices — then crashed with SIGSEGV inside
`kernel_system::install_memory()`, reached via
`rescan_devices() → set_device(0) → reset() → set_symbian_version_use()`.

`rescan_devices()` bakes in an implicit "boot the first found device" side
effect (`conf_->device = 0; set_device(0);`) when it finds anything. On
iOS, `set_device()`/`reset()` is normally only ever called immediately after
`bootDeviceAtIndex:`'s prologue — a fresh `eka2l1::system` built via
`make_system_components` + `startup()` — which sets up everything
`install_memory()` and friends assume is already in place. The iOS bridge's
first cut of `rescanDevices` called `_state->symsys->rescan_devices()`
directly on the app's long-lived `_state->symsys`, which had been through
`startup()` at app launch but never through a device boot (the app was in
the empty "no device installed" state). That symsys was missing whatever
`install_memory()` needs, and the implicit `set_device(0)` crashed
synchronously on the calling thread — not a race with the background
os_thread loop (which is correctly gated on `state->mounted` and never
touches a half-booted symsys), just a direct precondition violation.

Fixed by giving `rescanDevices` the same rebuild prologue as
`bootDeviceAtIndex:` (null `winserv`, clear screen-redraw handles, reset
present fences, construct a fresh `eka2l1::system`, `startup()`) before
calling `rescan_devices()`, so the implicit `set_device(0)` always runs
against a freshly-started system, exactly like a real boot would. The
resulting symsys is still not iOS-mounted (no drive mounts, no driver
bindings) — the frontend must not treat it as booted, and calls
`bootDeviceAtIndex:` afterward (which rebuilds *again* and rereads the
`devices.yml` this call just saved) to actually bring the device up. Two
rebuilds per rescan is wasteful but reuses the already-correct boot path
instead of duplicating its mount/bind/winserv sequence inline.

## Recovery note

Testing the first (case) bug's fix on the dev's own simulator deleted the
unpacked `drives/z/<code>/` trees for all 5 regression-fixture devices
before the second fix was in place. The compact resident `roms/<code>/SYM.ROM`
copies survived (a separate code path), but `rescan_devices()` needs the
*unpacked* tree to re-derive product info, not the resident image. Recovery
used a standalone Python reimplementation of `install_rpkg`'s extraction
loop (rpkg.cpp) run against the original source `.rpkg` files, writing
straight to `drives/z/<code>/` and then letting the (now-fixed) "Rescan
devices" button do the actual re-registration — which doubled as an
end-to-end validation of both fixes.
