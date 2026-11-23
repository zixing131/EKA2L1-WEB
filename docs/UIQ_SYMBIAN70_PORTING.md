# Bringing up UIQ 2.1 / Symbian OS 7.0 (Sony Ericsson P900)

Working notes for the `epocver::epoc70` device family. The target was the nine Mophun
titles that ship for UIQ 2.x plus the two native games used as controls
(KarmaFighter, Men in Black 2). Everything below was found on the iOS simulator
build; nothing here is device specific.

## What Symbian 7.0 is, for the emulator

Symbian OS 7.0 sits between 6.1 and 7.0s. It is an EKA1 kernel, so it shares the
EKA1 executor tables and the EKA1 HAL entry points, but several servers already
carry a "modern" version number while still speaking the old protocol. That
mismatch is the source of most of the work below.

## Installing the device: a ROM dump is enough (upstream #492)

An EKA1 ROM may hold the whole of drive Z, in which case it installs without an RPKG -
but `should_install_requires_additional_rpkg()` proved that by looking for
`z:\system\versions\sw.txt`, which only Nokia firmware has. Every UIQ ROM failed the
test and the wizard asked for an RPKG that does not exist for these phones.

The file counts say what is really being tested. Burn tree vs. the installed drive Z:
P900 1482/1386 and N-Gage 1107/1018 - the ROM is the whole drive; 6680 619/2945, N70
475/5026 and N95 8GB 2726/8061 - the dump is the core layer and the rest is in ROFS.
What names a device is *variant* data, so it lives in exactly the layer a core-only
dump is missing; that is why one version file stands in for a complete drive Z.

So the test stays the same shape, just wider. `device_naming_files()` lists the files
`determine_rpkg_product_info()` can name a device from and the burn tree is checked
against all of them, which also makes the pre-check answer the right question: will
`install_rom()` get far enough to name the device? UIQ is named after the platform
package its ROM installs (`z:\system\install\sonyericssonp90xplatform.sis`), and
`uiq21platform.sis` pins the version at 7.0.

Verified against the P900 dump from the issue: it needs no RPKG, installs as
`P900 / Sony Ericsson P900 / epoc70`, and its drive Z comes out file-for-file identical
to the device that was already working. Nokia EKA1 ROMs (N-Gage) still install on their
own, 6680/N70 dumps still ask for an RPKG, and EKA2 ROMs are unchanged.

The name itself is the one thing that cannot be read out of a UIQ ROM. Nokia writes
`sw.txt` / `model.txt`; UIQ has no equivalent, and the file that should carry it -
`z:\system\syncml\devinf` - still says `<Mod>P800</Mod>` on a P900 ROM.
`z:\system\data\version.txt` gives a Sony Ericsson CXC number, not a model, and the
only literal "P900" in a system binary is an embedded string in `consvr.exe`. So the
device is named from the machine UID below, which *is* read from the dump, with the
platform package left as the fallback for any other P90x ROM.

### The machine UID is in the dump after all

