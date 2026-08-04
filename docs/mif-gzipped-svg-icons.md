# Worms shows no app icon: MIF entries can hold a gzipped SVG

## Symptom

`Worms` (UID `0x2002B2DF`) installs fine on the X7 (rm-707) and appears in the iOS home
list with the right caption, but its icon cell stays empty. Everything else about the
app works — it launches and plays.

## Narrowing it down

The app itself was never in doubt: the log shows

```
[Service.Applist]: Found app: Worms, uid: 0x2002B2DF
```

so registration parsing, the `.rsc` and the caption were all fine, and
`reg->icon_file_path` pointed at a real file (`e:\resource\apps\worms.mif`, 101 KB).
That moves the whole question into the icon decoder.

The first useful signal came from the debinarized-SVG cache
(`data/cache/icons/<firmware>/`): every app whose icon renders has a
`debinarized_<name>.svg` there, and Worms had none. So the MIF path was being entered
and failing, not skipped.

Dumping the MIF by hand explains why:

```
mif header : ver=2, array_len=2
icon header: type=1 (mif_icon_type_svg), depth=6, mask_depth=4
payload    : 1f 8b 08 00 ...
```

`1f 8b` is a gzip stream. The entry is a *gzipped plain SVG* (SVGZ) — not the
binarised SVGB form the decoder expects, and not the plain-text SVG it falls back to.
Inflating it yields a perfectly ordinary 48×48 Adobe Illustrator export with 570
paths, which lunasvg renders without complaint (verified by linking the repo's own
`liblunasvg.a` against a 20-line harness: 2259 of 2304 pixels come out with non-zero
alpha).

The failure chain in the decoder was:

1. `header.type == mif_icon_type_svg` → call `convert_svgb_to_svg()`.
2. SVGB starts with magic `0x03FA56CC..CF`; a gzip stream doesn't, so conversion
   fails at the very first check with `svgb_convert_error_invalid_file`.
3. Every caller reads that particular error as *"the payload is already plain SVG"*
   and writes the **raw bytes** into the `.svg` cache file.
4. What lands on disk is gzip binary, so lunasvg (or `QSvgRenderer`) refuses it, the
   document is null, and the icon comes back empty.
5. The iOS bitwise-icon fallback finds nothing either — this app only ships a MIF.

A dead end worth skipping next time: the inflated SVG declares its namespaces through
DOCTYPE-internal entities (`xmlns="&ns_svg;"`), and lunasvg's XML parser deliberately
skips the internal subset, so those entities never resolve. That looks alarming but is
harmless — `decodeText`'s failure return is ignored at the call site, and `xmlns` maps
to `PropertyID::Unknown`, so the attribute is dropped rather than failing the parse.

## Fix

`svgb_convert_error_invalid_file` really means "not SVGB", which covers more shapes
than the plain-text one that was assumed. The three shapes an SVG-typed MIF entry can
take are now all handled in one place, `loader::convert_mif_icon_to_svg()`
(`src/emu/loader/src/mif.cpp`): gzip is detected by magic and inflated first (miniz
only understands zlib-wrapped or raw deflate, so the gzip header is skipped by hand
and the body run as raw deflate), then SVGB, then plain text. NVG entries go through
the same entry point; raster entries are still rejected.

This was never iOS-specific — the same copy-pasted block lived in four places, all
equally blind to SVGZ: the iOS bridge, the Qt app list, the Android launcher, and the
in-guest `!AknIconServer` HLE (so a game drawing its own icon saw the same blank).
All four now call the shared decoder. The iOS path additionally deletes the cache file
when decoding or loading fails, instead of leaving a corrupt `.svg` that the
mtime-based freshness check would happily serve back on the next launch.
