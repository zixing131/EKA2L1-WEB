# Talking Tom recording corrupts the host heap on iOS

TestFlight build 260819 could run Talking Tom on the X7 ROM after the Belle
MMF dispatch fix, but a physical iPhone crashed after a few minutes. The
exactly matching dSYM (UUID `9DDF6205-4084-3AD4-995D-5DAF6FCFD876`) placed
the fault in `object_ix::close(0xE0)`: the process handle record contained an
invalid host object pointer, and the virtual `decrease_access_count()` call
faulted while loading its vtable.

The close call was a downstream victim. Enabling the previously dormant iOS
RemoteIO input path introduced a real producer thread into
`dsp_input_stream_shared`. The CoreAudio thread consumed `read_queue_` and
updated `read_bytes_` while the guest CPU thread concurrently pushed new read
requests. Neither side synchronized the `std::queue`. Its container metadata
could therefore be corrupted, after which the recording callback's `memcpy`
used a bogus destination and damaged unrelated host heap objects. The input
class also accidentally declared a second `callback_lock_`, so callback
registration and invocation used different mutexes.

The input request queue, byte cursor, and completion transition are now
serialized. The host callback releases that state lock before notifying the
kernel, then verifies the same request is still at the front before removing
it. Callback functions are copied while holding the shared base-class callback
mutex and invoked after releasing it. DSP sample counters are atomic because
the audio and guest threads also access those concurrently.

The RemoteIO scratch buffer and `AudioBuffer` metadata now use the configured
channel count instead of describing every capture buffer as mono while MMF
requested stereo. Separately, the kernel handle table now rejects free or
generation-mismatched records and fully clears records during reset. This
matches Symbian stale-handle behavior and prevents a bad guest handle from
closing an object later allocated in the same slot, but it is not used as a
substitute for fixing the audio heap corruption.

Both Debug and Release simulator builds kept Talking Tom's live recording path
running beyond the physical-device report's 158-second failure point. The
Release run remained on the rendered 3D scene after more than three minutes;
the only temporary black screenshot was the simulator display sleeping, and
waking it showed the unchanged live scene. The log contained no panic, access
violation, MMF opcode, AudioUnit, or bad-handle failure.

The Release simulator build passed the default regression suite twice
(12/12 each: Final Battle, Calculator, and N95 Calculator) and the X7
Angry Birds suite once (5/5).
