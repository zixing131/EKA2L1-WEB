# Symbian^3 Camera exits at launch: a shredded central repository value

## Symptom

On the X7 (rm-707, Symbian^3/Anna), launching the built-in **Camera**
(`cameraapp.exe`, UID3 `0x101F857A`) flashes nothing and drops straight back to the
app list about a second later. No panic dialog, no access violation. The 5320
(rm-409, S60v3 FP2) Camera behaves normally, so it does not look like a generic
camera-stack problem.

## Narrowing it down

`extensive-logging: true` plus `log-ipc: true` in `Documents/data/config.yml` shows
the same signature the Symbian^3 File manager had before its domain-manager fix:

```
Thread cameraapp forcefully killed with category: None and exit code: -1
```

Exit code −1 (`KErrNotFound`) with `entity_exit_type::kill` is `E32Main` returning an
error — `EikStart::RunApplication`'s TRAP result, not a panic. Two leaves precede it,
both trapped. The first one sits directly under an unmistakable line:

```
CenRep: Try to open repo 0x10282EDC
CenRep: Repository not found with UID 0x10282EDC
Kernel: Leave started! Guess leave code: -1
```

`0x10282EDC` (owner `0x10282EDB`, the camcorder MMF plugin settings) is the video and
image quality-level repository the Camera app reads while constructing. The file
*does* exist in the ROM: `z:\private\10202be9\10282edc.txt`. So the repository was
found and rejected, not missing — `parse_new_centrep_ini()` returned false.

`file(1)` gives the first real hint: unlike its neighbours, this repository has
"very long lines (548)". Its `[Main]` entries look like

```
0x10001 string "QualitySetLevel=98,VideoFileMimeType=video/3gpp,VideoCodecMimeType=video/H263-2000,..." 0 cap_rd=alwayspass
```

— one quoted value packing a whole comma-separated settings list, several of which
also contain `=` and `; profile-level-id=2`.

Building `common/src/ini.cpp` into a small host harness (it only needs
`dynamicfile.cpp` and `cvt.cpp`, plus a `compare_ignore_case` stub) and dumping the
parse tree made the failure obvious, and it was worth the two minutes: iterating on
the tokenizer against the real ROM files takes seconds instead of a full iOS build
and simulator round trip.

## Root cause

`ini_linestream::next_string()` opened a quoted token by setting its stop character to
`"`, but the scan loop *also* stopped unconditionally at `,` and `\t`:

```cpp
while (counter < line.length() && line[counter] != cto_stop
    && line[counter] != ',' && line[counter] != '\t') {
```

So the value above was cut at the first comma into `QualitySetLevel=98`, which then
went through the `=` splitting stage and became a nested `ini_pair`. The rest of the
line degenerated into a mixture of stray values, `,` tokens and pairs, and one of them
landed where a type token was expected. `indentify_central_repo_entry_var_type()` got
`320` instead of `string`, returned false, and the whole repository failed to load.

Two smaller defects sat on top of the same code: the closing quote was never consumed
(so the next call read it as an *opening* quote and swallowed the rest of the line),
and `=` splitting was applied to quoted content at all.

Across the installed ROMs this affected more than one repository — rm-707 went from
17 to 16 unparseable `.txt` repositories, rm-409 from 7 to 6, rm-507 from 8 to 7. The
remainder fail for an unrelated reason (`string8` is not among the recognised entry
types, and one file puts a `cap_wr=` line inside `[Main]`); those are pre-existing and
are not part of this fix.

## Fix

`ini_linestream::next_string()` treats a quoted token as literal: it ends only at the
closing quote, that quote is consumed, and no `key=value` splitting is applied to its
contents. Unquoted tokenizing is untouched, which the existing `test.ini` / `test2.ini`
expectations confirm; `test3.ini` covers the quoted case.

## A second blocker behind it: RAccessoryMode

With the repository loading, the Camera app no longer exits — and hangs instead, on a
black screen at 0 FPS, right after:

```
Service.Accessory: Unimplemented opcode for accessory session 0x0
```

Opcode 0 is `EAccSrvCreateSubSessionAccessoryMode`: the app opens an `RAccessoryMode`
subsession to learn whether a headset or TV-out is attached. EKA2L1 only implemented
the connection subsessions, and the unhandled-opcode paths merely logged. Every one of
these requests is a synchronous `SendReceive`, so a request that is never completed
blocks the client thread for good.

