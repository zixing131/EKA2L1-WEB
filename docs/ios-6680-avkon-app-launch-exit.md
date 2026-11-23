# 6680 application exits at launch: EKA1 MTM packed-record boundary

**Resolved.** On the Nokia 6680 (`rm-36`, Symbian OS 8.0a / S60 2nd Edition
FP2, EKA1), 黄泉道 / `sdancer` (`0x03510510`) flashed briefly and returned to
the application list. The failure was not in Avkon startup or scheduler timing.
The EKA1 Messaging Server serialized MTM registry records in the pre-8.0 format,
so the guest could not find the SMS client MTM required during application
construction.

## Symptom and the misleading first diagnosis

`sdancer` exited through the ordinary EKA1 `RThread::Kill` path with reason 0,
without a panic, access violation, or graphics halt. Asphalt 2 launched normally
on the same firmware. Calculator also appeared to exit in early tests, so the
initial diagnosis grouped the failures as full-Avkon applications and suspected
a race near `CActiveScheduler::Start()`.

That interpretation was wrong. A reason-0 thread teardown only describes the
final process exit; it does not prove that the active scheduler started and later
returned. Guest instruction tracing showed that `sdancer` never reached
`CActiveScheduler::Start()`. A leave during application construction propagated
through EikStart, after which the launch wrapper ended normally.

Calculator was also a false control. EKA2L1 shares drive C between installed
firmwares, and this test environment contained a 46-byte `calcsoft` state file
written by a different Calculator version. Removing it allowed the 6680 ROM to
create its own 70-byte state and stopped that separate early exit. It was not
evidence for a common Avkon failure. The 6680 Calculator's remaining blank guest
output is a separate rendering/first-frame issue and is not involved in
`sdancer`'s launch path.

## Following the guest leave

A ring buffer around the EKA1 guest `User::Leave` implementation identified the
last unhandled error as `KErrNotFound`. The return address mapped into
`msgs.dll`, export ordinal 141. Disassembling the 6680 ROM image and comparing it
with the Symbian Messaging Framework source identified the function as
`CClientMtmRegistry::NewMtmL(TUid)`. Its requested UID was `0x1000102C`,
`KUidMsgTypeSMS`.

The host Messaging Server was not missing the resource. Its registry contained
nine client MTM components, including:

```
group UID      component UID  specific UID  name
0x1000102C     0x10003C5F     0x1000483B    Text message
```

The guest nevertheless enumerated a corrupted registry. That reduced the fault
to the response of `EMsvFillRegisteredMtmDllArray` rather than ROM scanning,
registry persistence, or an invalid application request.

## The ABI mismatch

The 6680 `msgs.dll` does not unpack this IPC with the exported stream
`CMtmDllInfo::InternalizeL`. Its EKA1-specific
`TMsvPackedRegisteredMtmDllArray` reader walks a fixed-layout record. ROM
disassembly established the epoc80 layout as:

| Field | Size |
|---|---:|
| MTM type UID | 4 |
| Technology type UID | 4 |
| Human-readable `TBuf<50>` | 108 |
| DLL `TUidType` | 12 |
| Entry point | 4 |
| Packed version | 4 |
| Send/body/available capability fields | 12 |
| **Total** | **148** |

EKA2L1 already produced this fixed EKA1 structure, but it appended the three
32-bit capability fields only for `epoc81a` and later. On `epoc80` it emitted a
136-byte record while the guest advanced by 148 bytes. The first MTM UID could
be read, but every following record began 12 bytes away from where the guest
expected it. SMS is the second client MTM in this ROM, so
`CClientMtmRegistry::NewMtmL(KUidMsgTypeSMS)` deterministically left with
`KErrNotFound`.

An attempted switch to the newer variable-length stream format was useful as a
negative result. The fixed reader interpreted the first name bytes as a Symbian
descriptor header and attempted a huge allocation. This confirmed that the
existing EKA1 fixed structure was correct and that only its version boundary was
wrong.

## Fix

The Messaging Server now includes the three 32-bit capability fields for EKA1
starting at `epoc80`, while preserving the shorter structure for epoc6/epoc7 and
the existing EKA2 stream format. No application-specific behavior is involved.

With a clean build and all tracing removed:

- The Release build takes `sdancer` through its sound prompt, title, new-game
  introduction, and into the first playable scene. Holding the game's `8`
  movement key advances the character through the opening trigger and onward to
  the stone steps at a stable 15–16 FPS.
- Asphalt 2 still reaches its language screen at approximately 40 FPS on the
  same 6680 firmware.
- No guest panic, access violation, or process exit appears in either log.
- Both required Release regression passes complete all 11 Final Battle and
  Calculator checks without a guest crash.

## Dead ends worth retaining

- Emptying drive C did not change `sdancer`; its required SMS MTM comes from the
  ROM registry.
- Supplying an empty application document name did not change the leave.
- Window-group and AKNCAP opcode `0x3F` warnings also occur in the working
  Asphalt control.
- Coarse process-exit logs cannot distinguish a trapped construction leave from
  a scheduler that really ran and stopped.
- The apparent extensive-logging/timing correlation was an artifact of judging
  the final teardown rather than tracing the earlier guest leave. The malformed
  record length is deterministic.
