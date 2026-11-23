# Nokia 5320 Voice Recorder investigation

The RM-409 Voice Recorder (`0x100058CA`) initially opened with a generic error. This investigation did not reach a safe fix and all implementation changes were rolled back. The notes below preserve the useful findings and failed approaches for a future attempt.

## What was established

The recorder uses MMF custom interfaces during startup. The existing `mediaclientaudio_general.dll` patch did not implement the Custom Interface Builder protocol, recorder state transitions, or recording itself. The relevant interface identifiers are:

- Custom Interface Builder: `0x10200017`
- Custom Interface Builder implementation: `0x10207A8E`
- Audio Input: `0x101FAFD9`
- Audio Output: `0x10200018`

The official S60v3 implementation returns a five-word `TMMFMessageDestination`. Its first word is the interface UID and its second word is an object-container-generated handle; the remaining words are reserved. `CCustomInterfaceBuilder::DoBuildL` creates a message handler, adds it to `CMMFObjectContainer`, and returns that handler's destination. Returning arbitrary constant handles is therefore only a diagnostic approximation.

The original recorder utility also reports state changes asynchronously. An early patch called `MMdaObjectStateChangeObserver::MoscoStateChangeEvent` synchronously and passed the internal implementation as the callback object. The correct callback object is the public `CMdaAudioRecorderUtility`, and the callback must be queued. Replacing a new `CActive` subclass with an embedded `CAsyncCallBack` avoided exporting additional RTTI and vtable symbols, which otherwise shifted the unfrozen patch DLL's export ordinals.

The RM-409 ROM expects the EKA2 `KernelConfigFlags` SVC. The observed ROM flags were `0x1e`; platform-security and locked bits produce `0x9e`. This was a real compatibility gap, but it was reverted with the rest of the unfinished experiment because it was not independently regression-tested.

## Prototype attempted

The host audio dispatcher was extended with a recording-capable player mode. It opened an input DSP stream and wrote PCM16 mono 8 kHz samples into a WAV container, finalized the RIFF header on stop, exposed record position, and lazily reopened the resulting file for playback. The guest media patch gained a `Record` dispatch call, basic record state transitions, gain and duration behavior, and Custom Interface Builder responses.

The guest patch was rebuilt with the S60 3rd Edition FP2 SDK inside the Windows XP UTM virtual machine. This was useful because rebuilding on the native host cannot reproduce the GCCE ABI and export layout.

## Failure mode and dead ends

When both Audio Input and Audio Output proxies were returned, startup advanced further but eventually invoked the patched audio utility's internal `Stop` with `this == 0x38`. That became an invalid host audio handle (`0xffffffff`) and caused an access violation while transitioning state. Export routing was checked exhaustively: patch source ordinals 72 through 82 mapped to ROM destination ordinals 97 through 107, including source 81 to destination 106. The public patched `Stop` export was not among those routes, so a simple off-by-one export mapping was not demonstrated.

Temporarily refusing Audio Output did not provide a safe workaround. The proxy factory ignores the custom-command return code and decides success from the returned destination UID. Because the output descriptor was left unchanged, the caller consumed garbage and execution eventually jumped to `0x1000`, producing repeated access violations. A future diagnostic must always write a fully zeroed `TMMFMessageDestination` when an interface is unavailable.

Changing asynchronous custom commands to leave `TRequestStatus` pending also did not resolve the bad `Stop` receiver. Diagnostic SVC-trampoline logging showed that the relevant ROM path used a direct Thumb trampoline instead. The saved link register was inside RM-409 `customcommandutility.dll` around `0x82cf6739`; source and disassembly suggested a custom-command wrapper, but its exact method and the origin of `0x38` were not proven.

## Suggested continuation

A future attempt should first reproduce the official object-container handle semantics or explicitly return a zero destination for unsupported interfaces. It should then identify the exact `customcommandutility.dll` wrapper at `0x82cf6734` by building the matching OSS component with GCCE symbols or mapping its import veneer. Add a temporary guest-side trace at the public and internal `Stop` boundaries to determine whether `0x38` is already the receiver on entry or is introduced by an ABI mismatch. Only after that path is understood should host microphone capture be restored.

No code, binary patch, logging probe, build batch file, commit, or push from this investigation was retained.
