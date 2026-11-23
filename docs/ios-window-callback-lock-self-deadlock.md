# N-Gage Launcher exit deadlocks in the window focus callback

## Symptom

On the 5320 device, open `Games` (the N-Gage Launcher), wait for its Home
screen, and press the right soft key (`Exit`). The guest stops drawing at 0
FPS. Opening the native game menu and selecting **Exit Game** then freezes the
whole iOS application. TestFlight build 260806 is eventually killed by the
scene-update watchdog (`0x8BADF00D`) with 0% application CPU.

The first frozen screen is guest behaviour. The second freeze is a host
deadlock in EKA2L1's forced process teardown.

## What the crash actually shows

The report's EKA2L1 image UUID is
`2AF15155-B049-3428-B442-C79B86D2D21A`. Symbolicating it with the exact dSYM
from the build's TestFlight workflow puts the main thread at:

```text
std::mutex::lock
screen::fire_focus_change_callbacks
screen::update_focus
window_group::~window_group
window_server_client::~window_server_client
window_server::disconnect
service::session::destroy
kernel_system::destroy
thread::kill
process::kill
-[EKA2L1Emulator closeRunningApp]
```

The Timing thread is waiting in the animation scheduler. It is a consequence,
not the owner of the deadlock: once teardown stalls, the redraw path also stops
making progress.

## Root cause

The earlier fix for the window teardown/redraw race correctly made
`window_server_client::~window_server_client()` lock every screen's
`screen_mutex` across `objects.clear()`. That prevents the Timing thread from
walking a window while its draw-command containers are being destroyed.

Clearing a focused `window_group` has a synchronous side effect:

```text
objects.clear()
  -> window_group::~window_group()
  -> screen::update_focus()
  -> screen::fire_focus_change_callbacks()
```

`fire_focus_change_callbacks()` also used `screen_mutex`. Because
`std::mutex` is non-recursive, the teardown thread tried to acquire the lock it
already owned and blocked forever. Releasing the redraw lock around the
callback would reopen the half-destroyed-window race, while converting
`screen_mutex` to a recursive mutex would hide accidental redraw-lock
reentrancy throughout shared window-server code.

The same mutex was also being used to protect callback registration even
though callback containers and the window tree do not share an invariant.
Screen-redraw and screen-mode callback dispatch did not consistently take that
mutex at all, so registration and dispatch were not uniformly synchronized.

## Fix

Keep `screen_mutex` exclusively for the window tree, draw data, and screen
state. Give each independent callback registry its own mutex:

- focus-change callbacks;
- screen-redraw callbacks;
- screen-mode-change callbacks.

The per-kind split is intentional. A focus callback in the SGC server can call
`set_screen_mode()`, which synchronously fires screen-mode callbacks. One
ordinary mutex shared by all callback types would create another same-thread
reentrant deadlock. Separate locks match the independent containers and allow
that existing focus-to-mode transition.

This preserves the full teardown/redraw exclusion that fixed the original
SIGSEGV, while removing the unrelated lock acquisition from the focus callback
path. It is a shared window-server fix and contains no N-Gage-specific
condition.

## Verification

With a Debug simulator build on the Nokia 5320d-1 (05.01):

1. The N-Gage Launcher reached its Home page after the normal startup delay.
2. RSK left the guest at the same 0 FPS screen as the report.
3. **Game menu → Exit Game** returned to the iOS application list in about
   three seconds.
4. The UI remained interactive and the emulator log contained no guest panic,
   access violation, graphics halt, or host crash on the exit path.

The same 5320 exit path also passed with the Release build, returning to the
application list within five seconds. The required Release regression then
passed twice (once with a clean install and once without reinstalling), 12/12
each time. It covered Final Battle's 90-second in-game dwell and teardown,
Calculator focus/input/menu transitions, N95 Calculator boot/rendering, and the
iOS string catalog check.
