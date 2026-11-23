# `destroy()` erased the object next to the one it was given

## Symptom

Two TestFlight `EXC_BAD_ACCESS (SIGSEGV)` reports from an iPhone 17 Pro (`iPhone18,4`,
iOS 26.5.2), from two different builds, with nothing obviously in common:

```
# 26.7.0 (260861), on a device switch
Exception Subtype: KERN_INVALID_ADDRESS at 0xda808c09b7addffa
0  kernel::codeseg::destroy() + 52
1  kernel_system::wipeout() + 1632
2  system_impl::~system_impl()
3  -[EKA2L1Emulator bootDeviceAtIndex:]

# 26.7.0 (260863), a minute into a session
Exception Subtype: KERN_INVALID_ADDRESS at 0x14cb40000
0  _platform_memmove + 420
1  XXH_INLINE_XXH64_update(...)
2  epoc::bitmap_cache::hash_bitwise_bitmap(epoc::bitwise_bitmap*) + 288
3  epoc::bitmap_cache::add_or_get(...)
4  epoc::canvas_base::add_draw_command(...)
5  epoc::graphic_context::gdi_blt_masked(...)
```

## Narrowing down the first report

`codeseg::destroy()` is four lines long, so the `+ 52` can be placed exactly. Disassembling
the same function out of a local Release build (the CI dSYM carries symbols but no line
tables for the C++ core, so `atos` only ever answers `+ 52`):

```
3b8  stp x20, x19, [sp, #-0x20]!
...
3d8  ldp x20, x19, [x19, #0xb0]   ; dependencies.begin / .end
3e4  ldr x0, [x20], #0x20         ; dep.dep_
3e8  ldr x8, [x0]                 ; vptr
3ec  ldr x8, [x8, #0x30]          ; <- +52, decrease_access_count slot
3f0  blr x8
```

The registers agree: `x19 - x20 = 0xE0`, seven remaining `codeseg_dependency_info` entries of
32 bytes each. `x0` (the dependency) was readable, but the vtable pointer read out of it was
`0xda808c09b7addfca` — a scrambled libmalloc free-list word. So a codeseg held a *counted*
reference (`add_dependency()` does `increase_access_count()`) on a codeseg that had already
been freed and its memory recycled.

That reference is only released in `codeseg::destroy()`, and `decrease_access_count()` is a
no-op once `wipeout_in_progress()`, so the dependency could not have died during the wipeout
itself. Something had freed a live codeseg earlier, during the session.

## The bug

`kernel_system::destroy()` looked its object up with `std::lower_bound` and then used that
iterator twice:

```cpp
auto res = std::lower_bound(obj_map.begin(), obj_map.end(), obj, ...uid ordering...);
if (res == obj_map.end())
    return false;
(*res)->destroy();
obj_map.erase(res);
```

Two independent defects live in those four lines.

**The iterator does not survive `destroy()`.** `codeseg::destroy()` drops the references it
holds on its dependencies; a dependency that reaches zero is destroyed through this very
function, which erases an element from this very vector. Every element after the erased one
shifts down, so `res` now addresses the *next* codeseg — a live one. `obj_map.erase(res)`
then destroys and deletes it, leaving every codeseg that depends on it with a dangling
`dep_`, while the codeseg we were actually asked to destroy stays in the container. Nothing
looks wrong at that moment; the bill arrives at the next `wipeout()`, which walks the
dependency lists.

**`lower_bound` does not mean "found".** It returns the first object whose uid is greater or
equal, so an object that is not in the container at all resolves to an unrelated live
neighbour, which is then destroyed and erased in its place. `kernel_system::get_by_id()` and
`normal_object_container::remove()` both check identity after the search; this one never did.
`thread::destroy()` alone calls `kern->destroy()` on four chunks, so a second destroy of an
already-freed chunk is not exotic — and it silently takes out whichever chunk sorts next.

The fix keeps the object in the container while its `destroy()` runs (some teardown paths
still expect to find themselves), verifies identity before touching anything, and re-locates
the slot afterwards instead of trusting the stale iterator.

## The second report

`hash_bitwise_bitmap()` faulted on the very first eight bytes of the pixel data, at an
address that the report places in a 44 MB hole with no VM region at all — not a decommitted
page (those keep their `PROT_NONE` mapping), memory that is not mapped. The length it was
about to hash, recovered from `x21 - x20`, was `0xDFFFA`: not a multiple of four, which a
real bitmap's `byte_width_ * height` always is. The header driving the read was garbage.

`bitwise_bitmap` headers live in the FBS shared chunk, which is mapped into every client
process, so `bitmap_size` / `header_len` / `data_offset_` are all guest-writable; a wrongly
destroyed chunk (see above) has the same effect from the other side. Either way the window
server followed the resulting pointer into memory the emulator does not own.

`readable_bytes_from()` was added for exactly this crash class in an earlier round, but it
*failed open*: a pointer inside neither bitmap chunk returned the caller's own unbounded
length. All bitmap pixels come out of one of those two chunks — even a bitmap read from a ROM
MBM is decompressed into them — so a pointer in neither is not a pointer the host may read.
It now returns zero, and `bitmap_cache::add_or_get()` drops such a bitmap up front rather
than hashing, RLE-decoding and `memcpy`ing through it. The draw is skipped; the graphics
driver already tolerates a null bitmap handle.

`normal_object_container::get<T>()` had the same fail-open shape as the kernel lookup — no
identity check after `lower_bound`, so a closed bitmap handle (or the zero handle a command
uses to mean "no mask") returned an unrelated live object, `reinterpret_cast` to the expected
type. Callers only ever test for null, so nothing downstream would have noticed. Fixed the
same way.

## Dead ends worth avoiding

- The CI `EKA2L1-testflight-dSYM-<sha>` artifacts have exact UUID matches but no line
  numbers for the C++ core. Placing a frame inside a small function means disassembling the
  same function from a local Release build and matching the register file, not `atos`.
- Report 260861 already had a fix landed against it (`flexible-chunk-attach-lifetime.md`).
  A build can carry several unrelated crashes; matching the UUID only tells you which
  commit, not which bug.
- The wipeout ordering is not at fault here. Every teardown path is already guarded by
  `wipeout_in_progress()`, and the containers are cleared after their `destroy()` loop, so no
  codeseg can die halfway through it. The damage was done long before.