So the accessory server now:

* implements the `RAccessoryMode` subsession — create/close, `GetAccessoryMode`
  (sync and async) reporting `EAccModeHandPortable` with no active audio output,
  and a mode-changed notification that is registered and never fires, which is the
  correct behaviour for an emulator where nothing is ever plugged in or out;
* completes every unrecognised opcode with `KErrNotSupported` instead of leaving the
  message outstanding, at session and at both subsession levels.

## Where the X7 Camera stops now: CCameraAdvancedSettings

With a camera present (the simulator's synthetic backend, see
[the simulator camera writeup](./ios-simulator-camera-and-ecam-buffer.md)) the app
gets past the crash below and dies a different way: `E32Main` returns −5,
`KErrNotSupported`. A probe in `leave_start` that resolves LR and walks the guest
stack named the frame:

```
LEAVEPROBE code=-5
  stack -> ecamadvsettings.dll+0x117
  stack -> ecamadvsettings.dll+0x171
  stack -> cameraapp.exe+0x32DB5
  stack -> cameraapp.exe+0x12027
```

`CCamera::CCameraAdvancedSettings::ConstructL` asks the camera for its advanced
settings implementation and leaves with `KErrNotSupported` when it gets none:

```cpp
TAny* CCameraPlugin::CustomInterface(TUid aInterface) {
    return NULL;
}
```

The ECam patch DLL implements the base `CCamera` only. `ecamadvsettings.dll`,
`ecamimageprocessing`, `ecamsnapshot` and `ecamdirectviewfinder` are all stock ROM
DLLs that reach the camera exclusively through `CustomInterface()`, and the
Symbian^3 Camera app requires the advanced-settings one during construction. S60v3
cameras do not, which is why the 5320 never needed it.

Fixing this means implementing `MCameraAdvancedSettings` in `src/patch/ecam` and
returning it from `CustomInterface()`. That interface is large, and the log shows the
app also pulls in `ecamimageprocessing`, `ecamsnapshot` and `ecamdirectviewfinder`, so
construction is unlikely to be the last custom interface it wants. Not attempted; the
X7 Camera is parked here deliberately.

(The patch DLL can now be rebuilt — see
[the simulator camera writeup](./ios-simulator-camera-and-ecam-buffer.md) for how — so
this is a matter of scope, not tooling.)

One unrelated defect was found and fixed on the way: the epoc10 exec table had no
entry for `0xBE`, so `RProperty::Delete` fell through as "Unimplement system call"
and returned a stale r0. It fires on this app's teardown path but is not the cause
of the exit.

## What is left, and why the simulator cannot judge it

Past that point the Camera app reaches its own UI construction and dies with
`KERN-EXEC 3` reading `0x104`. A probe on `ecam_number_of_cameras` explains it:

```
number of cameras = 0
Access violation reading address 0x104 in thread Camera
pc=0x70032F32 (cameraapp.exe+0x32F32) lr=0x7000C187 (cameraapp.exe+0xC187)
```

`cameraapp.exe+0xC180` is `ldr r0, [r4, #0x40]` followed by a call into the two-line
accessor `adds r0,#0xff; adds r0,#1; ldr r0,[r0,#4]`, i.e. an accessor invoked on a
null object. The iOS *simulator* has no `AVCaptureDevice`, so
`CCamera::CamerasAvailable()` legitimately returns 0 and the Symbian^3 Camera app
never builds that controller. The 5320's Camera app hits the same zero-camera
condition but unwinds cleanly, which is why it looks healthy on the simulator.

That last crash is therefore a property of the host having no camera, not of the two
bugs fixed here, and the real-device path has to be verified on hardware.

## Verification

X7 Camera no longer exits at launch and no longer hangs in the accessory server; on
the simulator it now runs into the zero-camera path described above. The X7 File
manager still opens and its log contains no `Repository not found` at all. The
standard regression suite (Final Battle, Calculator, N95 Calculator, string catalog)
passes 12/12 and the Angry Birds touch suite 5/5 on a Release simulator build.

The temporary probes used during the investigation (an access-violation PC/LR/codeseg
dump in `kernel_system::cpu_exception_handler` and a camera-count trace in
`ecam_number_of_cameras`) have been removed.
