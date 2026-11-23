# X-Plore on the N-Gage: EColor4K makes a guest allocate zero-byte surfaces

> **Correction, 2026-08-30:** the X-Plore diagnosis below is still useful, but
> its global workaround was wrong and has been reverted. A 12-bit N-Gage
> framebuffer occupies 16 storage bits per pixel, but those bits are XRGB4444,
> not RGB565. Reporting `EColor64K` avoids X-Plore's guest-side 12-bit table bug
> only by changing the pixel format for every application. Cybersaurus then
> renders with a severe cyan/green cast. EKA2L1 now keeps the real `EColor4K`
> mode; an X-Plore-specific compatibility setting is the appropriate place for
> a workaround if one is retained.

## Symptom

Opening X-Plore (`x-plore_s60_1_56.sis`) on the classic N-Gage device `nem-4`
(EPOC6/EKA1, S60v1) shows a "black screen". The black screen is actually the guest's own
panic dialog: **`Process X-plore / panic / Category USER / Reason 44`**.

`USER 44` is `ETHeapFreeBadNextCell`: in `RHeap::Free()`, the cell being freed overlaps the
first free-list cell above it (`pC + pC->len > n`). So the guest's `RHeap` free list is
corrupt. Fully deterministic — same registers, same addresses on every run.

## Dead ends worth not repeating

An earlier pass burned a lot of time on the assumption that the emulator was corrupting
guest memory. All of these were checked and are **not** the cause:

- **CPU backend.** Reproduces identically on dyncom *and* dynarmic, so it is not an
  instruction miscompute.
- **SIS install / file I/O.** The installed `Data.dta` is a valid, intact ZIP. Logging every
  `fs_server_client::file_read` for that file shows byte-correct results: local header at the
  right offset, `PK\x03\x04`, method 8, and deflate payload offsets that match the on-disk
  file exactly.
- **Page-table aliasing / remapping.** The tempting theory was that a heap page silently got
  re-pointed at another chunk's host memory, which would explain corruption with no
  observable write. Disproven by walking every heap page at panic time and comparing
  `ptr<>::get(process)` against the chunk's own contiguous host mapping — no unmapped and no
  discontiguous page.
- **Chunk commit/decommit stale data.** A memset-on-newly-committed-page experiment in
  `multiple_mem_model_chunk::commit` did not change anything; the churn happens inside
  already-committed chunk space.
- **`Invalid ordinal N, requested from euser.dll`** floods the log and looks alarming. It is
  entirely EKA2L1's *own* patch DLLs (`ecam_general`, `mediaclientvideo_v100`,
  `postingsurfacefactory_general`, `audiooutputrouting_general`) — built against a modern
  euser with ~2100 exports — being loaded against this ROM's euser, which has 1679. Nothing
  to do with X-Plore.
- **Watching one cell header.** The previous pass watched the 32 KB zlib window cell's
  neighbour and saw only legitimate euser writes, and concluded the bad cell was "somewhere
  else in the free list". True, but the window cell was a red herring: the real writer is
  elsewhere entirely.

## How it was actually narrowed down

Three instrumentation steps did all the work.

**1. Establish *when* the heap breaks, at guest-instruction granularity.** `thread::kill`'s
panic branch dumped the panicking thread's `RHeap` (found via `ldata->heap`), which for this
process sits at `0x700000`: `iBase = 0x70007c`, `iTop = 0x777000`, `iFree.next` at
`RHeap+0x30`. Walking the free list and then the *linear cell chain* (every byte in
`[iBase, iTop)` belongs to exactly one cell, so summing lengths must land exactly on `iTop`)
gave a strong, cheap invariant. Checking that invariant on every SVC entry/exit for this
process showed the heap intact until the very first corrupting instruction — which also ruled
out any host-side/HLE writer, since those all happen inside service calls.

The linear-chain walk matters: a free-list-only check cannot detect a cell that is
simultaneously allocated and on the free list.

**2. Find the writer.** A watchpoint in `ARMul_State`'s write paths — **including
`WriteMemory32Block`, the LDM/STM bulk cursor, which an earlier pass missed** — caught it:

```
W32 addr=0x707104 data=0x8  pc=0x503a7f7c   <- euser builds an 8-byte free cell here
W16 addr=0x707104 data=0x0  pc=0xe0157384   <- 16-bit zero lands on its length header
```

`0xE0000000` is `ram_code_addr_eka1`, and no codeseg covers `0xe0157384`: X-Plore's
`x-plore.app` codeseg is only `0xa48` bytes of text. X-Plore ships a small Symbian stub that
loads its real ~290 KB payload into the RAM-code region itself, so the payload is invisible
to `get_codeseg_list()`. Dumping and disassembling guest memory there gives:

```
0xe01572b8  push {r4-r8, sb, sl, lr}     ; Convert32To16(src32, dst16, count, ...)
...
0xe0157380  bl   #0xe0180c40             ; 32bpp ARGB -> display pixel
0xe0157384  strh r0, [sb], #2            ; store 16bpp pixel, advance dest
```

