# Ashen stops before its first frame after stale-handle validation

Ashen on the N-Gage ROM stopped during application startup. The emulator view
showed only a fragment of the application icon and a white rectangle at 0 FPS.
The process remained alive, with no panic, access violation, or graphics halt.
The same game had rendered normally before the Talking Tom fixes.

The OpenVG frame-transition work initially looked suspicious because it was the
largest graphics-related part of that change. Ashen does not use that path,
however. Reverting the later EKA1 window-opcode selection also made no
difference.

The regression came from stricter validation in `object_ix::get_object()`.
That change correctly stopped an old EKA2 handle from resolving to an unrelated
object after its slot was reused. It applied the same generation check to EKA1,
where existing clients can retain a handle while the emulator reopens the same
object into the same slot with a new instance value.

Runtime tracing caught Ashen requesting thread handle `0x400C0000` while the
slot contained the same `Ashen` thread under canonical handle `0x40130000`.
The index and object were still valid; only the emulated instance bits differed.
Rejecting that lookup prevented the UI initialization sequence from reaching
the Window Server.

The fix retains generation validation for EKA2 and uses the legacy slot
semantics for EKA1. Closing an EKA1 alias removes the slot's current canonical
handle from the bookkeeping list, rather than searching for the obsolete alias.
This preserves the stale-handle protection needed by newer systems without
breaking legacy clients.

After the fix Ashen reaches its title and demonstration scenes at 34–47 FPS.
The log contains no panic, access violation, graphics halt, or diagnostic
handle tracing.
