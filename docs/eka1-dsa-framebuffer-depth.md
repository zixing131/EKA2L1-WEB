# EKA1 direct screen access: guests hardcode different framebuffer depths, and no single setting suits them all

## Status

**Unresolved.** Two attempted fixes are recorded here, both of which regressed another title. The
symptom (Sky Force Reloaded's garbled frames on the 6680) is still present. Read this before trying
a third.

## Symptom

Nokia 6680 (rm-36, `epoc80`) → *Sky Force Reloaded*. The game runs at 32 FPS but draws every frame
as fine vertical blue/magenta stripes, about twice as wide as they should be, with the menu running
off the right edge.

## What is actually going on

Sampling one horizontal run of the screenshot shows the value peaks repeat every two guest pixels,
alternating blue-heavy and red-heavy: 32-bit pixels being consumed as pairs of 16-bit ones.

Dumping the direct screen access chunk and rendering it at both depths is the decisive measurement,
and it has to be done for more than one game:

| game | chunk contents | second half of chunk | coherent as |
|---|---|---|---|
| 黄泉道 | — | — | 16bpp (breaks at 32) |
| Sky Force | `f0 07 …` | untouched `0xff` fill | tight RGB565, `w*2` stride |
| Sky Force Reloaded | `f8 f8 f8 00 …` | fully written | 32-bit BGRX, `w*4` stride |

**On one device, in one display mode, different guests write different pixel sizes.** Neither game
asks the emulator what to use: both take the framebuffer address from `UserSvr::ScreenInfo()`, which
carries no depth, and neither queries the video-info HAL. The four `CWsScreenDevice::DisplayMode()`
calls that show up in the log come from AVKON during startup and are identical across both games.

Sky Force is the odd one out: it follows the window server's display mode, so it writes 16-bit
pixels when the emulator reports `EColor64K` and 32-bit pixels when it reports `EColor16MU`. That
makes it useless as a control — it passes under either policy and hides the disagreement.

## Attempt 1 — force the DSA transfer texture to 32-bit (regressed Sky Force)

`update_screen()` in `dispatch/screen.cpp` builds its transfer texture from `scr->disp_mode`. Since
`create_screen_buffer_for_dsa()` allocates the chunk as `w * h * 4` and every pitch in
`update_screen()` already assumes four bytes per pixel, hardcoding the texture to 32-bit looks
principled.

It fixes Reloaded. It also regresses the original Sky Force into a double-width, confetti-coloured
mess, because that game was writing 16-bit pixels under the then-current `EColor64K` report.

## Attempt 2 — honour the device's declared window mode (regressed 黄泉道)

`z:\rm-36\system\data\wsini.ini` says `WINDOWMODE COLOR16MU` — 32-bit — while `window.cpp` clamped
any EKA1 mode deeper than 16 bits down to `EColor64K`. Removing the clamp makes `scr->disp_mode`
agree with the ROM, and both Sky Force titles then render correctly (Reloaded because the texture
becomes 32-bit, Sky Force because it follows the report and switches to 32-bit too).

It regresses **黄泉道**, which hardcodes 16-bit writes into the DSA framebuffer regardless of what
the window server reports: its screen collapses to black with tiny red/cyan-fringed sprite fragments
and a stretched ground band. Reverted in favour of the pre-existing behaviour.

Note `WINDOWMODE` describes the mode WSERV composes in. It is not evidence about the format of the
physical framebuffer that `UserSvr::ScreenInfo()` hands to DSA clients, which is what these games
write into — that conflation is what made attempt 2 look justified.

## Where a real fix would have to come from

The depth is chosen by the guest when it builds its draw device, via scdv's
`CFbsDrawDevice::NewScreenDeviceL(TScreenInfoV01, TDisplayMode)` — the one place the intended format
is stated. EKA2L1 does not implement or intercept that export (`scdv` is the real ROM DLL here; the
`LIB(scdv)` entries in `bridge/epoc6.def` are part of an export-hash table whose `#include`s in
`libmanager.cpp` are commented out). Recording the `TDisplayMode` from that call per screen, and
using it for the DSA upload instead of `scr->disp_mode`, is the shape of a correct fix.

## Verification notes

- The automated suites cannot catch any of this: the standard suite covers Final Battle, Calculator
  and the N95 Calculator, and angrybirds covers the X7 — **all EKA2 devices**. Both attempts passed
  12/12 and 5/5 while regressing a 6680 title. Anything touching the EKA1 screen path has to be
  driven per-app on the 6680 by hand.
- 6680 apps worth checking: 黄泉道, Tetris3D, skyforce, Ashen, Asphalt 2, skyforcereloaded,
  dragonworld, X-plore. `-LaunchAppUID` does not work for all of them (Sky Force's `0x10105B92` is
  silently ignored); drive them with `xcodebuildmcp ui-automation snapshot-ui` then
  `tap --elementRef`, taking the snapshot and the tap in the *same* invocation because refs go stale.
- Tetris3D's garbled strip below the help text is pre-existing; it reproduces identically on an
  unmodified build.
- A probe that fails to compile leaves the previous binary installed, and "the log shows nothing"
  then looks exactly like a real answer. Two conclusions here — "`update_screen` is never called"
  and "the game creates no bitmaps" — were both artifacts of a build error in the probe itself.
