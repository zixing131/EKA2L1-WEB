# TestFlight crash triage, round 7 (build 260784)

Two unrelated `EXC_BAD_ACCESS` reports came out of the same TestFlight build, four
minutes apart on the same iPhone XR. Both are null/dangling pointer faults that the
code handed to somebody else's library — one to our own `system` object, one to
libc's `FILE`.

## 1. `currentDeviceIsTouchScreen` reads a `system` that a reboot is freeing

```
Thread 0 Crashed:
0  eka2l1::system::get_symbian_version_use() const + 12
1  -[EKA2L1Emulator currentDeviceIsTouchScreen] + 28   (IosEmulator.mm:2081)
2  closure #2 in EmulatorView.body.getter + 376        (EmulatorView.swift:195)
...
Thread 18:
...
6  eka2l1::epoc::window_group::~window_group()
8  eka2l1::epoc::window_server_client::~window_server_client()
13 eka2l1::kernel_system::wipeout()
15 eka2l1::system::~system()
19 -[EKA2L1Emulator bootDeviceAtIndex:] + 316          (IosEmulator.mm:1372)
20 -[EKA2L1Emulator runLaunchAppWithUID:] + 300
```

The two stacks say it outright. `KERN_INVALID_ADDRESS at 0x882b8` is not a small
offset off null, it is a read through freed memory: `get_symbian_version_use()`
dereferences `impl` (and then `kern_`) of a `system` whose destructor is running on
the control queue.

The sequence is the ordinary second-launch flow. `runLaunchAppWithUID:` reboots the
device before relaunching (`needs_reboot_before_launch`), so `bootDeviceAtIndex:`
replaces `_state->symsys` while `EmulatorView`'s `onAppear` is running on the main
thread. `currentDeviceIsTouchScreen` loaded the old pointer, passed its null check,
and the unique_ptr assignment freed the object before the call landed.

The bridge already has a rule for this, spelled out on `emulator::session_mutex`:
entry points that walk symsys internals from another thread must take that lock, and
main-thread readers must `try_lock` only (a boot holds the lock while the graphics
thread `dispatch_sync`s onto the main queue, so blocking main would deadlock the
boot). `guestFrameLimitForAppUID:` and `guestScreenModeSnapshot` follow it;
`currentDeviceIsTouchScreen` never did.

`try_lock` is a poor fit here anyway — a failed lock has no sensible fallback, and
answering "no touch screen" for an X7 would leave the wrong input model on screen
for the whole session. The value is a property of the booted device, not of live
kernel state, so it is now cached: `emulator::device_is_touch_screen` is published by
`bootDeviceAtIndex:` right after `set_device()` succeeds, and the main thread only
reads that atomic. Publishing after (not before) the device is known good means a
failed boot keeps the last good answer instead of flipping the screen.

While in there, `iconPNGDataForUID:` had the same exposure: the frontend decodes
icons on a background queue, and the decode walks the applist/fbs servers of the
booted system with no session lock at all — the home list stays alive under the
pushed emulator view, so a re-render during the launch reboot could fault the same
way. It is off the main thread, so it takes the session lock the normal (blocking)
way.

## 2. `physical_file::resize` faults on a `FILE` its reopen never produced

```
Thread 4 Crashed:
0  flockfile + 28
1  fseek + 76
2  eka2l1::physical_file::resize(unsigned long) + 320
3  eka2l1::fs_server_client::file_set_size(...)
```

`KERN_INVALID_ADDRESS at 0x68` is `flockfile` reading the lock member of a null
`FILE*`. `resize()` closes the host file so `truncate()` can work, reopens it, and
then seeks back to the saved position — without ever checking that the reopen
returned anything:

```cpp
file = fopen(common::ucs2_to_utf8(physical_path).c_str(), translate_mode(fmode, true));
fseek(file, static_cast<long>(saved_pos), SEEK_SET);   // <- null here
```

Note that `file_set_size` calls `f->size()` first, which would have faulted in
`ftell` had the handle been null on entry; so this file was open, and it was
specifically the *reopen* that failed. Which host reason (descriptor exhaustion —
iOS ships a low `RLIMIT_NOFILE` — the file going away underneath, a permission
change) cannot be recovered from the report, and it does not matter: `fopen` is
allowed to fail, and a guest `RFile::SetSize()` must not be able to kill the
emulator.

The same hole exists one level up. `physical_file_system::open_file` returns a
`physical_file` even when its `fopen` failed, so `fs_server_client::new_node`
happily hands the guest a handle with no host file behind it; the first read/write
on it faults inside libc exactly like the above. And a failed open left `fmode`
uninitialised, so even the `WRITE_MODE` guards in `resize`/`flush` were reading
garbage.

Fixes, all in the same object:

- `init()` fills `input_name`/`physical_path`/`fmode` before opening, so a failed
  open still leaves a well-defined object.
- `open_file` rejects a `physical_file` whose open failed (`is_open()`), so the
  guest gets `KErrNotFound` from `new_node` instead of a poisoned handle.
- Every operation that touches `file` bails out through `NO_HANDLE_RETURN` instead
  of passing null to the C library, and `close()` nulls the handle rather than
  leaving it dangling (the pre-existing "closed but operation still continues"
  warning implies that does happen — reusing a `fclose`d `FILE` can silently hit a
  recycled stream).
- `resize()` reports the failed reopen and stays handle-less rather than seeking
  through null.

Two adjacent null derefs that the stricter `open_file` makes reachable are guarded
at the same time: `rom_file_system::open_file`'s `ff->size()` in the
`PREFER_PHYSICAL` comparison, and `fs_server_client::file_rename`'s reopen of the
renamed path.

## Verification

`scripts/ios_regression_test.sh` (Release build) — Final Battle, Calculator, N95
Calculator. The FS changes sit on the path every app uses to open files, which is
what that suite exercises end to end.
