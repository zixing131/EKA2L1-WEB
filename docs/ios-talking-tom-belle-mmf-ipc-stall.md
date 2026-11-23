# Talking Tom stays black on X7: incomplete Belle MMF dispatch

Talking Tom Cat installed successfully on the X7 ROM and remained alive after
launch, but its guest surface stayed black at 0 FPS. There was no panic or
access violation. The last useful log entry was an unimplemented opcode 11 on
`!MMFDevServer`.

The graphics errors near startup initially made the OpenVG path look
suspicious. They were not the first-frame blocker: Talking Tom had not reached
its OpenVG rendering calls. The client was still synchronously initializing
Phonon's audio input.

The MMF service has separate opcode layouts for S60v3/v5 and the newer
Symbian^3 architecture. Its shared handlers already implemented gain,
recording, balance, and format queries, but most of those handlers were wired
only into the old dispatch table. In the Belle layout opcode 11 is
`MaxGain`, not the old layout's `Gain`. Logging an unknown opcode without
completing its synchronous IPC left the Qt application thread asleep forever.

Connecting the existing handlers to their Symbian^3 opcode counterparts lets
the application continue to its first frame. It then exposed the same omission
for opcode 42, `SamplesRecorded`, which Qt polls while audio input is active.
The input DSP path also did not maintain a sample count, so the new handler now
returns the samples accepted by the recording callback.

That exposed a separate host-side omission: the iOS driver had a real
AURemoteIO input bus and capture callback, but kept `AVAudioSession` in the
playback-only category and declared no microphone usage string. It therefore
could not be treated as working capture on a real device. Starting the first
guest input stream now requests system microphone access, changes the session
to play-and-record after approval, and starts RemoteIO asynchronously. The
permission callback is lifetime-guarded so closing the guest stream while the
prompt is visible cannot call through a freed stream.

After the fix, Talking Tom reaches its animated 3D scene and a tap on the cat
produces the expected reaction. The MMF log no longer ends on opcodes 11 or 42.
Resetting microphone privacy state produces the native permission alert on the
next launch. The change is service-level rather than title-specific and applies
to other Symbian^3 clients using DevSound recording.
