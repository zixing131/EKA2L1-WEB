# Icy Tower died on the X7 because the screen reported premultiplied alpha

Icy Tower (`0x2003B10C`) panics with KERN-EXEC 3 during startup on the X7 (rm-707),
right after it opens `E:\private\2003B10C\res\r6.bin`. The same install on the N97
(rm-507) reaches the title screen.

## Narrowing it down

The log gives the faulting address but nothing else, so the exception handler was
temporarily extended to dump the CPU context, the stack, and the whole 1.8 MB code
segment of the running process (`0x70000000`, which the running image has already
decompressed — the file on disk is compressed).

```
Access violation reading address 0x4D1000 in thread Icy Tower
pc 0x70121DD0 sp 0x48E5338
r4 0x4D1004  r6 0x48D7B30  r7 0x4D9000
```

Disassembling around the PC gives a plain 32bpp red/blue swizzle:

```asm
0x70121DD0: ldr  r2, [r4], #4     <-- fault
0x70121DD4: bic  r3, r2, #0xff0000
...
0x70121DEC: str  r3, [r0], #4
```

and dumping the two objects it works on gives the geometry: source and destination are
both 415 x 59, source pixels at `0x4BF000`, destination at `0x4D9000`. The heap chunk
starts at `0x4B6000`, so the source buffer is at heap offset `0x9000` — exactly the
block a `RChunk::Allocate(0x12000)` had returned, ending at `0x1B000`. The fault
address `0x4D1000` is that block's first byte past the end.

The arithmetic settles it. 415 x 59 x 4 = 97940 bytes, which the destination got
(`Allocate(0x18000)`). The source only ever got 73455 bytes rounded up to a page:
**415 x 59 x 3**. The game allocated the source for a narrower pixel and then read it
with a 32bpp loop.

## Why it picks the wrong pixel size

The call site is guarded by a global:

```asm
0x700EE06C: ldr   r3, [pc, #0x94]   ; &0x415644
0x700EE070: ldrb  r3, [r3]
0x700EE074: cmp   r3, #0
0x700EE078: beq   0x700EE084        ; skip the conversion
0x700EE080: bl    0x70121CD8        ; the 32bpp swizzle
```

and that global is written by a display-mode switch:

```asm
r0 = <CWsScreenDevice::DisplayMode()>
*0x41563C = r0                      ; raw mode
cmp r0, #0xB                        ; EColor16MU  -> 4 bytes/pixel
bgt ...  cmp r0, #0xC               ; EColor16MA  -> 4 bytes/pixel
                                    ; anything else -> EColor64K, 2 bytes/pixel
```

Dumping those globals at the fault confirms the path taken:

```
0x41563C = 0x0D   (13, EColor16MAP — what the screen reported)
0x415644 = 0x07   (EColor64K — what the game decided to use)
0x415648 = 0x02   (bytes per pixel)
```

The N97 reports `EColor16MA` (12) and takes the 4-bytes-per-pixel branch, so its source
buffers are wide enough and the same swizzle loop stays inside them.

EKA2L1 reports 13 because the X7 ROM's `wsini.ini` says `WINDOWMODE Color16MAP` and the
window server hands that value straight to clients.

A device does use `WINDOWMODE` to pick the screen device's format — `CScreen::
CreateScreenDeviceL` reads `SCREENMODE` and falls back to `WINDOWMODE`
(`nonnga/SERVER/screen.cpp:314`), and the OpenWF render-stage plugin reads the same key
(`windowserverplugins/openwfc/src/displayrenderstage.cpp:148`). `EColor16MAP` is a
perfectly legal screen device mode; the memory-mapped driver has a draw device for it
(`CDrawThirtyTwoBppScreenBitmapAlphaPM` in `screendriver/smomap/scnew.cpp`).

What differs is what clients are *told*. `CScreen::DisplayMode()` returns
`iScreenDevice->DisplayMode()`, and from Symbian^3 on that screen device is the render
stage, which folds every 32bpp format to one canonical answer
(`displayrenderstage.cpp:313`):

```cpp
TDisplayMode CDisplayRenderStage::DisplayMode() const
    {
    const TInt KThirtyTwoBpp = 32;
    const TDisplayMode dm = iRenderTarget->DisplayMode();
    const TInt bpp = TDisplayModeUtils::NumDisplayModeBitsPerPixel(dm);
    return bpp == KThirtyTwoBpp ? CFbsDevice::DisplayMode16M() : dm;
    }
```

`CFbsDrawDevice::DisplayMode16M()` is defined per screen driver, and the memory-mapped
driver real devices use returns `EColor16MA` (`smomap/scnew.cpp:197`; the Windows
emulator's driver returns `EColor16MU` instead). So a Symbian^3 device with a 32bpp
screen tells clients `EColor16MA` whatever its `wsini` asked for — exactly what the N97
reports, and what Icy Tower's 4-bytes-per-pixel branch expects.

The pre-NGA window server has no such fold: there `CScreen::DisplayMode()` really is the
screen device's own mode, so a S60v5 ROM asking for `Color16MAP` would report it.

## Fix

Apply the same fold: from `epoc10` on, a 32bpp `WINDOWMODE` is reported to clients as
`color16ma`. Older versions keep passing the ini value through, the way their window
server does.

## Why it only shows on the X7

The out-of-bounds read is one page past the end of a heap block, and on Symbian^3 the
user heap lives in a *disconnected* chunk: pages the allocator has not committed simply
are not there. The N97's heap is a normal, fully committed chunk, so the same read off
the end of the same block lands on mapped memory and nobody notices. That is worth
remembering as a general shape: an allocation bug that only panics on Symbian^3 devices
is not necessarily Symbian^3-specific — it may just be the only place the emulator can
see it.
