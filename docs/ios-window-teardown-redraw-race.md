# X-Plore exit kills the emulator: window teardown races the redraw walker

## Symptom

On the Nokia 5320 (`rm-409`, S60 3rd FP2), opening X-Plore, choosing **Exit** and
confirming **Yes** terminates the whole iOS app. The crash is a `SIGSEGV`
(`KERN_INVALID_ADDRESS at 0x0`) on the **Timing thread**, never on the guest CPU
thread:

```
eka2l1::epoc::redraw_msg_canvas::draw(...)          winuser.cpp:1248
eka2l1::epoc::window_drawer_walker::do_it(...)      screen.cpp:53
eka2l1::epoc::walk_tree_back_to_front(...)          winbase.cpp:218  (x4)
eka2l1::epoc::screen::redraw(...)                   screen.cpp:196
eka2l1::epoc::animation_scheduler::invoke_due_animation(...)
eka2l1::ntimer::advance()                           timing.cpp:165
```

The faulting line is the segment loop in `redraw_msg_canvas::draw`:

```cpp
for (std::size_t i = 0; i < segments.size(); i++) {
    if (segments[i]->type_ != gdi_store_command_segment_pending_redraw) {   // <-- segments[i] == nullptr
```

## What the crash is *not*

The instinctive reading — "the window was freed, so `this` points at reclaimed
memory" — is wrong and cost time. Breaking on the fault and inspecting the object
shows it fully intact: valid vtable pointer, `id = 10`, `type = client`, live
`scr` and `parent`. In one run the fault was on `scr->flags_` instead (line 1268),
which reinforced the free-memory theory; both are the same underlying problem seen
at different instants.

The real state at the fault is small and specific:

```
(lldb) p this->redraw_segments_.segments_.size()   -> 1
(lldb) p this->redraw_segments_.segments_          -> { [0] = nullptr }
(lldb) p this->redraw_segments_.current_segment_   -> 0x12d8961b0
```

A vector of one element that is a null `unique_ptr`, with a non-null
`current_segment_`. Nothing in `gdi_store_command_collection` can produce that:
`add_new_segment` always pushes a `make_unique`, and neither
`promote_last_segment` nor `clean_old_nonredraw_segments` leaves a hole. That
state only exists *transiently*, inside `~vector`, between the moment
`unique_ptr::reset` nulls the pointer and the moment the element is destroyed.

## The other thread

`thread backtrace all` at the fault answers it immediately. The guest CPU thread
was in the middle of destroying that exact object (`this` matches on both
threads):

```
Symbian OS thread
  ...
  gdi_store_command_segment::~gdi_store_command_segment
  vector<unique_ptr<gdi_store_command_segment>>::clear
  gdi_store_command_collection::~gdi_store_command_collection
  eka2l1::epoc::redraw_msg_canvas::~redraw_msg_canvas          <-- same `this`
  vector<unique_ptr<window_client_obj>>::clear
  eka2l1::epoc::window_server_client::~window_server_client    window.cpp:119
  eka2l1::window_server::disconnect                            window.cpp:2179
  eka2l1::service::session::destroy
  eka2l1::kernel::thread::do_cleanup
  eka2l1::kernel::thread::kill(terminate, u"KERN-EXEC", 3)
  eka2l1::kernel_system::cpu_exception_handler
  ARMul_State::ReadMemory32Slow
  InterpreterMainLoop
```

So X-Plore panics with `KERN-EXEC 3` on exit, the kernel tears the thread down,
the window-server session is closed, and the client's windows are destroyed —
while the animation scheduler is walking the very same window tree on the timing
thread.

## Why nothing prevented it

The two sides are not mutually exclusive:

* `animation_scheduler::invoke_due_animation` takes `kern->lock()` **and**
  `scr->screen_mutex` around `screen::redraw`.
* The guest CPU thread runs `cpu->run()` in `system_impl::loop` without holding
  the kernel lock, and the window-server IPC/teardown path takes neither lock.

`~window_server_client` did lock every screen, but only when
`kernel_system::wipeout_in_progress()` — that branch was added for emulator
shutdown. An ordinary client disconnect, which is what a guest panic produces,
skipped it entirely.

## Fix

Hold the screens' redraw lock for the whole of the client teardown, not just
during wipeout, and for the same reason around `window_server_client::delete_object`
— a guest that explicitly frees a window through IPC unlinks it from the same
tree and has always had the same race, just with a much narrower window.

