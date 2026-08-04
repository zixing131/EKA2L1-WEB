# Sky Force Reloaded corrupts the X7 display

Sky Force Reloaded reached its language-selection screen on the X7 without a
guest panic or a graphics halt, but the entire portrait display consisted of
repeating diagonal bands. The regularity of the bands pointed to a row-layout
problem rather than bad game assets or a shader failure.

The first misleading possibility was the pixel format. The X7 advertises
`EColor16MAP`, and interpreting the captured framebuffer as RGB565 did change
the colours but did not restore the image. Treating the same memory as tightly
packed 360-by-640 BGRA reproduced the emulator's corrupt output exactly.

The decisive check was to capture more than the nominal framebuffer size and
score adjacent rows while varying the candidate pitch. A 1472-byte pitch
(368 pixels) was the unique match; cropping the eight padding pixels from each
row reconstructed the complete, undamaged language-selection screen. The game
was therefore producing valid 32-bit pixels with the 64-byte-aligned row pitch
used by the X7's ScreenPlay framebuffer. The emulator uploaded those bytes as
if every row were the tight 1440 bytes, so each new row began eight pixels too
early and wrapped into diagonal bands.

Symbian's direct-screen path confirms that the framebuffer pitch is supplied
separately by `HALData::EDisplayOffsetBetweenLines`; it is not derived from the
visible width. EKA2L1 discarded that distinction in two places: its HAL
reported a tight row and the DSA texture upload omitted the graphics backend's
existing `pixels_per_line` argument.

The fix gives 32-bit ScreenPlay framebuffers a 64-byte-aligned pitch, exposes
that value through the display HAL, and supplies the corresponding pixels per
line when uploading active DSA memory. GPU-to-framebuffer synchronization now
also copies tight readback rows into the padded guest layout.

A useful dead end was changing the replacement
`CDirectScreenBitmap::BeginUpdate`. Runtime breakpoints showed that Reloaded
does not submit its frames through that API: its first update had an active
DSA session and a padded framebuffer, but the DSB probe was untouched.
Changing DSB also damaged the regular Window Server path used by Calculator.
The final fix therefore leaves the SCDV ABI and its tight bitmap contract
alone and applies the aligned interpretation only while a real DSA session is
active.

Older bitmap-screen architectures retain their existing word-aligned layout,
and no application UID or title-specific behavior is involved.