with an ordered-dither matrix and the payload's `"1.1.3"` version string sitting next to it at
`0xe018db90`. The caller passes a surface descriptor whose fields decode cleanly:

```
S+04 = data pointer   S+0c = width   S+10 = height   S+14 = byte stride   S+18 = bytes/pixel
```

For the first corrupting call: width 6, height 2, stride `0xc`, so the routine writes exactly
`6 * 2 = 12` halfwords = 24 bytes. The destination `0x702a64` is the payload of a heap cell
whose header at `0x702a60` has length **8** — four usable bytes. A 24-byte blit into a 4-byte
allocation.

**3. Find who sized the allocation.** Disassembling the payload's surface-create path:

```
0xe01587a0  ldreq r3, [r8, #8]      ; display-mode descriptor
0xe01587a4  ldrbeq r2, [r3, #0xc]   ; descriptor's BYTES per pixel
0xe01587a8  strbeq r2, [r8, #0x24]
0xe01587ac  ldrbeq r3, [r3, #0xd]   ; descriptor's BITS per pixel
0xe01587b4  add   r2, r8, #0x14
0xe01587b8  ldm   r2, {r2, r3}      ; width, height
0xe01587bc  mul   r2, r3, r2
0xe01587c0  ldrb  r3, [r8, #0x24]   ; bytes per pixel
0xe01587c4  mul   r0, r3, r2        ; size = width * height * bytesPerPixel
0xe01587c8  bl    #0xe016e14c       ; allocate
0xe01587cc  str   r0, [r8, #0xc]
```

Logging those factors at the allocation site is the punchline:

```
[TEMP-ALLOC] obj=0x7074c4 w=6 h=2 bpp=0 bits=12  desc.bpp=0 desc.bits=12
```

**12 bits per pixel, 0 *bytes* per pixel.** `size = 6 * 2 * 0 = 0`, so `Alloc(0)` returns
RHeap's minimum cell (8 bytes, 4 usable). Later the blit path derives 2 bytes per pixel from
the same 12-bit mode and writes 24 bytes — straight over the neighbouring free cell's length
header. The next unrelated `RHeap::Free()` walks into that zeroed length and panics
`USER 44`.

So the heap corruption is a *guest* write, but the emulator handed the guest the value that
makes it inevitable.

## Historical diagnosis

`window_server::init` derives the EPOC6 screen mode from the ROM header:

```cpp
const epoc::display_mode conv_res =
    epoc::get_display_mode_from_bpp(rom_info->header.eka1_diff1.bits_per_pixel, true);

if (scr_mode_global != epoc::display_mode::color_last) {   // <-- wrong variable
    scr_mode_global = conv_res;
    use_in_ini = false;
}
```

The N-Gage ROM header reports 12 bits per pixel, so `conv_res` is `EColor4K`, and
`get_bpp_from_display_mode(color4k)` reports 12 bits to the guest. X-Plore's display-mode
table has no byte stride for a 12-bit mode, so its bytes-per-pixel stays 0.

A 4K-colour panel is not a packed 12-bit framebuffer: it is driven through a 16-bit
framebuffer with the top four bits unused, which is why EKA2L1's own `get_byte_width()`
already treats `case 12:` and `case 16:` identically. Reporting `EColor4K` is therefore
gratuitously hostile to any guest that maps a display mode to a byte stride, while gaining
nothing — the in-memory layout is 2 bytes per pixel either way.

The same line also has a plain bug: the guard tests `scr_mode_global` (unconditionally
`color16ma` at that point, so always true) instead of `conv_res`. Since
`get_display_mode_from_bpp` returns the `color_last` sentinel for a bpp it cannot map, a ROM
with an unusual value would have had that sentinel written straight into the screen mode. The
`use_in_ini` branch's EKA1 `bpp > 16` clamp never protected the ROM path at all.

## Historical workaround (reverted)

The original workaround in `src/emu/services/src/window/window.cpp` tested
`conv_res` against `color_last` as intended, then mapped a ROM-derived
`EColor4K` to `EColor64K` for EKA1. The sentinel check remains correct; the
display-mode remapping does not.

With that workaround, the same instrumentation reported `bpp=2 bits=16`, every surface allocation was
correctly sized, the heap-invariant check never fires, and X-Plore renders its license
dialog and file browser.

Cybersaurus supplied the missing control case. It writes native 16-bit words
whose channel layout is XRGB4444. Interpreting those words as RGB565 changes
both the channel widths and bit positions, which produces the visible colour
cast. Symbian's own screen-driver source agrees with that distinction: 12 HAL
bits select `EColor4K`/XRGB4444, while 16 select `EColor64K`/RGB565. Storage
width alone therefore cannot select the display mode.

## Historical verification

- X-Plore on `nem-4`: reaches its License agreement dialog; zero heap-invariant violations,
  zero panics.
- Ashen on `nem-4` (control, and a title that writes the screen buffer directly): renders at
  48 FPS with correct colours, no panic — so the 4-4-4 → 5-6-5 change does not disturb
  direct-screen guests.
- `scripts/ios_regression_test.sh` on a Release build: 12/12 PASS.
