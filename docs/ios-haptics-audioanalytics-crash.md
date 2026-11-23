# TestFlight crash inside AudioAnalytics, triggered by keypad haptics

## Symptom

TestFlight build 26.7.0 (260825) on an iPhone 18,4 / iOS 26.5.2, roughly 20 minutes
into a session:

```
Exception Type:  EXC_BREAKPOINT (SIGTRAP)
Triggered by Thread:  10

Thread 10 Crashed:
0   CoreFoundation   __CFTypeCollectionRelease.cold.1
1   CoreFoundation   __CFBasicHashDrain
...
4   CoreFoundation   _CFRelease
5   Foundation       _NSXPCSerializationCreateWriteData
6   Foundation       -[NSXPCEncoder _encodeInvocation:isReply:into:]
7   Foundation       -[NSXPCConnection _sendInvocation:orArguments:...]
10  AudioAnalytics   ServerClient.sendMessageOnQueue(reporterID:message:category:type:rateLimit:)
...
19  CoreHaptics      __43+[CHMetrics doHandleErrorCode:description:]_block_invoke
```

Not one EKA2L1 frame on the crashing thread. `esr: 0xf2000001 (Breakpoint) brk 1`
with `__CFTypeCollectionRelease.cold.1` is CoreFoundation's deliberate trap for an
over-released object found while draining a CF collection.

## Narrowing it down

The crashing thread is useless on its own — it is entirely Apple code reporting a
metric. The signal is in the *other* threads of the same report.

Thread 19 is stuck in `_dispatch_sync_f_slow` inside the very same
`AudioAnalytics ServerClient.sendMessage`, reached from:

```
-[_UIFeedbackEngine _playFeedback:atTime:withCompletionBlock:]
-[_UIFeedbackCoreHapticsPlayer _internal_createPlayerWithPattern:]
-[CHHapticEngine createPrivilegedPlayerWithPlayable:error:]
-[PatternPlayer initWithPlayable:engine:privileged:error:]
-[CHMetrics handleInitForPlayer:events:isAdvanced:patternID:]
```

So two haptics-originated reports are in flight at once: thread 19 is publishing a
*player-init* metric and blocking on the serial queue, while thread 10 owns that
queue publishing an *error* metric (`doHandleErrorCode:`) and over-releases mid
NSXPC serialisation. The over-release itself is a bug in Apple's AudioAnalytics
client — nothing in this repo can patch it. What we control is how hard we make the
app hit that path.

Every haptic call site looked like this:

```swift
UIImpactFeedbackGenerator(style: .light).impactOccurred()
```

A freshly allocated generator, fired once, released immediately. That is the
documented anti-pattern: UIKit brings its CoreHaptics engine up for the generator
and lets it idle back down when the generator dies, so the on-screen keypad
(`KeypadComponents.swift` `press()`) and especially the sliding d-pad
(`updateActive`, which fires on *every* sector change during a drag) were driving a
continuous engine start/stop cycle. Each new pattern player emits an init metric;
each engine hiccup emits an error metric. The two report kinds racing on
AudioAnalytics' queue is exactly the crash above.

Dead end worth naming: chasing this as emulator heap corruption. The repo has real
heap-corruption history, so a trap in `_CFRelease` invites that reading — but the
corrupted object belongs to a system framework, lives on a system dispatch queue,
and the report itself hands you the concurrent second thread. Read the other
threads before reaching for the memory model.

A second contributor: `-[EKA2L1Emulator pause]` calls
`AVAudioSession setActive:NO` on background. CoreHaptics shares the process audio
session, so feedback played around that transition fails and produces precisely the
error report that races the init report.

## Fix

The feedback has to come from a generator that outlives the tap. New
`src/emu/ios/App/Haptics.swift` adds a `hapticImpact(_:trigger:)` view modifier
that all four call sites (keypad key press, d-pad sector change, keypad-layout
reset, controller-mapping capture) now use, each driven by a counter it bumps
instead of calling a generator directly:

- on iOS 17+ it is SwiftUI's own `.sensoryFeedback`, which owns the generator for
  as long as the modifier is installed — the framework already solves the lifetime
  problem, so there is no reason to hand-roll it;
- on iOS 16 (still the deployment target) it falls back to the `Haptics` enum,
  which caches one `UIImpactFeedbackGenerator` per style and calls `prepare()`
  after each impact so the engine stays warm, guards on
  `UIApplication.shared.applicationState == .active` because playing while the
  audio session is deactivated is what raises the error report, and is emptied by
  `Haptics.release()` from the `scenePhase` background branch in `EKA2L1App.swift`.

Driving the modifier from a counter rather than the control's own state matters:
the d-pad's `activeScan` also changes on release and on the finger leaving the
ring, and feedback is only wanted on the press edge.

This is a mitigation, not a repair of Apple's over-release. It removes the app's
ability to generate the concurrent metric traffic that provokes it.
