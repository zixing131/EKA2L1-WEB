# Talking Tom never hears anything on Symbian^3

Talking Tom on the X7 (rm-707) ignored the microphone completely: the cat played
its tapped-on animations but never reacted to sound. An earlier round of work had
already made the iOS RemoteIO input path capture real audio, so the suspicion was
that the capture itself had regressed.

## Narrowing it down

Instrumenting the MMF DevSound server showed that the guest reached the recording
path and then stopped talking to the server entirely. The A3F opcode sequence was:

```
0 post_open, 4 init3, 11 max_gain, 13 set_gain, 5 capabilities,
42 samples_recorded, 6 config, 7 set_config, 26 record_init   <- last opcode ever
```

Meanwhile the host input stream was running and delivering real samples, but the
driver-level read queue was permanently empty. Nothing was ever handed to the
guest.

Two things were missing, and each alone was fatal:

* `do_submit_buffer_data_receive()` bailed out when `buffer_info_` was empty. That
  is right for the pre-Symbian^3 protocol, where the client posts an asynchronous
  `GetBuffer` request *before* `RecordInit`, and the recorded buffer completes that
  request. Under A3F the client has no outstanding request at all — it waits for a
  `BufferToBeEmptied` event on the message queue. So no host read was ever queued,
  no buffer ever completed, and the event that would start the cycle was never
  sent.
* Even if the event had been sent, `EMMFDevSoundProxyBTBEData` (opcode 56) had no
  handler. Only its playback twin `BTBFData` (55) was dispatched. The client's
  `CMsgQueueHandler::DoBTBECompleteL` calls it synchronously to learn the buffer
  size and to receive the chunk handle, so the guest would have blocked there.

`mmfdevsoundsession.cpp` and `mmfdevsoundcallbackhandler.cpp` in
`SymbianSource/oss.FCL.sf.os.mmaudio` define the contract: on a filled buffer the
server copies the data into the shared chunk and posts a BTBE event; the client
then calls `BufferToBeEmptiedData`, which writes a `TMMFDevSoundProxyHwBuf` into
slot 2 and completes with the chunk handle when `iChunkOp == EOpen`, or with
`KErrNone` otherwise; the client finally calls `RecordData` to ask for the next
buffer.

The fix follows that: for A3F, `record_init` / `record_data` create the chunk and
queue the host read without needing a client request, and the new
`get_recorded_buffer` fills the buffer description and hands over the chunk handle
the first time each chunk is used. The old protocol keeps its original path.

## A dead end worth recording

With the loop running, Talking Tom froze: no idle animation and no reaction to
taps. That looked like the fix had starved or deadlocked the guest, but the record
cycle was still ticking at exactly real time (one 2048-byte buffer every 64 ms),
so the guest thread was not spinning.

Replacing the captured samples with digital silence made the cat animate normally
again, and injecting an eight-second synthetic tone into that silence made it
switch to its listening/reacting pose. The "freeze" was Talking Tom permanently
*listening*: the simulator's host microphone is the Mac's input device, and with
that input gain the room noise floor sat around -40 dBFS, which is above the
game's trigger threshold. This is a property of the test environment, not of the
emulator — but it makes a noisy microphone look exactly like a hang, so it is
worth knowing before chasing a scheduler bug.

## Verification

The Release simulator build passed the default regression suite (12/12: Final
Battle, Calculator, N95 Calculator) and the X7 Angry Birds touch suite (5/5).
Talking Tom launches, runs the full record cycle against the live microphone, and
the log contains no panic, access violation, or unimplemented MMF opcode.
