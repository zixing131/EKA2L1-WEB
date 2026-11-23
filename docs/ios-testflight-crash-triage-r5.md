# TestFlight crash triage, round 5

Two crashes from adjacent TestFlight builds (260769, 260771, `26.7.0`). Both fire
during **app close / teardown**, and both are lifetime races between the guest/host
teardown path and a callback that is still in flight on another thread. They are
independent root causes in different subsystems (kernel logon completion vs. the
audio DSP stream destructor), fixed separately.

## 1. `notify_info::complete` null-deref killing a process with a dead logon requester

### Symptom

`EXC_BAD_ACCESS (SIGSEGV)` at `KERN_INVALID_ADDRESS 0x48` on the main thread while
closing a running app:

```
0  eka2l1::get_raw_pointer(kernel::process*, unsigned int) + 4
1  eka2l1::epoc::notify_info::complete(int)
2  eka2l1::kernel::process::finish_logons()
3  eka2l1::kernel::process::kill(...)
4  -[EKA2L1Emulator closeRunningApp]  (IosEmulator.mm:1711)
```

`x0 = 0`, and `get_raw_pointer` faults reading offset `0x48` off a null base.

### Narrowing

`get_raw_pointer(pr, addr)` is `pr->get_ptr_on_addr_space(addr)`, so `pr == null`.
Its only caller here is `notify_info::complete`:

```cpp
epoc::request_status *sts_real = sts.get(requester->owning_process());
```

`requester->owning_process()` returns the thread's `owner` field, which was null.
The line just above (`requester->get_kernel_object_owner()`) did **not** fault, so
`requester` still points at readable memory but its `owner` slot reads as 0 — the
classic signature of a thread object that has already been destroyed / half torn
down (a freed-then-reused thread whose owner pointer is now zero).

`process::finish_logons()` walks `logon_requests` / `rendezvous_requests` and
completes each unconditionally. Those `notify_info`s hold a **raw** `requester`
pointer to a thread living in *another* process (the one that armed the `RProcess::Logon`
/ `Rendezvous`). If that requester process/thread exits before the target process is
killed, the entry is left dangling in the queue, and completing it dereferences the
dead thread.

### Fix

This is the same dangling-requester family already guarded elsewhere in the kernel
(`property.cpp` subscription cancel/notify, `dispatch/audio.cpp`, `dispatch/camera.cpp`),
using `kernel_system::is_thread_alive()`. Applied the same guard in `finish_logons`:
signal only requesters still present in the kernel thread list, drop the rest (the
queues are cleared immediately afterward). `finish_logons` runs under `kern->lock()`
via `closeRunningApp`, so walking the thread list is safe.

## 2. `__cxa_pure_virtual` in the audio render callback during stream destruction

### Symptom

`EXC_CRASH (SIGABRT)` — `__cxa_pure_virtual` — on the CoreAudio render thread:

```
Thread 19 (crashed):
  __cxa_pure_virtual
  dsp_output_stream_shared::data_callback(short*, unsigned long)
  output_render_cb(...)                     // RemoteIO render callback
  AURemoteIO::RenderBus / PerformIO ...

Thread 4 (concurrent):
  AURemoteIO::Stop()
  audiounit_ios_output_stream::stop()
  dsp_output_stream_shared::~dsp_output_stream_shared()   // <-- base dtor
  dsp_output_stream_ffmpeg::~dsp_output_stream_ffmpeg()
  mmf_dev_server_session::~mmf_dev_server_session()       // session disconnect
```

### Narrowing

Classic "virtual call during destruction, from another thread" race:

1. The mmf session is closed (guest `RSessionBase::Close` on the emulator loop
   thread), destroying the `dsp_output_stream_ffmpeg`.
2. `~dsp_output_stream_ffmpeg` runs first and frees `codec_` / `av_format_`; the
   object's vtable is now downgraded to the **abstract** `dsp_output_stream_shared`.
3. Only *then* does `~dsp_output_stream_shared` call `stream_->stop()`, which enters
   `AURemoteIO::Stop()` and blocks waiting for the render thread to finish.
4. But the render thread is *inside* `data_callback` at that moment. `data_callback`
   calls the virtual `decode_data()` (line `buffer_.size() <= ...` → `decode_data`),
   which is now pure (`= 0` in the base) → `__cxa_pure_virtual` → `abort`.

The OS stream (which calls back into `this`) was being stopped **too late** — after
the derived state and vtable were already gone.

### Fix

Stop and release the backing hardware stream *before* the derived destructor
invalidates the object, i.e. at the very top of the most-derived destructor while the
vtable is still intact. Added a protected `dsp_output_stream_shared::shutdown_stream()`
(`stream_->stop(); stream_.reset();`, which joins the render thread) and call it first
in every concrete destructor: `~dsp_output_stream_ffmpeg` and the iOS non-ffmpeg
`~dsp_output_stream_pcm`. The base destructor still calls it as a safety net for a
stream that somehow outlived a derived class. Once `shutdown_stream()` returns no
callback can be in flight, so freeing `codec_`/`av_format_` afterwards is safe.

### Dead end worth avoiding

Guarding inside `data_callback` (e.g. an "alive" flag) does not close the window: the
render thread can already be past the guard when the vtable degrades. The teardown
must *synchronize* with the render thread (stop/join) before the object stops being
its real type — a flag check on the callback side is not enough.

## Verification

Release simulator regression suite: `PASS=12 FAIL=0` (Final Battle, Calculator,
N95 Calculator, string catalog). Both fixes are defensive teardown-path changes that
leave the normal running path untouched.
