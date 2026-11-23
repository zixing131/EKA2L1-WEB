# OpenVG round caps and joins were rendered as square or bevelled strokes

RhythmBelle uses `QPen(Qt::RoundCap, Qt::RoundJoin)` for osu! slider bodies. The
same `MEGALOVANIA [Normal]` slider was rounded on the macOS Qt raster backend,
but its open ends were square and its corners were bevelled on the X7/Belle
OpenVG path. Repeated screenshots ruled out timing or playfield scaling: the
centerline and circle geometry agreed, while only the stroke silhouette differed.

gnuVG accepted `VG_STROKE_CAP_STYLE` without storing it, and its round-join branch
explicitly fell back to the bevel implementation. The context now validates,
stores, and returns butt, round, and square cap styles. Simplified-path stroke
tessellation adds semicircular fans for round open-contour caps, half-width
extensions for square caps, and circular fans on the outside of round joins.
Butt caps remain the default. Dashed-path cap behavior is unchanged for now.

Qt 4's Belle OpenVG paint engine does not issue `VG_STROKE_CAP_STYLE` for the
`QPen` used by RhythmBelle, so the emulator fix alone cannot change that client.
RhythmBelle therefore also emits coincident endpoint/join discs for its slider
fallback. The EKA2L1 work is still required for guests that call OpenVG directly
and prevents the no-op state setter from becoming a separate future trap.

Validation used an iOS Simulator Release build on RM-707. The final RhythmBelle
capture has continuous round slider ends and joins, while its normalized
playfield coordinates remain within the existing one-to-two-pixel capture
tolerance. The standard Final Battle, Calculator, and N95 Calculator captures
were produced after the change; string-catalog reconciliation also passed.
