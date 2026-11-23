# N-Gage on iOS: installer "Installation complete" popup + game launch black screen

Repro asset: `roms/ONE_fixed_by_BodyZ.n-gage` copied to
`Documents/data/drives/e/n-gage/`, then launch the N-Gage launcher
("Games", UID `0x20003B78`). The launcher auto-installs the card on first run.

## 1. Missing "Installation complete" popup — FIXED

### Symptom
On Android the install finishes with an "Installation complete" info popup
above the `Start Game` / `Cancel` soft-key bar. On iOS the popup never
rendered — only the soft-key bar showed.

### Root cause
The N-Gage installer draws its popup as an AVKON info note in a `redraw`
window that sits above the launcher content. On iOS these notes finished their
`EndRedraw` but never appeared: the window server only composited them through
the incremental *client* redraw path, so their content could be overdrawn by a
later update of a window *behind* them, or skipped entirely if a full server
composite pass had already run before the note's content landed. The note's
pixels were produced but never made it into the correct z-order on screen.

### Fix
`redraw_msg_canvas::end_redraw` now requests a full server recomposite
(`FLAG_SERVER_REDRAW_PENDING`) whenever a redraw completes with changed
content, so newly-finished redraw windows are always composited back-to-front
in the right order. (`src/emu/services/src/window/classes/winuser.cpp`)

A prerequisite Debug-build crash was also fixed: `kernel::mutex::wait` had an
`assert(!holding->wait_obj)` that aborts when a thread waits on a mutex whose
holder is itself blocked on another wait object. That is a legitimate Symbian
state (a mutex holder may block on e.g. a request semaphore while still owning
the mutex), and the code below the assert already queues the waiter correctly.
Release builds (NDEBUG) already stripped the assert; Debug now matches. Hit on
the standalone "N-Gage Installer" app path. (`src/emu/kernel/src/mutex.cpp`)

### Verification
Verified end-to-end on **both** backends (iPhone 16 Pro sim): reset guest
state, recopy the `.n-gage`, launch Games — the "Installation complete" popup
renders on dyncom and on dynarmic (JIT). `ios_regression_test.sh` 8/8.

## 2. "Start Game" black screen — root-caused, NOT fixed

### Symptom
Tapping `Start Game` (or launching the installed game directly) shows a black
screen; the guest renders ~2 frames then freezes permanently. Input does not
wake it.

### Findings
The hang is **independent of the CPU backend** — identical on dyncom and on
dynarmic. A scheduler idle-state dump at the freeze shows:

```
thr='ngiplay0x20003b78' state=wait reqcnt=8 susp=true sleeplvl=1 waitobj='-'
  ...all servers (AknIconServer/ecomserver/PlayServer/NAF*/wmdrmpkserver) idle
     in wait_fast_sema on their request semaphores (normal)...
```

The N-Gage launcher UI thread (`ngiplay`) is **suspended** (`susp=true`) while
it was mid-`User::After` sleep (`sleeplvl=1`), holding **8 completed-but-
unconsumed requests** (`reqcnt=8`), and is never resumed. This is a guest-side
deadlock in the PlayServer game-launch orchestration (launcher suspends its UI
to hand over to the game / splash sequence, and the resume never arrives). The
game process either never fully starts or exits without the handshake that
would resume the launcher.

The title is a DRM-cracked N-Gage 2.0 game ("ONE, fixed by BodyZ"); its startup
runs the activation server `AS_2000AFBF` and repeatedly loads the OMA/WMDRM
agents. A clean N-Gage game (Snakes, `0x2000730F`) launches and plays fine on
iOS, so the general N-Gage path is healthy — this is specific to ONE's
DRM/anti-tamper launch handshake.

### Separately: dynarmic JIT crash on this title
With JIT enabled, dynarmic crashes while compiling ONE's code — a host-stack
overflow in the IR optimizer (`ConstantPropagation` → `FoldShifts` →
`Value::IsImmediate` recursing through a long `Identity` chain). Disabling the
`ConstProp` optimization avoids the crash and lets the game launch on dynarmic,
but it then hits the same launcher suspend/resume deadlock as dyncom, so it is
not a real fix and was not kept (it would also globally cost JIT perf).

### Status
Not fixed. A real fix needs deep, risky changes to guest suspend/resume/IPC
orchestration (broad impact on all apps) or a game-specific hack — both against
project guidelines — and even then the DRM handshake may block further. Left as
a known deep issue; the clean N-Gage path (Snakes) is unaffected.
