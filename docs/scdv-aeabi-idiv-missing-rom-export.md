# Snakes dies on the N95: a screen-driver import no ROM can resolve

## Symptom

Launching Snakes (`0x2000730F`) on the N95 (rm-320) terminated the guest with
`KERN-EXEC 3` a few seconds after the window server handed it the screen. The
same title reached its main menu on the 5320 (rm-409), and the N95 had worked
before.

The log gave almost nothing:

```
Trying to open a non-existent file: E:\Snake60defaults.txt ...
Unimplement system call: 0xFFFFFF!
Unimplement system call: 0xFFFFFF!
Unimplement system call: 0x353472!
Access violation reading address 0x8C308DA9 in thread Snakes
```

Those "system calls" are not real SVCs. They are what the interpreter reports
when the guest is executing arbitrary bytes that happen to decode as `SVC`.

## Diagnosis

Dumping the guest register file at the fault and mapping every address onto a
loaded code segment turned the noise into one line:

```
pc=0x98  lr=0x83F006B9  sp=0x8C308DA5
lr 0x83F006B9 => scdv_general.dll + 0x6B8
```

`PC` was walking upward from the bottom of the address space and `SP` held two
RGB565-looking halfwords, so the thread was already destroyed by the time it
faulted. `LR` was intact and pointed one instruction past
`scdv_general.dll + 0x6B4`:

```
06b0  ldr   r0, [r4, #0xc]
06b2  muls  r0, r3, r0
06b4  blx   #0x41b0          ; -> ARM veneer: LDR pc, [pc, #-4]
06b8  b     ...              ; == LR, thumb bit set
```

`0x41B0` is an import veneer whose target word is patched at load time. The call
site matches `CFbsDrawDeviceAlgorithm::LongWidth()` in
`src/patch/scdv/src/drawdvcalgo.cpp`:

```cpp
TInt CFbsDrawDeviceAlgorithm::LongWidth() const {
    if (iOrientation & 1) {
        const TSize size = SizeInPixels();
        return (size.iWidth == 0) ? 0 : iLongWidth * size.iHeight / size.iWidth;
    }
    return iLongWidth;
}
```

`ScanLineBytes()` just above it has the same shape. Both divide only when the
draw device is rotated — which is why a landscape title on the N95 hit it while
the 5320's portrait Snakes menu did not. Every other division in scdv is by a
compile-time constant, so it compiles to a multiply and needs no helper.

`git bisect` (12 builds, good `750dfe199` → bad `90bc746af`) landed on
`78ff7124e`, "fix(scdv): rebuild Belle patch from source". That commit changed
nothing but the delivered `scdv_general.dll`: the byte-patched image was
replaced by a full Nokia Symbian Belle SDK / GCCE build.

Logging the importer next to the existing "Invalid ordinal" error named the
veneer exactly:

```
Invalid ordinal 222, requested from drtaeabi.dll (exports=221)
  by scdv_general.dll at code+0x41B4
```

`code+0x41B4` is the word behind the veneer at `0x41B0`. The import resolved to
`0`, so the `BLX` branched to address zero.

Ordinal 222 of `drtaeabi.dll` is `__aeabi_idiv` — read out of the SDK's
`drtaeabi.dso`, where a symbol's `st_value` is `(ordinal - 1) * 4`:

```
221 abort
222 __aeabi_idiv
223 __aeabi_uidiv
```

The SDK import library exports 227 ordinals. **Every** ROM EKA2L1 supports
exports exactly 221 — rm-320, rm-409 and rm-707 all report `exports=221`, and
the X7 is itself a Belle-era device. The two helpers were appended to the frozen
DEF after the last of those firmwares shipped, so no device can satisfy that
import.

The link map explains why the SDK copy won at all: `libgcc.a`, which carries its
own `__aeabi_idiv`, *is* on the link line — but after `drtaeabi.dso`. The linker
resolved the symbol from the import library first and never pulled libgcc's
definition. The original upstream binary predates that SDK and had the helper
linked in statically, which is why only the rebuild broke.

The landmine was present on all devices from `78ff7124e` onward, not just the
N95. It only detonates where a guest puts a scdv draw device into a rotated
orientation.

### Dead ends worth skipping

* The three `Invalid ordinal` lines at startup look identical on a working 5320
  and a crashing N95, so the log alone suggests background noise. The message
  did not say *which* codeseg asked, and two of the three come from
  `ecam_general.dll`.
* `Unimplement system call: 0xFFFFFF` reads like a missing executive call. It is
  only ever the interpreter decoding garbage; treat it as "the guest is already
  lost", not as a lead.
* The nearby scdv/DSA commits (`52308e205`, `2559fefff` framebuffer pitch,
  `fcefd8324` EKA1 screen mode) are plausible suspects and all innocent.
* The N95 and the 5320 are both 240x320 and both pre-ScreenPlay, so screen
  geometry explains nothing — orientation does.
* Synthesising the two helpers inside the emulator and resolving the failed
  import to them also works and was verified end to end, but it leaves the
  shipped DLL carrying an import that is wrong on every real device.

## Fix

`src/patch/scdv/src/aeabi.cpp` defines `__aeabi_idiv` and `__aeabi_uidiv` (a
restoring division; ARMv5 has no divide instruction) and is listed in
`scdv_general.mmp`. Symbols defined by the project's own object files are
resolved before any library, so the helpers stay internal no matter which SDK
builds the DLL, and no ROM import is emitted.

Rebuilt with the Belle SDK in the Windows XP VM per AGENTS.md. The ABI is
unchanged — 31 exports, `10000079 10003B19 EE000002`, ARMV5/EKA2/DEFLATE — and
code size grows `0x6E24` → `0x6ECC`. The map confirms `__aeabi_uidiv` at
`0x8050` and `__aeabi_idiv` at `0x8094` inside the image, and the emulator no
longer reports ordinal 222 on any device.

The emulator side keeps reporting an unresolvable import as an error rather than
quietly patching in a substitute, but the message now names the importing
codeseg, the provider's export count, and the offset of the veneer it poisoned.
That is the line that turns "guest ran off to address zero somewhere" into a
one-step diagnosis.

Other unresolved imports (`ecam_general.dll` against `fbscli.dll`,
`postingsurfacefactory_general.dll` against a 6680 `euser.dll`) are genuinely
missing platform APIs, not runtime helpers, and are left reported.

## Verification

Snakes on the N95 reaches its main menu and plays the 3D board at ~40 FPS with
no guest fault, and the startup log no longer mentions ordinal 222. Release
regression: standard 12/12, `angrybirds` 5/5, `asphalt6` 9/9 — the last covers
the Belle draw-device path the rebuilt DLL was originally made for.
