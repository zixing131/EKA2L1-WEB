# Single-member intrusive ring misdetected as unlinked

## Symptom and symbolication

TestFlight build 260766 (commit `c5072d166`) crashed after about 13 minutes in
the foreground with `EXC_BAD_ACCESS` while writing address `0x8` on the
Symbian OS thread. The crash image and downloaded dSYM have the same UUID,
`9AB49483-FE74-31F0-93F7-02D5FF0A2479`.

The symbolicated stack is:

```text
codedump_collector::add(codeseg*) + 260
codeseg::free_attached_data(attached_info&) + 872
codeseg::detach(process*, bool) + 124
process::kill(...)
thread::kill(...)
thread_kill(...)
```

At `add + 260`, the compiler had inlined `roundabout::push()`. The faulting
instruction stores the new node through the ring sentinel's previous pointer;
that pointer was null, hence the write to `0x8`.

## Root cause

An intrusive node is unlinked when both of its pointers are null. The helper
instead implemented `alone()` as:

```cpp
return next == previous;
```

That is true for an unlinked node, but it is also true for the only member of
a circular list: both links point to the ring sentinel. Callers consistently
use `alone()` as a membership test. In particular, a codedump node that was
the collector's sole member was incorrectly treated as absent:

- `remove(attached_info&)` returned without dequeuing it before its owner was
  erased, leaving a dangling node in the collector; or
- `add(codeseg*)` accepted the already-linked sole node a second time,
  self-linking it and detaching it from the sentinel's back-links.

Later cleanup followed or dequeued the corrupted node and left a null/dangling
sentinel link. The next `push()` produced the observed null write. This also
explains why earlier fixes for collector teardown and erase-before-remove did
not close the crash: their membership guards themselves were wrong for the
single-node case.

The same helper guards semaphore and mutex intrusive links. With one suspended
or pending thread, those paths could likewise report that the thread was not
queued, although they did not cause this host crash.

## Fix

`alone()` now means exactly "unlinked": both `next` and `previous` are null.
A unit test covers the important transition—new node is alone, the sole member
of a roundabout is not alone, and dequeuing makes it alone again. This keeps
the fix in the shared list contract rather than adding another codedump-only
workaround.

The Release simulator build succeeded, and the iOS regression suite passed
twice (11 checks each), including the 90-second Final Battle dwell and both
Calculator paths. The final emulator log contained no guest crash, access
violation, or graphics halt; its only `panic` matches were the existing
missing optional panic-blacklist file warning.
