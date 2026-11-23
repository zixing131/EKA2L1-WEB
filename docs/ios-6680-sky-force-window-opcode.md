# Sky Force stays white on the 6680

The original Sky Force, not Sky Force Reloaded, launched on the Nokia 6680
profile into a permanently white 176 by 208 guest display. The emulator kept
refreshing at about 32 FPS, and the game process remained alive.

The repeated redraw-window warnings initially suggested that an unimplemented
window command had prevented the splash screen from being composed. That was a
dead end: Sky Force uses direct screen access, and LLDB showed that the physical
framebuffer, guest address mapping, display mode, and screen information
returned to the game were all correct. The framebuffer stayed white because
the DSA clipping region contained no rectangles.

The DSA request targeted a full-screen window whose flags said it was visible
but not active. This matches the original WSERV contract: its
`CWsDirectScreenAccess::Request()` calls `CWsClientWindow::GenerateTopRegion()`,
which returns an empty region for an inactive window. Making DSA recalculate
the region or implicitly activate the window would therefore hide the earlier
compatibility error.

Tracing the target window's commands exposed that error. The 6680 WS client
reported version 1.0.151 and sent opcode `0x0e` for `Activate()` followed by
`0x0c` for `AbsPosition()`. EKA2L1 nevertheless treated every EPOC 8.0 client
as using the older window opcode table, which lacked `AbsPosition`, and shifted
every opcode from `0x0c` upward. Consequently `Activate()` became
`Invalidate()` and never set the active flag.

The fix makes the WS client build number the authority for this particular
opcode-table conversion. Builds through the old-architecture cutoff still get
the missing-`AbsPosition` adjustment, while later EKA1 clients such as the
6680's build 151 retain the opcodes they actually send. The separate old DSA
protocol selection remains unchanged. With the window activated normally,
WSERV calculates a full DSA region and Sky Force renders its splash screen.
