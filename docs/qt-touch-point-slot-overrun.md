# A trackpad touch killed the Qt frontend

## Symptom

Two macOS crash reports, seconds apart, from the same 0.0.4.2 release running on a
MacBook. Both are `EXC_BAD_ACCESS` on the main thread with an identical stack:

```
AppKit    _routeTouchEvent
QtGui     QWindowSystemInterface::handleTouchEvent
QtWidgets QApplicationPrivate::translateRawTouchEvent
EKA2L1    display_widget::event(QEvent*)  + 1564
QtGui     QEventPoint::id()                        <- fault
```

The same emulator build ran for an hour on another Mac without a hiccup.

## Narrowing it down

macOS annotated both faults as "possible pointer authentication failure", which
suggests a corrupted return address or a stomped vtable. It is worth ignoring here.
The faulting instruction from `instructionByteStream` is `LDR W0, [X8, #0xD8]`, and
the preceding bytes are `LDR X8, [X0]` / `CBZ X8` — the classic
`QExplicitlySharedDataPointer` dereference at the top of `QEventPoint::id()`. So `X0`,
the `QEventPoint` this-pointer, was already wrong before any authentication happened.

The two faulting addresses settle it: `0xffff97178dfa22cf` and `0x425355206e69782c`.
Read the second one as little-endian bytes and it spells `,xin USB`. That is not a
mangled pointer, it is a string. The code was reading ordinary heap data as a
`QEventPoint`.

`display_widget::event`'s `TouchBegin` branch bounded its loop by `active_pointers_`
(8 pointer slots) but indexed the event's point list with the same counter:

```cpp
for (std::size_t i = 0; i < active_pointers_.size(); i++) {
    if (active_pointers_[i] == 0) {
        active_pointers_[i] = points[i].id() + 1;
```

A trackpad `TouchBegin` carries one or two points and all eight slots are free at that
moment, so iteration 1 is already past the end of the `QList`. Whether it faults or
silently returns a garbage id depends on what the allocator left behind the list.

That also explains the machine that never crashed: macOS only delivers touch events
for trackpad contacts (the widget sets `WA_AcceptTouchEvents`). Drive the emulator
with a mouse and the branch never runs.

## Verification without a trackpad

`displaywidget.cpp` has almost no dependencies outside Qt — `emu_window` is a
header-only interface — so it links standalone. A harness compiled from the file plus
its moc output constructs `QTouchEvent`s by hand and calls `display_widget::event`
directly, recording the callbacks the widget emits. It reproduces the crash on the
first single-point `TouchBegin` and doubles as the regression check, no hardware
gesture needed.

The harness also exposed two more defects in the same handler that no crash report
would have shown: `TouchUpdate`'s "find a free slot" loop never broke out, so one new
point claimed all seven remaining slots and reported itself seven times; and its
release loop did not skip empty slots, so every update released the six slots that
held nothing. `TouchEnd` had the guard all along.

## Fix

`TouchBegin` walks the points and takes a free slot for each, matching `TouchUpdate`.
The slot search in `TouchUpdate` breaks once it claims one, and the release loop skips
slots holding no pointer. Upstream PR
[#692](https://github.com/EKA2L1/EKA2L1/pull/692); the bug dates back to the original
multitouch support in `f6bb6b7` (2022) and affects every platform that delivers real
touch events.
