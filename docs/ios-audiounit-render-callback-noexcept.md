# AudioUnit render callback must be a `noexcept` boundary

## Symptom

TestFlight build 260771 produced an `EXC_CRASH (SIGABRT)` whose crashing
thread (Thread 20) is CoreAudio's realtime I/O render thread:

```
0  __pthread_kill
2  abort
4  demangling_terminate_handler()
5  _objc_terminate()
7  std::terminate()
8  caulk::rt_function_ref<int (AudioConverterAPI*)>::functor_invoker<
       AudioConverterFillComplexBufferRealtimeSafe::$_0>
10 AudioConverterFillComplexBufferRealtimeSafe
11 AUInputFormatConverter2::PullAndConvertInput
12 AUConverterBase::RenderBus
13 AURemoteIO::RenderBus
...
19 AURemoteIO::IOThread
```

Simultaneously another thread (Thread 4, the guest CPU loop) is tearing the
same audio stream down:

```
5  AURemoteIO::Stop()
7  audiounit_ios_output_stream::stop()
8  dsp_output_stream_shared::~dsp_output_stream_shared()
9  dsp_output_stream_ffmpeg::~dsp_output_stream_ffmpeg()
10 mmf_dev_server_session::~mmf_dev_server_session()
...
19 kernel::process::kill(...)
20 kernel::thread::kill(...)
```

This is the same app-teardown window as the round-5 audio race, but a
*different* failure: the round-5 report aborted through `__cxa_pure_virtual`
(a direct `abort`, no exception), whereas this one unwinds through
`_objc_terminate` / `std::terminate` — i.e. a genuine uncaught C++ exception.

## Root cause

`AudioOutputUnitStop()` (called from the stream destructor on the teardown
thread) does not cut the render thread off instantly: it drains the *last*
in-flight render callback before returning. That final callback runs the full
guest data path — `output_render_cb` → `audiounit_ios_stream_base::call_callback`
→ the `dsp_output_stream_shared::data_callback` lambda. That path is allowed to
throw:

- ffmpeg decode (`av_packet_alloc`, `dest.resize`, swresample) and the
  `std::vector` scratch growth can throw `std::bad_alloc`;
- `std::lock_guard<std::mutex>` acquisition can throw `std::system_error`;
- the `more_buffer` completion re-enters kernel/guest state.

`output_render_cb` / `input_render_cb` are plain C-ABI function pointers handed
to CoreAudio, and here they are reached *through*
`AudioConverterFillComplexBufferRealtimeSafe` (RemoteIO inserts an
`AUConverter` because the guest stream rate — 8 kHz — differs from the hardware
rate). That realtime-safe converter path is not exception-aware: any exception
that escapes the input proc unwinds straight into `std::terminate()`.

The round-5 fix (stop/join the stream at the top of the most-derived
destructor, while the vtable is still intact) removed the pure-virtual variant
by making `decode_data()` resolve to the real ffmpeg override during the drain.
But it does nothing to stop that same drained callback from *throwing* for one
of the reasons above — the exception still crosses the C boundary and aborts.

## Fix

Make the two render callbacks true `noexcept` boundaries: mark them `noexcept`
and wrap the guest-facing body in `try { ... } catch (...) {}`. On the output
path a caught exception falls back to emitting silence (`memset` +
`kAudioUnitRenderAction_OutputIsSilence`); on the input path it is simply
swallowed. No exception may ever unwind across CoreAudio's C callback ABI.

This is a general driver-contract fix, not a title workaround: throwing across
a C callback boundary is undefined behavior regardless of who threw, and this
is the correct place to contain it. It composes with — does not replace — the
round-5 stop-before-teardown ordering.

See `src/emu/drivers/src/audio/backend/audiounit_ios/stream_audiounit_ios.mm`.

## Dead ends worth skipping

- Trying to pin down the exact exception type from the `.ips` is a rabbit hole:
  the realtime converter frame is all that survives, and several sites in the
  drained callback can throw. The boundary fix is correct for all of them, so
  the precise type does not change the fix.
- Assuming round-5 already covered this because it is the same build/teardown
  path — it does not; pure-virtual `abort` and `std::terminate` are distinct
  exit routes and only the former was addressed.
