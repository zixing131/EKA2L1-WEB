# EKA1 devices never got the GLES HLE patch, so 3D titles ran Nokia's software rasterizer

## Symptom

Nokia 6680 (rm-36, `epoc80`) → *Karapuzzz Tetris 3D* v1.10.

The game boots, the help screen and the Options menu render and respond, but *New game* leads to a
screen that is black except for a single blue digit in the bottom-left corner (the level counter,
drawn through the window server). The FPS counter sits at 2. After roughly half a minute the guest
dies with `KERN-EXEC 3`.

The emulator log shows no panic before the access violation, and the game's own drawing looks
healthy — so the failure is entirely inside whatever renders the 3D playfield.

## Narrowing down

### The window server sees almost no drawing

Logging every graphics-context opcode the guest submits (window server client version `epoc80`
resolves to the `v139u` opcode table) showed the in-game frame is just:

```
active / set_pen_* / set_brush_* / use_font(55) / draw_rect(46) / draw_text(33) / deactive
```

One rectangle and one string per frame — the level counter and nothing else. The playfield never
reaches the window server at all, so it must be drawn through a lower-level path.

### The access violation points at the real culprit

The faulting reads walk 0x11DE0 → 0x11DFC in 4-byte steps: one `LDM` of eight registers. Dumping
PC/LR and mapping them onto the loaded code segments gave:

```
pc = euser.dll   + 0x10DA8      (Mem::Copy inner block loop)
lr = libgles_cm.dll + 0x40F4
```

**`libgles_cm.dll`** — the guest is running Nokia's own OpenGL ES implementation as ARM code.
Disassembling around the call site:

```
0xE02040CC: ldr r0, [r4, #0x3c]   ; destination colour buffer
0xE02040D8: add r2, r4, #0x40
0xE02040DC: ldm r2, {r2, r3}      ; width, height
0xE02040E4: ldr r3, [r4, #0x1c]   ; bytes per pixel
0xE02040E8: mov r1, r5            ; source = argument
0xE02040EC: mul r2, r3, r2        ; length = w * h * bpp
0xE02040F0: bl  Mem::Copy
```

Dumping the object at `r4` gave `width = 0xB0 (176)`, `height = 0xD0 (208)`, `bpp = 2`, so the copy
length is `0x11DC0`. The faults end at `0x11E00`, which puts the source pointer at **0x40** — a
near-null address the library got from its caller (`ldr r1, [r4, #0x2c]` one frame up).

Two dead ends worth skipping: the HAL display attributes are *not* where that pointer comes from —
`EDisplayMemoryAddress` is never queried, and `screen_info` correctly reports the DSA chunk at
`0x58000000`. Chasing where the bogus pointer is stored inside the library is also wasted effort,
because the library should not be executing at all.

### Why the library was executing

`register_functions()` in `src/emu/dispatch/src/libraries/register.cpp` installs the HLE trampolines
that replace the guest GLES/EGL exports with the emulator's own GLES1 dispatch. It only did so for
`epocver >= epoc93fp1`, and only under `Z:\Sys\Bin\`. EKA1 devices keep the library somewhere else:

```
rm-36  (epoc80)    Z:\System\Libs\libgles_cm.dll
rm-320 (epoc93fp1) Z:\Sys\Bin\libgles_cm.dll
```

So on the 6680 the patch matched nothing, the real software rasterizer ran, and it depended on
frame buffer plumbing the emulator does not reproduce.

## Fix

Patch `Z:\System\Libs\libgles_cm.dll` on the EKA1 branch too, gated on the same `enable-hw-gles1`
setting.

The ordinal table is shared with the newer devices, which is safe because Symbian freezes DEF
files: `libgles_cm.dll` exports EGL at ordinals 1–25 and GL above that on both. The 6680 copy only
exports 132 entries (GLES 1.0) against the table's 158; `patch_libraries()` skips ordinals it cannot
resolve, so the 1.1 additions simply do not get patched.

Neither ROM ships a separate `libegl.dll`, so EGL comes from the same library on both — no extra
path is needed.

With the patch applied the playfield renders through the host GPU, the game plays to game over, and
the access violation is gone.

## Also changed

`screen::resize()` created the screen texture without defining its contents. A fullscreen app that
leaves part of the screen untouched — the control pane strip, here — presented that undefined GPU
memory. Requesting a clearing redraw when the texture is first created makes the untouched area
black instead of garbage.

## Known remaining issue

Tetris 3D still paints a garbled 176×20 strip at `y = 188`. It is the game's own `BitBlt` of a
runtime-created 176×20 `color64k` bitmap whose contents are wrong; the blit geometry the window
server receives is correct. This is unrelated to the GLES path (it predates the fix) and does not
affect other 6680 titles — Sky Force renders fullscreen cleanly.
