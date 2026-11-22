# iOS Port — Investigation Log Index

Each entry is a deep-dive into a non-trivial bug or design question hit during the
iOS port: symptom, how it was narrowed down, and the conclusion/fix. Newest first.

| Date | Doc |
|---|---|
| 2026-11-22 | [ekalynx ARMv5/v6 interpreter on iOS](./ekalynx-interpreter.md) |
| 2026-11-21 | [Thumb block interpretation on iOS](./thumb-block-interpreter.md) |
| 2026-11-17 | [Guest and host filesystem paths](./PATH_TYPES.md) |
| 2026-11-13 | [UIQ 3 bring-up contract review](./uiq3-bringup-review.md) |
| 2026-10-30 | [Snakes Subsonic: RGB444 data on the N97 PostingSurface path](./ngage-snakes-rgb444-posting.md) |
| 2026-10-27 | [Ashen movement timing and the Worms EKA1 tick mask](./ashen-movement-and-eka1-tick-mask.md) |
| 2026-10-26 | [EKA1 framebuffer refresh and the OS 7 ROMs](./eka1-framebuffer-refresh-contracts.md) |
| 2026-10-24 | [Warhammer: card protection and the startup crash](./warhammer-card-protection.md) |
| 2026-10-21 | [Rayman 9210 on the Nokia 9500 ROM](./rayman-s80-rom-compatibility.md) |
| 2026-10-21 | [Let’s Fly: IAP startup dependencies and cross-process communication](./lets-fly-iap-startup.md) |
| 2026-10-20 | [Smash The Bugs: unavailable X7 haptics becomes USER 0](./x7-smash-the-bugs-haptics-panic.md) |
| 2026-10-02 | [Nokia 7710: Series 90 ROM and window protocol contracts](./nokia-7710-series90-rom-contracts.md) |
| 2026-09-19 | [The N-Gage Radio icon, and why mask polarity is the display mode after all](./icon-mask-polarity-is-the-display-mode.md) |
| 2026-09-13 | [Restoring Tomb Raider's original N-Gage Arena](./tomb-raider-arena-networking.md) |
| 2026-09-12 | [Tomb Raider on the N70: ROM lookup and per-mode display depth](./ngage-title-on-s60v2-rom-lookup-and-display-mode.md) |
| 2026-09-11 | [Tomb Raider's invisible launcher icon: N-Gage ROM evidence](./tomb-raider-aif-icon-selection.md) |
| 2026-09-08 | [Super Miners finds no Bluetooth players](./super-miners-sdp-server-never-answers.md) |
| 2026-09-08 | [Rebuilding discovery without blocking the host lifecycle](./netplay-suspension-lifetime.md) |
| 2026-09-08 | [The Bluetooth queries socket that filled the disk](./bluetooth-defunct-socket-log-flood.md) |
| 2026-09-08 | [GLES1 rotation axes: Symbian ROM evidence](./gles1-rotation-axis-contract.md) |
| 2026-09-08 | [Block Breaker 2 Deluxe has no paddle](./block-breaker-2-invisible-paddle.md) |
| 2026-09-07 | [Killer Virus counts time in frames, not in seconds](./killer-virus-frame-paced-clock.md) |
| 2026-09-07 | [Final Battle's intro subtitles fly past unreadably](./final-battle-intro-timer-tick-rounding.md) |
| 2026-09-07 | [Dyncom dispatch and the next iOS performance work](./dyncom-dispatch-and-ios-performance.md) |
| 2026-09-06 | [Local Network discovery through Apple Bonjour](./apple-bonjour-local-netplay.md) |
| 2026-09-06 | [Dragon World slows down while waiting for its multiplayer peer](./dragon-world-multiplayer-frame-pacing.md) |
| 2026-09-06 | [Bluetooth discovery sends expired control packets](./bluetooth-control-packet-buffer-lifetime.md) |
| 2026-09-06 | [N-Gage Dragon World immediately leaves its Client role](./ngage-bluetooth-device-selection-response-layout.md) |
| 2026-09-06 | [Controller touch, motion and haptics on an external iOS display](./ios-controller-external-display-input.md) |
| 2026-09-06 | [Independent controller stick bindings on iOS](./ios-independent-controller-sticks.md) |
| 2026-09-06 | [AirPlay game output with controls on the phone](./ios-airplay-game-display.md) |
| 2026-09-06 | [Qt mouse clicks were delivered as right-button events](./qt-mouse-button-index.md) |
| 2026-09-06 | [GLES2 shader compilation on macOS Core Profile](./macos-gles2-shader-version.md) |
| 2026-09-06 | [Inactive GLES2 attributes overwrote the wheels' vertex positions](./gles2-inactive-attributes-overwrite-position.md) |
| 2026-09-06 | [Icy Tower requires the Rotation sensor channel](./ios-icy-tower-rotation-channel.md) |
| 2026-09-06 | [GDI command payloads need their own alignment](./gdi-command-payload-alignment.md) |
| 2026-09-02 | [One framebuffer, two row pitches](./x7-framebuffer-pitch-is-per-client.md) |
| 2026-09-02 | [Alien Pinball hangs on a black screen on the X7](./alien-pinball-x7-uncompleted-ipc.md) |
| 2026-08-31 | [GetImage is a viewfinder poll, not a photo — and the frames arrived sideways](./camera-getimage-viewfinder-poll.md) |
| 2026-08-31 | [Four N-Gage titles on the N70: a missing EKA1 exec block and a framebuffer that was never 16-bit](./n70-eka1-exec-and-framebuffer-depth.md) |
| 2026-08-31 | [Case-insensitive path recovery re-walked whole ROM directories on every miss](./case-insensitive-lookup-directory-index.md) |
| 2026-08-31 | [Connecting the S60v2 CameraServer to the camera backend](./s60v2-camera-server-backend.md) |
| 2026-08-30 | [Restoring EKA1 services and filesystem semantics for N-Gage games](./ngage-eka1-game-compatibility.md) |
| 2026-08-30 | [Puyo Pop went silent a minute in and never made a sound again](./puyo-pop-audio-stream-dies.md) |
| 2026-08-30 | [Quitting a game left the Qt frontend on a black screen, and closing it hung the process](./qt-app-exit-never-returns-to-list.md) |
| 2026-08-29 | [Every character a linked font's fallback supplied drew as .notdef](./linked-font-atlas-canonical-only.md) |
| 2026-08-29 | [Importing an N-Gage QD dump as a `.7z` installed an 8 KB ROM and crashed the app](./archive-import-picked-the-wrong-rom.md) |
| 2026-08-28 | [ROM stub registration spent minutes resolving wildcard paths on iOS](./ios-rom-stub-wildcard-case-resolution.md) |
| 2026-08-26 | [RPKG extraction repeatedly statted the same directory hierarchy](./rpkg-repeated-directory-stat.md) |
| 2026-08-25 | [Decoding Qt's JPEGs on the host](./qt-jpeg-host-decode.md) |
| 2026-08-25 | [AtomShift shows a level only after you touch the screen](./openvg-frame-publish-and-composite-pacing.md) |
| 2026-08-23 | [OpenVG round caps and joins rendered as square or bevelled strokes](./openvg-round-stroke-caps-and-joins.md) |
| 2026-08-23 | [Stereo player position advanced at twice the playback rate](./audio-player-stereo-position-double-speed.md) |
| 2026-08-23 | [Qt OpenVG ellipses collapsed to fragments](./openvg-path-scale-bias.md) |
| 2026-08-23 | [Every GLES1 draw failed on macOS because `inverse()` is not in GLSL 1.40](./macos-gles1-inverse-glsl140.md) |
| 2026-08-23 | [`--app` deadlocked before the guest ran a single instruction](./qt-graphics-event-lost-wakeup.md) |
| 2026-08-22 | [A merge dropped dyncom's breakpoint resume fix, and the Avkon patch died with it](./dyncom-thumb-rom-breakpoint-resume.md) |
| 2026-08-16 | [A stability net for upstreaming this fork](./upstream-stability-plan.md) |
| 2026-08-11 | [Dragon World stalls at the difficulty screen in netplay](./dragon-world-recv-one-or-more-layout.md) |
| 2026-08-11 | [Verifying Bluetooth netplay with two simulators on one host](./netplay-two-simulator-verification.md) |
| 2026-08-11 | [Central netplay collapsed same-source peers onto one UDP endpoint](./bluetooth-central-same-address-ports.md) |
| 2026-08-10 | [Installing a device straight from a .7z](./device-install-from-7z-archives.md) |
| 2026-08-10 | [Proxy-server Bluetooth netplay never worked, and how to run your own matching server](./bluetooth-netplay-central-server.md) |
| 2026-08-10 | [X7 Bounce Boing Battle: common Bluetooth options and reform NetDB opcodes](./x7-bounce-boing-battle-bluetooth-netplay.md) |
| 2026-08-10 | [N70 Bluetooth netplay: EKA1 IPC headers and device selection](./n70-bluetooth-netplay-ipc-and-device-selection.md) |
| 2026-08-05 | [Chinese text drew as boxes because nobody assembled the ROM's linked fonts](./cjk-linked-fonts.md) |
| 2026-08-03 | [The stray signal behind `E32USER-CBase 46` was a DSA cancel completed twice](./dsa-cancel-double-completion-stray.md) |
| 2026-08-03 | [Symbian^3 File manager exits itself because the domain manager is missing](./x7-file-manager-domain-manager-exit.md) |
| 2026-08-02 | [Bluetooth Join outlives its query object, then indexes an empty friend list](./netplay-join-asker-lifetime-and-empty-friends.md) |
| 2026-08-02 | [Netplay query handlers parse the error path as if it were a datagram](./netplay-query-socket-error-null-buffer.md) |
| 2026-08-02 | [LAN netplay discovery crashes at startup on every getifaddrs platform](./lan-discovery-ifaddrs-null-deref.md) |
| 2026-08-02 | [N-Gage Call of Duty: uppercase AIF import + EKA1 clock divide-by-zero](./ngage-call-of-duty-import-and-eka1-clock-div0.md) |
| 2026-08-02 | [Nokia 5320 Voice Recorder investigation](./5320-voice-recorder-investigation.md) |
| 2026-08-02 | [Snakes dies on the N95: a screen-driver import no ROM can resolve](./scdv-aeabi-idiv-missing-rom-export.md) |
| 2026-08-01 | [EKA1 DSA framebuffer depth](./eka1-dsa-framebuffer-depth.md) |
| 2026-08-01 | [Debug write command descriptor bound](./debug-command-write-descriptor-bound.md) |
| 2026-08-01 | [Dragon.World flickers black on every server recomposite](./dragon-world-empty-redraw-black-flicker.md) |
| 2026-07-30 | [Ashen stops before its first frame after stale-handle validation](./ios-ngage-object-handle-alias-regression.md) |
| 2026-07-30 | [Sky Force stays white on the 6680](./ios-6680-sky-force-window-opcode.md) |
| 2026-07-30 | [A ScreenPlay stride fix breaks landscape DSA on the 5320](./ios-5320-landscape-dsa-stride-regression.md) |
| 2026-07-30 | [Talking Tom loses audio and flashes black between actions](./ios-talking-tom-audio-and-frame-composition.md) |
| 2026-07-30 | [Talking Tom recording corrupts the host heap on iOS](./ios-talking-tom-recording-input-race.md) |
| 2026-07-30 | [Talking Tom stays black on X7: incomplete Belle MMF dispatch](./ios-talking-tom-belle-mmf-ipc-stall.md) |
| 2026-07-30 | [Sky Force Reloaded corrupts the X7 display](./ios-sky-force-reloaded-x7-framebuffer-stride.md) |
| 2026-07-30 | [Angry Birds Rio's main menu repeatedly starts TZSERVER](./ios-angry-birds-rio-tzserver-retry.md) |
| 2026-07-29 | [A guest's undefined STRD calls host address 0](./ios-dyncom-null-addressing-mode.md) |
| 2026-07-29 | [N-Gage Store re-entry jumps to an unmapped function pointer](./ngage-store-codeseg-data-relocation.md) |
| 2026-07-29 | [N-Gage Launcher Store hangs, reports OOM, or renders black](./ios-ngage-launcher-store-applist-recognize-hang.md) |
| 2026-07-28 | [N-Gage Launcher freezes while exiting: incomplete ETel notification cancels](./ios-ngage-launcher-rsk-etel-cancel-hang.md) |
| 2026-07-28 | [N-Gage Launcher exit deadlocks in the window focus callback](./ios-window-callback-lock-self-deadlock.md) |
| 2026-07-28 | [System menu corrupts UIKit collection storage](./ios-system-menu-context-menu-crash.md) |
| 2026-07-28 | [Bloks shows no app icon: SVGB `<text>` coordinate lists](./svgb-text-coordinate-lists.md) |
| 2026-07-27 | [First ThreadSanitizer pass over the iOS build](./ios-tsan-first-pass.md) |
| 2026-07-27 | [TestFlight crash triage, round 7](./ios-testflight-crash-triage-r7.md) |
| 2026-07-27 | [Play-done callback deadlocks a guest `Play()`](./ios-audio-play-done-kernel-lock-deadlock.md) |
| 2026-07-27 | [Audio keeps playing after "Exit Game"](./ios-audio-outlives-killed-app.md) |
| 2026-07-27 | [`ekatests` build and runtime failures](./ekatests-build-and-runtime-failures.md) |
| 2026-07-27 | [Installing a second device hangs until backgrounding](./ios-device-install-idle-event-lost-wakeup.md) |
| 2026-07-27 | [Flexible mapping detach left stale CPU TLB entries](./FLEXIBLE_MAPPING_DETACH_STALE_TLB.md) |
| 2026-07-27 | [X7 Asphalt 6 launch & race path — Belle SDK rebuild follow-up](./ios-asphalt6-x7.md) |
| 2026-07-26 | [Worms shows no app icon: MIF entries can hold a gzipped SVG](./mif-gzipped-svg-icons.md) |
| 2026-07-26 | [X-Plore exit kills the emulator: window teardown races the redraw walker](./ios-window-teardown-redraw-race.md) |
| 2026-07-25 | [N-Gage Tetris freezes: the stray-signal filter ate real completions](./ios-ngage-tetris-absorbed-signal-hang.md) |
| 2026-07-25 | [X-Plore on the N-Gage: EColor4K makes a guest allocate zero-byte surfaces](./ios-ngage-color4k-zero-byte-surface.md) |
| 2026-07-24 | [N-Gage folder import deadlocks while the guest is idle](./ios-ngage-folder-import-idle-deadlock.md) |
| 2026-07-23 | [Rescan devices — case-sensitivity data loss and a reset() crash](./ios-rescan-devices-case-and-reset-crash.md) |
| 2026-07-22 | [MMF `more_buffer` callback leaks the kernel lock on a throw](./ios-mmf-callback-kernel-lock-leak.md) |
| 2026-07-22 | [AudioUnit render callback must be a `noexcept` boundary](./ios-audiounit-render-callback-noexcept.md) |
| 2026-07-22 | [Angry Birds in-game "X" exit — null callback](./ios-angrybirds-exit-null-callback.md) |
| 2026-07-22 | [Native-only scripting patches on iOS](./ios-native-scripting-patches.md) |
| 2026-07-22 | [S60v3 empty Avkon menu breakpoint patch](./s60v3-empty-avkon-menu-breakpoint-patch.md) |
| 2026-07-21 | [Launch screen-kick vs. animation redraw font-atlas race](./ios-launch-kick-redraw-font-atlas-race.md) |
| 2026-07-21 | [TestFlight crash triage, round 5](./ios-testflight-crash-triage-r5.md) |
| 2026-07-17 | [Single-member intrusive ring corruption](./ios-intrusive-ring-single-member-corruption.md) |
| 2026-07-16 | [SMS PDU non-virtual destructor crash](./ios-sms-pdu-nonvirtual-destructor-crash.md) |
| 2026-07-16 | [Stale CPU TLB on cross-process decommit](./STALE_CPU_TLB_ON_CROSS_PROCESS_DECOMMIT.md) |
| 2026-07-16 | [S60v5 accelerometer inversion](./ios-s60v5-accelerometer-panel-mount.md) |
| 2026-07-16 | [`User::After` completion null-deref on teardown](./ios-notify-sleep-teardown-null-deref.md) |
| 2026-07-16 | [System-language switch app-list reload](./ios-language-switch-applist-reload.md) |
| 2026-07-16 | [6680 application launch exit](./ios-6680-avkon-app-launch-exit.md) |
| 2026-07-15 | [Legacy AIF icon mask polarity](./ios-legacy-aif-icon-mask-polarity.md) |
| 2026-07-14 | [Asphalt 6 menu car material](./asphalt6-menu-car-material-investigation.md) |
| 2026-07-13 | [X7 Asphalt 6 launch & race path](./ios-asphalt6-x7.md) |
| 2026-07-13 | [iOS CoreMotion async IPC crash](./ios-sensor-async-ipc-crash.md) |
| 2026-07-11 | [5320 Camera panic 49](./ios-5320-camera-panic49.md) |
| 2026-07-11 | [Camera menu over viewfinder](./ios-camera-menu-over-viewfinder.md) |
| 2026-07-11 | [ipc_msg refcount UAF](./ios-ipc-msg-refcount-uaf.md) |
| 2026-07-10 | [TestFlight crash triage, round 2](./ios-testflight-crash-triage-r2.md) |
| 2026-07-08 | [N-Gage installer popup & game launch](./ios-ngage-installer-popup-and-game-launch.md) |
| 2026-07-08 | [N-Gage ONE launch deadlock — handoff](./ios-ngage-one-launch-deadlock-handoff.md) |
| 2026-07-08 | [TestFlight crash triage, round 1](./ios-testflight-crash-triage.md) |
| 2026-07-07 | [Stray-signal accounting — follow-up](./stray-signal-accounting-followup.md) |
| 2026-07-05 | [iOS productization checklist](./ios-productization-checklist.md) |
| 2026-06-15 | [Metal via ANGLE plan](./ios_metal_angle_plan.md) |
| 2026-06-14 | [dyncom optimization plan](./dyncom_optimization_plan.md) |
| 2026-06-14 | [Snakes / dyncom performance](./ios_snakes_perf.md) |
| 2026-06-14 | [Final Battle timer stray signal](./ios-final-battle-timer-stray.md) |
| 2026-06-13 | [Calculator Options menu hang](./ios-calculator-options-menu-hang.md) |
| 2026-06-05 | [Optimized-build Snakes stray signal](./ios-optimized-build-snakes-stray-signal.md) |
| 2026-06-03 | [Device missing patch DLLs](./ios-device-missing-patch-dlls.md) |
| 2026-06-02 | [App-list icons (SVG/MIF decode)](./ios-applist-icons.md) |
| 2026-06-02 | [Native AudioUnit backend](./ios-audiounit-backend.md) |
| 2026-06-02 | [Calculator EAGL magenta screen](./ios-calculator-eagl-magenta.md) |
| 2026-06-02 | [Document picker import](./ios-document-picker-import.md) |
| 2026-06-02 | [DSP / FFmpeg reconnection](./ios-dsp-ffmpeg.md) |
| 2026-06-02 | [Guest PC=0 deadloop](./ios-guest-pc0-deadloop.md) |
| 2026-06-02 | [Log flood pins CPU](./ios-log-flood-cpu.md) |
| 2026-06-02 | [Mount chunk SIGBUS](./ios-mount-chunk-sigbus.md) |
| 2026-06-02 | [mprotect silently strips W^X](./ios-mprotect-wx-strip.md) |
| 2026-06-02 | [Real ROM install pipeline](./ios-rom-install-pipeline.md) |
| 2026-06-02 | [SIS install silent write failure](./ios-sis-install-write-fail.md) |
| 2026-06-02 | [Snakes stray-signal freeze](./ios-snakes-stray-signal.md) |
| 2026-06-02 | [VFS case-sensitive path resolution](./ios-vfs-case-sensitive-path.md) |
| 2026-06-02 | [S60v5 AVKON FEP / PtiEngine stall](./s60v5-avkon-fep-pti.md) |
| 2026-07-31 | [Haptics-triggered AudioAnalytics crash](./ios-haptics-audioanalytics-crash.md) |
| 2026-08-01 | [A3F DevSound recording never delivers](./a3f-devsound-recording-never-delivers.md) |
| 2026-08-01 | [EKA1 GLES HLE patch path](./eka1-gles-hle-patch-path.md) |
| 2026-08-01 | [S60v2 binary colour-key icon mask](./s60v2-binary-colour-key-icon-mask.md) |
| 2026-08-02 | [Dropped DSP stream buffer-ready notifications](./ios-dsp-stream-buffer-notify-dropped.md) |
| 2026-08-02 | [NVG path decoding: unsigned coords and swallowed close-subpath](./nvg-path-decoding-signed-coords-and-close-subpath.md) |
| 2026-08-03 | [NVG extended bitmaps a guest blits itself](./nvg-extended-bitmap-guest-blit.md) |
| 2026-08-03 | [Avkon note renders without its skin background](./avkon-note-missing-skin-background.md) |
| 2026-08-03 | [A failed chunk allocation still handed the guest a usable handle](./hollow-chunk-null-mem-model.md) |
| 2026-08-03 | [Skin frames drawn by an app: the colour plane and the scanline padding](./nvg-skin-frames-colour-plane-and-padding.md) |
| 2026-08-03 | [Symbian^3 Camera exits at launch: a shredded central repository value](./x7-camera-quoted-cenrep-and-accessory-mode.md) |
| 2026-08-03 | [A camera for the iOS simulator, and the ECam buffer bug it exposed](./ios-simulator-camera-and-ecam-buffer.md) |
| 2026-08-04 | [Attaching to a flexible chunk was mistaken for owning it](./flexible-chunk-attach-lifetime.md) |
| 2026-08-04 | [The Clock application never draws: two servers that stop answering](./clock-app-alarm-list-and-nitz.md) |
| 2026-08-04 | [Why the time zone server is HLE'd, and why only from Symbian^3 up](./tzserver-hle-scope.md) |
| 2026-08-05 | [`destroy()` erased the object next to the one it was given](./destroy-erases-the-wrong-object.md) |
| 2026-08-05 | [Uninstalling an app the package manager never fully knew](./uninstall-package-of-an-app.md) |
| 2026-08-06 | [A damaged ROM crashed the app once, then on every launch after](./device-boot-crash-loop.md) |
| 2026-08-07 | [What blocks while a device installs, and the rule the Settings screen broke](./install-flow-main-thread.md) |
| 2026-08-07 | [A repository setting spelled `""` took the emulator down](./cenrep-ini-parser.md) |
| 2026-08-09 | [One unanswered IPC message black-screened every app on a device](./icon-server-hang-bricks-a-device.md) |
| 2026-09-03 | [The dialogue boxes a fresh bitmap's fill colour made disappear](./color4k-white-fill-colour-key.md) |
| 2026-09-03 | [Two-player mode killed the emulator when netplay was switched off](./netplay-off-bluetooth-crash.md) |
| 2026-09-04 | [Dungeon Hunter 2: menu music hiss and KERN-EXEC 3 on "Single Player"](./dungeon-hunter-2-gllive-address-overflow.md) |
| 2026-09-04 | [Installing a partial-upgrade SIS deleted the application it was patching](./sis-partial-upgrade-wipes-base-package.md) |
| 2026-09-04 | [The N-Gage card import that split the E drive into `system` and `System`](./ngage-card-import-case-split-system-folder.md) |
| 2026-09-05 | [Video flicker exposes missing window-surface binding semantics](./window-surface-binding-and-video-composition.md) |
| 2026-09-05 | [Ferrari GT on the X7: a flickering intro movie and a race clock running 20% fast](./ferrari-gt-video-overlay-and-fixed-step-clock.md) |
| 2026-09-05 | [Tilt steering was dead in Ferrari GT but fine in Asphalt 6](./sensor-buffering-count-exceeds-channel-buffer.md) |
| 2026-09-05 | [Icy Tower died on the X7 because the screen reported premultiplied alpha](./x7-window-mode-premultiplied-alpha.md) |
| 2026-09-08 | [A trackpad touch killed the Qt frontend](./qt-touch-point-slot-overrun.md) |
| 2026-09-22 | [Bringing up UIQ 2.1 / Symbian OS 7.0 (Sony Ericsson P900)](./UIQ_SYMBIAN70_PORTING.md) |

## Archived planning docs

These tracked the initial port to completion and are no longer updated:

- [`IOS_PORTING_PLAN.md`](./IOS_PORTING_PLAN.md) — original iOS port proposal/roadmap.
- [`IOS_PORTING_TASKS.md`](./IOS_PORTING_TASKS.md) — stage-by-stage task tracker with verification results and a changelog; the closest thing to a full project history.