Only `screen_mutex` is taken, never `kern->lock()`. The redraw path acquires the
kernel lock first and the screen mutex second, so a teardown that waits on the
screen mutex alone cannot invert the order.

The wipeout branch is left byte-identical: during shutdown the timing thread may
already be on its way out, so the scheduled redraws are cancelled first and the
screens are only taken by the client that performed the cancel.

## Side finding: an empty central-repository persist aborts the boot

Every crash of the emulator can leave a zero-byte `.cre` in
`c:\private\10202be9\persists\...`. `central_repo_server::load_repo_adv` did
`buf.resize(file->size())` and then `&buf[0]`, which under libc++ hardening
aborts the process on the next boot — a crash that outlives the crash that
caused it. Empty persists are now skipped so the ROM/TXT default is used.

## Still open: the guest's own `KERN-EXEC 3`

With the host crash fixed the emulator survives and shows the guest-error dialog
instead, so X-Plore still does not exit cleanly. That is a separate defect, and
the chain is now identified:

`euser.dll` ordinal 613 is `User::HandleException(TAny*)`, whose ROM code matches
`kernel/eka/euser/epoc/up_utl.cpp` exactly (`r7 == KCurrentThreadHandle ==
0xffff8001` at the fault). The guest stack shows the caller is
`User::RaiseException(TExcType)+0x28` with `aType == EExcGeneral`, reached from
`drtaeabi.dll` — the RVCT runtime's terminate path — under `cntmodel.dll` frames
ending in `RSessionBase::CreateSession` and `User::LeaveIfError`.

X-Plore installs `User::SetExceptionHandler(ExceptionHandler, 0xffffffff)` and its
handler deliberately tail-calls `User::HandleException(NULL)` to hand the
exception back to the kernel (see `LibSrc/Symbian/SymbianAppStart.cpp` in the
published X-Plore source); the null dereference there is the *intended* outcome,
not the bug — that is how X-Plore asks the kernel to kill it. The bug is the
*first* exception.

### Why the first exception happens

`RCntModel::ConnectL()` does the standard "assume it runs, else start it" dance:

```cpp
TInt err = CreateSession(KCntServerName, Version(), KAsyncMessageSlots);
if (err == KErrNotFound) {
    server.Create(KCntServerExe, KNullDesC);
    server.Rendezvous(reqStatus); server.Resume();
    User::WaitForRequest(reqStatus);
    User::LeaveIfError(reqStatus.Int());
    User::LeaveIfError(CreateSession(KCntServerName, Version(), KAsyncMessageSlots));
}
```

Breaking on the `!server` branch of `session_create` shows `"CNTSRV"` failing
**twice** — so the rendezvous returned `KErrNone` and the second create still
found no server. Tracing `thread::kill` explains that: a thread named `"Main"`
dies with reason 0 and category "None" right after `CNTSRV.EXE` is summoned. The
ROM contacts server starts and returns from `E32Main` immediately, the same
reason-0 shape as the 6680 `sdancer` early exit. `process::logon` then sees
`thread_count == 0` and completes the client's Rendezvous with `exit_reason`,
which is 0 — indistinguishable from a successful start. The client proceeds,
`CreateSession` fails, and `User::LeaveIfError` leaves with no TRAP in scope.

### A stub CNTSRV does not help

Registering an HLE `service::typical_server` named `"CNTSRV"` was tried and
reverted. It does get the client past `ConnectL` (the session connects and the
ROM server is never started), and the guest then sends opcodes 21, 100, 44, 22 —
100 is `ECntOpenDataBase`, anchored at `KCapabilityReadUserData` in
`cntsrv/inc/ccntipccodes.h`. Neither answer works:

* Completing everything with `KErrNone` makes the client internalise a packaged
  reply that was never written; the guest stack then ends in `estor.dll` →
  `User::LeaveNoMemory`, same terminate.
* Completing `ECntOpenDataBase` with `KErrNotFound` makes
  `CContactDatabase::OpenL` leave through `User::LeaveIfError` — again with no
  TRAP above it.

The caller genuinely requires a working contacts database, so anything short of
implementing the real CNTSRV protocol (including the `CCntPackager` stream
formats, which are only published for the 9.5+/qtmobility contacts model, not for
this ROM's 9.3 one) just moves the leave. Defining the missing
`KUidBackupRestoreKey` system property — the last thing `cntsrv.exe` touches
before exiting — was also tried and does not change its early exit.

The tractable next step is therefore not a contacts HLE but finding why the ROM's
own `cntsrv.exe` returns from `E32Main`; that would give the real server, its
database and its packager for free.
