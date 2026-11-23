# Four N-Gage titles on the N70: a missing EKA1 exec block and a framebuffer that was never 16-bit

## Symptoms

The four EPOC6/EKA1 games fixed for the N-Gage ROM (`nem-4`) behaved differently on the
N70 (`rm-84`, `epoc81a`), which is the same EKA1 kernel one minor release later:

- Jungle King quit during startup and dropped straight back to the application list.
- Cybersaurus reached its menu and then died with `KERN-EXEC 3`.
- Bowling Master and Killer Virus ran, but MotoGear — a fifth title on the same engine —
  drew every frame as fine red/green/blue noise with the artwork faintly visible through it.

## Two independent causes

### The 8.1a executive table is missing the RThread IPC descriptor calls

Jungle King's log carried `Unimplement system call: 0x800040` and `0x800043` right before
it gave up. EKA2L1 keeps one `func_map` per EKA1 revision, and comparing them side by side
shows the numbering never shifts: every opcode the `v6`, `v80` and `v81a` tables have in
common maps to the same call. The tables differ only in coverage, because each was filled
in as titles needed it.

`0x800040`–`0x800045` are the pre-EKA2 server-side descriptor block —
`RThread::GetDesLength`, `GetDesMaxLength`, `ReadL` and `WriteL` in their 8- and 16-bit
forms. Any guest that implements its own server needs them. The EPOC6 table has all six,
the 8.0a table has four, and the 8.1a table had none, so a game that worked on the N-Gage
failed on the N70 for no reason other than a gap in the table. The same comparison also
found `0x21` (`process_exit_reason`), `0x8000A2` and `0x8000B7` missing from 8.1a while
present in 8.0a; those are added too.

With the block restored, Jungle King boots to its menu and plays.

### WINDOWMODE is what WSERV composes in, not what the panel holds

Cybersaurus faulted writing at `0x10881000`, just past the committed part of
`FbsLargeChunk`. Instrumenting `fbs_server::create_bitmap` showed the game had just
created a 176x208 `EColor64K` bitmap — 73216 bytes — and the faulting code at
`cybersaurus.app+0x1300` is a loop reading 16-bit pixels and storing them with
`str r3, [r5, ip, lsl #2]`: four bytes per pixel, twice the space the bitmap has.

The caller picks between two blitters:

```
mov  r0, #35              ; HALData::EDisplayColors
bl   HAL::Get
cmp  r3, #0x10000
movgt r8, #3              ; 32-bit destination
cmp  r3, #0x1000
movgt r8, #2              ; 16-bit RGB565
```

So the game asks the ROM's own `hal.dll` how many colours the display has and sizes its
pixels from the answer, but takes the bitmap's display mode from the window server. On the
N70 those disagreed, because `window.cpp` clamped any EKA1 `WINDOWMODE` deeper than 16
bits down to `EColor64K` while `hal.dll` — which never reaches
`do_hal_by_data_num` for this attribute — kept reporting the hardware's real colour count.
MotoGear has the same split and produced the noise instead of a crash.

Removing the clamp fixes both, and also fixes Sky Force Reloaded on the 6680 — but it
regresses 黄泉道, which had been the reason the clamp survived (see
[EKA1 direct screen access framebuffer depth](./eka1-dsa-framebuffer-depth.md)).

## What the earlier investigation got wrong

That document proposed recording the `TDisplayMode` the guest passes to scdv's
`CFbsDrawDevice::NewScreenDeviceL`, on the premise that EKA2L1 does not implement that
export. It does: `scdv_v81a.dll` is installed as a patch DLL and its `RDebug` output shows
up in the log once the filter is at trace level. Driving 黄泉道, Sky Force and Sky Force
Reloaded past that call shows all three instantiate whatever mode the window server
reported — 16-bit under the clamp, 24-bit unsigned byte without it. The draw device is a
mirror of the report, not an independent statement, so it cannot separate the titles.

Dumping the direct screen access chunk under both policies settles what does. 黄泉道 fills
only the first `w * h * 2` bytes and leaves the rest at the `0xFF` the chunk is handed over
with; rendering that half as RGB565 reproduces its screen exactly, while the 32-bit reading
is noise. Sky Force, Sky Force Reloaded, Dragon.World and MotoGear write the whole buffer.

## Fix

`epoc::screen` now carries two display modes. `disp_mode` is what the ROM declares and what
every guest-visible surface uses: window and bitmap creation, HAL, the reported framebuffer
stride. `dsa_disp_mode` is the format EKA2L1 reads the direct screen access buffer back in,
and it is measured rather than declared. It starts at the conservative 16 bits for EKA1
devices whose declared mode is deeper, and
`screen::promote_dsa_depth_if_deep_pixels_written()` widens it to the declared mode the
first frame anything lands past the halfway mark. The buffer is refilled with its untouched
marker when a new client takes direct screen access, so each one is judged on its own
frames, and the write-back in `sync_screen_buffer_data()` uses the same measured depth so it
cannot forge evidence.

## Result

On the N70: Bowling Master, Jungle King, Killer Virus and Cybersaurus all reach gameplay,
and MotoGear renders correctly. On the 6680: Sky Force Reloaded is fixed, and 黄泉道,
Sky Force, Tetris3D and Dragon.World are unchanged (黄泉道 pixel-identical to the previous
build). iOS standard regression 12/12, angrybirds 5/5, native suite 185/185.