`install_rom()` used to pass 0 for `HALData::EMachineUid`, because only an RPKG header
carries it - so every ROM-only install (UIQ, and Nokia's EKA1 phones) reported no model
to guests that ask. It is in the dump: the user-side HAL library is built with the
device's attribute defaults as a flat array indexed by attribute number, sitting right
after the `"HAL-UserHal"` name descriptor it is keyed on, and attribute 5 is EMachineUid.
`determine_rpkg_machine_uid()` reads it from `z:\system\libs\hal.dll` (EKA1) or
`z:\sys\bin\hal.dll` (EKA2).

Checked against every ROM to hand, against the RPKG headers and `DEVICE_UID_MAP`:
N-Gage `0x101F8C19`, 6680 `0x10200F99`, N70 `0x10200F9A`, N95 8GB `0x20002D84`,
5320 `0x2000DA5A`, X7 `0x20029A6F` - all match. The P900 answers `0x101FB2AE`, which
MOPHUN.DLL confirms: it reads attribute 5 itself and compares it against exactly two
values, `0x101F408B` (P800) and `0x101FB2AE`.

## Window server: the version number lies

`ws32.dll` in the P900 ROM reports build 151, which is above `WS_NEWARCH_VER`, so
EKA2L1 selected the new client opcode table and every UIQ app exited at startup.
The ROM actually uses the **old** table, and that table is not simply the old one:
it is missing two commands later versions inserted.

Derivation: the UIQ 2.1 SDK's `WS32.LIB` is an `ar` archive of COFF objects whose
`.idata$4` word holds `0x80000000 | ordinal`. Cross-referencing those ordinals with
the ROM's own export table gives a name for each opcode:

| 7.0 opcode | meaning | modern opcode |
| --- | --- | --- |
| 47 | ClaimSystemPointerCursorList | 49 |
| 78 | PrepareForSwitchOff | 80 |
| 80 | LogCommand | 83 |
| 82 | SendEventToOneWindowGroupsPerClient | 85 |

So the fixup is `+2` from `ws_cl_op_start_custom_text_cursor` upward and `+3` from
the old `LogCommand` (80) upward — see `window_server_protocol::session_opcode`. The additional SetFaded gap is
limited to OS 7.0; other old clients retain their previous translation.
A blanket `+2` is what made the Mophun titles hang on `Unimplemented ClOp: 0x53`.

### The window opcode table shifts too

`TWsWindowOpcodes` is derived the same way, cross-checking each `WS32.LIB` ordinal
against the immediate the ROM's exported stub loads into the opcode argument. The
functions sit in opcode order in `ws32.dll`, so reading them in address order gives
the whole table. Three commands the modern table has do not exist in 7.0:

| missing in 7.0 | modern opcode | 7.0 opcodes at or above it shift by |
| --- | --- | --- |
| `EWsWinOpAbsPosition` | 12 | 1 |
| `EWsWinOpSendAdvancedPointerEvent` | 98 | 2 |
| `EWsWinOpEnable/DisableGroupListChangeEvents` | 107, 108 | 4 |

Anchors from the ROM: `Size()` = 12, `Activate()` = 13, `InquireOffset` = 24,
`SetNoBackgroundColor` = 38, `DisableKeyClick` = 88, `SimulatePointerEvent` = 96,
`DisplayMode` = 97, `DisableFocusChangeEvents` = 104, `CaptureLongKey` = 105.
`window_server_protocol::window_opcode` handles these gaps at the wire boundary.
Session, window and DSA handling now obtain their compatibility policy from this
one object. 7.0's 89 and 90 land in the gap the modern enum marks as
"Two messages removed" and stay unimplemented.

DSA follows the same split. With a sync thread, the old protocol reads `GetRegion`'s
completion as a rect count and wants 0; the P900 must take that path even though it
reports build 151 (`legacy_dsa_region()`, the same rule as the legacy opcode table).
Keying it on the build alone hands the P900 `KMaxTInt`, its region `ReAlloc` goes
negative and KarmaFighter dies with `USER 54` right after launch. That regression
shipped in upstream #731 and was fixed in #732; any change on this path needs a
KarmaFighter run.

The session, screen-device and graphics-context tables were checked the same way and
need nothing: the screen device matches `ws_screen_device_opcode` entry for entry, and
the GC matches the `u139` column of `gcop.def`.

`RSoundPlugIn` (CLICKDLL) also needed its remaining opcodes: the default branch
does not complete the message, so an unimplemented opcode deadlocks the client
rather than logging and moving on. That is deliberate elsewhere in the code base —
the deadlock is how missing opcodes get noticed — so the fix is always to implement
them, never to complete-and-ignore.

## Display: 7.0 numbers the video LDD differently

BITGDI panicked with 21 (`EBitgdiPanicInvalidHalValue`) because P900's `hal.dll`
routes the display HAL attributes through the VideoDriver LDD using a control
function table that has nothing to do with the Series 80 one EKA2L1 implemented.
The 7.0 numbering is in `video_driver_control_op_epoc70`. Just as important: the
default branch used to return `KErrNone` without writing the caller's buffer, so
BITGDI read uninitialised memory — unimplemented controls now return
`KErrNotSupported`.

BITGDI 10 (`EBitgdiPanicInvalidRegion`) came from `screen::physical_mode`: a wsini
may list several unrotated modes (P900 has 208x320 and 208x208) and the loop kept
the last one instead of the first.

## The big one: EKA1 panels are directly mapped

All nine Mophun titles booted, loaded `mophun.dll`, ran their VM — and drew
nothing. 0 FPS, black screen.

On an EKA1 phone the LCD *is* the memory the display HAL hands out. Proof from the
ROM: every one of the twelve `CFbsDrawDevice` vtables in P900's `scdv.dll` has
`Update()`, `Update(const TRegion&)` and `UpdateRegion()` as a bare `bx lr`. There
is no flush to hook. EKA2L1 only ever presented the DSA chunk when a guest called
`Update()` (through the scdv patch DLL's `UpdateScreen` dispatch), so a client that
writes straight into the panel was invisible.

The window server owns the refresh timer, and the screen owns framebuffer upload
and composition. HAL only tells the window server that a panel address has been
exposed. The HLE `UpdateScreen` bridge retains the guest's vsync wait and delegates
presentation to the same screen method. Both paths use the observed regions for
a mapped panel; a patched `Update()` must not overwrite untouched window-server
content with the entire buffer. The periodic path never sleeps a guest
thread, and its timer is cancelled before the window server destroys its screens.

`framebuffer_observer` snapshots each mapped panel and compares rows at 60 Hz. A
query alone does not draw anything. Rows changed by the guest are retained as panel
regions, including rows subsequently restored to white; disjoint regions leave the
window server's intervening rows alone. Host composition writeback updates the
snapshot without claiming more rows. A new DSA session and screen-mode changes reset the
observation. This replaces pixel-fill heuristics and row hashes: white is a valid
pixel value, and a hash is not evidence of panel ownership.

The observer samples memory rather than intercepting stores. Writes which return
to the sampled value between refreshes are not observable, and ownership is tracked
at row granularity. These are limitations of the software panel model, not rules
specific to any game.

### Two dead ends worth not repeating

* `[SCDV-HLE] Orientation set to 1370507272` looks like a vtable mismatch. It is
  not: the printed number is the message literal's address, an artifact of how the
  patch DLL's `LogOut` passes varargs. `SetOrientation`'s `cmp r4, #3` guard is
  present in the built DLL and the real argument is in range.
* The scdv patch's vtable layout **is** correct for 7.0. In the old GCC ARM ABI the
  stored vptr points at `vtable_base - 8` (two header words), so bitgdi's
  `ldr ip, [r3, #0x90]` is entry 34 = `Update(const TRegion&)`, which lines up with
  `scdv/draw.h` exactly. Each entry was checked against a semantic fingerprint:
  `SizeInPixels` returns through sret, `SetBits` stores to +0x28,
  `SetFadingParameters` masks two `TUint8`s, `OrientationsAvailable` writes through
  its pointer argument.

### No palette header on the P900 panel

Every Mophun title and KarmaFighter drew 16 pixels to the left, with the cut-off
columns wrapping round to the right edge a row higher. EKA2L1 lays every EKA1 panel
out the Nokia way: 16 palette words, then pixels. N-Gage's ROM `scdv` agrees
(`iBits = iScreenAddress + 0x20`), but P900's stores `iScreenAddress` as `iBits`
unchanged, and its `hal.dll` answers `EDisplayMemoryAddress` with the raw
`iVideoAddress`. Titles that write the panel themselves take those addresses as the
first pixel. On 7.0 the display HAL therefore reports the first pixel as the screen
address, video address and display memory address, with a zero offset to the first
pixel. The `scdv` patch always adds the Nokia palette offset, so 7.0 has no route to
it and uses the ROM's own `scdv`. That driver only builds `EColor64K` screen devices,
which is the P900's `WINDOWMODE`, and `Update()` is a no-op there, so the mapped
framebuffer observer above presents the panel.

## Two general defects found along the way

**`fbscli::resize_bitmap` corrupted the bitmap header.** It updated
`size_pixels`/`size_twips` but not `bitmap_size`, and
`bitwise_bitmap::data_pointer()` uses exactly that field to decide whether the
pixels live in the large chunk or at an offset from the bitmap itself. A bitmap
resized from small to large and then resized again was read at
`this + large_chunk_offset`, several megabytes outside the chunk: a host SIGBUS with
`KERN_PROTECTION_FAILURE`. The fault address always ended in `...2beb0`, which is
the fingerprint of this bug. Boxing 3D and Martial Arts 3D crashed the emulator on
it every run.

**The EKA1 v6 executor table was missing `logon_cancel_thread` (0x31).** Compare
with the v80 table: v6 lacks the three rendezvous entries, so its thread block runs
kill(0x2D) / terminate(0x2E) / panic(0x2F) / logon(0x30) / logon_cancel(0x31) /
get_heap(0x32).

## The on-screen joystick: a rescale that throws the composite away

Every Mophun title ships a `joystick.mbm` and its wrapper `.app` blits it once, at
`(0, 240)` of the 208x320 panel, through `CWindowGc::BitBlt(TPoint, CWsBitmap*)` -
outside any `BeginRedraw`/`EndRedraw` pair, and never again. The window server stores
that blind draw in a non-redraw segment and composites it correctly: reading the screen
bitmap straight after that pass shows the joystick pixels.

They then disappear, because `screen::restore_from_config` assigned
`display_scale_factor = 1.0f` directly instead of going through
`try_change_display_rescale()`. That skips both halves of what a scale change needs -
resizing the screen bitmap, and raising `FLAG_SERVER_REDRAW_PENDING` so every window
replays its store at the new scale. The composite that was made at the old factor is
simply orphaned, and an app that draws once has nothing that will ever put it back.

The DSA present path is what triggers it here (`present_screen_buffer` flips the app's
`screen_upscale_method` to 1 to avoid upscaling a directly drawn frame, which calls
`restore_from_config`), so it hits exactly the titles that draw one static chrome frame
and then take the panel over. With the rescale routed properly the joystick appears and
the Mophun titles become navigable.

The trail that found it, in order: `joystick.mbm` is opened -> the `BitBlt` reaches
`gdi_blt_impl` with a valid 208x80 16bpp `CWsBitmap` -> `redraw_msg_canvas::draw` runs
with `seen=true`, a full 208x320 visible region and both redraw flags set ->
`build_command_draw_bitmap` issues the draw at the scaled destination -> a read-back of
the screen bitmap right after that composite has the pixels -> the next composite
reports scale 1 and a black row.

## What the wrapper actually listens to

Every Synergenix wrapper carries the same UIQ key map, compiled into its
`CCoeAppUi::HandleWsEventL` override as a jump table over scan codes `0xAC..0xB5`.
`fa3d.app` and `HeliAttack2UIQ.app` agree exactly:

| scan code | P900 key | Mophun button bit |
| --- | --- | --- |
| 0xAC | jog dial press | 0x10 (fire 1 / confirm) |
| 0xB1 | jog dial pull (towards) | 0x08 (right) |
| 0xB2 | jog dial push (away) | 0x04 (left) |
| 0xB4 | Browser | 0x100 (fire 2) |
| 0xB5 | Camera | 0x20 (back in menus, pause in play) |

The key names come from the phone's own service menu: `tele.app` has a key test that
switches on the scan code and shows a string from `tele_servicemenus.rsc`. The full
P900 set is 0xA4 Power, 0xA5/0xA6 jog up/down, 0xA7 Back, 0xAC jog press, 0xAD Menu,
0xAE Clear, 0xB1 jog towards, 0xB2 jog away, 0xB3 Ok, 0xB4 Browser, 0xB5 Camera.
`critajogdial.dll` only reports the turns; the flip's bottom row (`critakeypad.dll`)
repeats towards, press and away. `ekdata.dll` translates all of them to `EKeyDeviceN`
or `EKeyApplicationN` codes, never to arrows: away shares `EKeyDevice3` with Back and
press shares `EKeyDevice8` with Ok.

The on-screen joystick the wrapper blits at `(0, 240)` is *directions only*: the hit
test is plain arithmetic (`y > 239`, rows split at `y >= 280`, columns at `x <= 59` /
`x <= 149`) and its six cells yield 5/1/9 over 4/2/8 — up-left, up, up-right over
left, down, right. So a Mophun title on UIQ needs exactly one hardware key to be
playable: **EStdKeyDevice8**. Confirmed by sweeping 0xA4..0xB5 through one emulator
key against Martial Arts 3D's menu; only 0xAC selects.

No frontend could send it: every d-pad centre sends `EStdKeyDevice3` (0xA7), the S60
select key, which UIQ titles ignore. The window server now remaps key input on a UIQ 2
device, whatever frontend it comes from, onto the jog dial: up and down turn it (0xA5,
0xA6), select presses it (0xAC), and left and right pull and push it (0xB1, 0xB2), which
is what the wrapper reads as right and left. In a native UIQ application left is
therefore Back, as pushing the jog away is on the phone. Whether the device is UIQ 2 is
answered on the first key event rather than when the window server is built: the check
looks for `Z:\System\Libs\qikctl.dll`, and the iOS frontend mounts drive Z only after
`set_device()` has created the services. Answered at construction it was always false,
and the remap never ran.

The keypads' green and red phone keys send 0xB4 and 0xB5, which on the P900 are the
Browser and Camera keys. They are off by default on iOS and Android; switch them on in
the layout editor for UIQ titles.

## Audio: every patch map stops at 7.0s

`load_patch_libraries` looks up a section named after the running `epocver`, and
`epocver::epoc70` is new, so **no** shipped `*.dll.map` had a section for it: UIQ
booted with none of the HLE patches. For Mophun that showed up as
`PCM Sound not supported!` — Boxing 3D and Martial Arts 3D refuse to start on it.

The P900's `mediaclientaudiostream.dll` exports exactly one ordinal (the class is
abstract: only `NewL` is `IMPORT_C`, everything else is virtual), and that is the one
ordinal MOPHUN.DLL imports. S60 2nd FP2 puts the same `NewL` at ordinal 1 as well, so
`[epoc70]` is `[epoc81a]`'s first route and the EKA1 `_v81a` patch DLL is picked by
the version walk. Both titles boot with sound now.

The other maps still have no `[epoc70]`. Adding one means checking that ROM's export
ordinals first — a wrong route hijacks the wrong export.

## Vibration: the application server nobody had started

Martial Arts 3D froze a few seconds into every fight, with the vibrator option on.
Nothing was spinning: every guest thread was parked. The game's first hit constructs
`CVibration`, whose `appcli.dll` `Connect` calls `appsvr.exe`'s exported
`StartServer` in the client thread. That launches `appsvr` and waits on a semaphore
named after `LinneaApplicationServer`. `appsvr` in turn starts `Eventsvr` and waits
for it, and `Eventsvr` panics (E32USER-CBase 43) because its `gsmledhandler.ecy`
plugin needs the baseband's `OSE Server`. A stub OSE server only moved the hang one
link down: `settingsvr`'s PM Broker panicked next. The whole chain exists to reach
the baseband.

On hardware the ROM starts Linnea at boot, so an HLE `LinneaApplicationServer` now
exists on 7.0 from the start and `StartServer` returns at once. `appcli`'s protocol
is small: op 2 creates a plugin subsession (argument 0 is the plugin id, the handle
goes back through argument 3), op 3 closes it, ops 4 to 6 are plugin requests, and
op 7 cancels. Only `vibplugin`'s id `0x1A87` is accepted. `vibratorapi.dll` sends op 4
to start and op 5 to stop; both complete at once because they share one active
object. The start request carries three pattern words, `1 0 1` for every Mophun hit,
but no duration, so each start plays a short pulse. Other plugin ids get
`KErrNotSupported`, which fails their clients instead of hanging them.

## Layout: the keypad used to sit on top of the guest picture

A UIQ panel is 208x320. Scaled to an iPhone's width that is ~1855 px tall, which
runs straight under the keypad — including the bottom 80 rows where the guest paints
its own joystick, the only way to steer these games. The render view used to pass the
emulator a single "anchor the picture's top here" pixel offset; it now passes the
whole band the keypad leaves free (`setDisplayBand:height:`) and the picture is
scaled to fit inside it. Controls dragged into the top half are ignored, so a
deliberate overlay still works. 176x208 guests are unaffected — width stays the
binding constraint there.

## SIS v1 conditional blocks

Old-format SIS packages store their file records in the **reverse** of the order the
package declares them. `sis_old.cpp` rebuilt the IF/ELSEIF/ELSE/ENDIF tree while
reading forwards, so every conditional package installed the wrong branch. Records
are now collected flat and replayed backwards; each ELSEIF nests one more block
inside the IF it continues and the single ENDIF closes all of them.

(An earlier diagnosis blamed `var_resolver`'s hardcoded manufacturer UID. That was
wrong — worth stating, because the symptom points that way.)

## Frontend: UIQ is a pen device with a keypad

UIQ phones take a stylus *and* carry hardware keys their titles read, so they are not
"touch screen" in the sense the frontend uses that word — that flag only picks the
keypad-free fullscreen layout by default, which is right for S60 5th and Symbian^3
and wrong here. `device_is_touch_screen` is back to `epocver >= epoc94`, so P900
defaults to the keypad layout, and fullscreen is still one toggle away in the layout
editor. The keypad sends the S60 codes on every device; the window server remaps them
for UIQ 2 (see above).

## Driving the guest from the simulator

Turn the FPS overlay off (`ios.showFPSOverlay`) before hashing screenshots to detect
change: it sits inside the guest picture and its digits tick, so any crop containing
it reports "changed" for every input. Three separate wrong conclusions in this work
came from detectors that were either polluted by that badge or cropped off the thing
they were meant to watch - crop the actual cursor or menu, and check the crop by
eye once before trusting it.

`axe tap` is silently dropped by Xcode 27's simulator far more often than the known
first-tap-after-install quirk suggests - during this work it dropped every tap for a
whole session, which looked exactly like "the guest receives no input". `axe touch
--down --up --delay 0.15` is reliable. When taps start disappearing across a whole
regression run, shut the simulator down and boot it again; the same failures reproduce
on an unmodified build, so they are the environment, not the change under test.

## State of the nine Mophun titles

All nine boot, render, take input and reach gameplay. Verified by hand: Heli Attack 2
into a level, Martial Arts 3D and Boxing 3D into a fight, Carmageddon to track
select, Golf PRO Contest to player select, Joe's Treasure Quest 3D into Options,
Rally Pro Contest to race select, The Da Vinci Code 3D into its opening scene, Fatal
Arena past the language list into the intro. No guest panics in any of them.

Re-checked on 2026-09-23 after upstream #731/#732 were merged into `ios-next`
(Release simulator): all nine plus KarmaFighter reach gameplay and respond to input.
That covers a fight in each of the fighting games, driving in both racers, walking in
DVC and Joe, aiming a shot in Golf and moving in Heli Attack 2.

Two input styles: most navigate with the guest's own on-screen joystick plus the
keypad's OK (EStdKeyDevice8), while Heli Attack 2's menus are a crosshair driven by
tapping the picture directly. Frame rates vary a lot, roughly 4-30 FPS depending on
the title.

## Open items

* Heli Attack 2 stopped answering taps once, after the fullscreen layout was toggled
  on and back off; a relaunch fixed it and no other title reproduced it. Not chased.
* An earlier draft of this document claimed the Mophun VM stops consuming input after
  its first screen. That was wrong, and worth recording: the detector was a screenshot
  hash over a crop that contained the FPS badge, and the "confirm" key had been
  identified as 0xB4 (Browser) by a sweep that only covered the seven scan codes
  the wrapper *captures* — 0xAC is not among them, because the shell never steals it.
* `epoc.cpp`'s `var_resolver` still hardcodes `MANUFACTURER_NOKIA_UID` for every
  device, and `fill_machine_info` hardcodes `machine_unique_id_ = 0`.
* Men in Black 2 writes RGB444 and rotates its picture by 90 degrees for the P800.
  Its device check is dead code, so it still writes RGB444 on the RGB565 P900 panel.
  On the P800 ROM, however, the same colour corruption was an emulator bug:
  `WINDOWMODE COLOR4K` fell through the display-mode parser to `EColor16MA`, and
  legacy DSA then interpreted the pixels as RGB565. Recognising `COLOR4K` selects
  the existing RGB444 path, matching the P800 ROM's `scdv` screen-device factory,
  which accepts only `EColor4K`. The P900 compatibility limitation remains.

## Recipes that paid off

* **ROM XIP disassembly.** ROM base `0x50000000`; a dumped ROM DLL is a
  `TRomImageHeader` (100 bytes) followed by code, and the dump may be truncated —
  read from the ROM image itself rather than the extracted file. Thumb import thunks
  are `ldr r3,[pc,#4]; ldr r3,[r3]; bx r3` with the slot at `(addr & ~3) + 8`; ARM
  thunks put the slot at `thunk + 12`. The slot word lands inside the target DLL's
  export directory, so `ordinal = (slot - expaddr) / 4 + 1`.
* **Live guest stack walking from a server handler**: `kern->get_cpu()->get_reg(13)`
  plus `kern->get_codeseg_list()` and `seg->get_code_run_addr(pr)` gives
  `dll+offset` for every word on the stack. The saved thread context is stale; the
  live SP is the one that matters.
* **Guest PC sampling** belongs in `system_impl::loop()` after `add_ticks`. Be aware
  that a dyncom run slice almost always ends just after an SVC returns, so the
  sample is biased toward the instruction after a syscall — that is not evidence the
  guest is stuck there.
* **A/B-ing a suspected presentation bug**: calling `dispatch::update_screen` from
  inside the `tick_count` SVC every 100 ms put the picture on screen and proved the
  only missing piece was the present. `epockern` does not include the dispatch
  headers; a hand-written namespace declaration links fine.
