# Bloks shows no app icon: SVGB `<text>` coordinates are lists, and CDATA must be consumed

## Symptom

`Bloks v1.01(1).sisx` installs cleanly on an N95 (rm-320) — the package registers,
`applist` finds `Bloks, uid: 0x2004BD3A`, and the app launches — but the home list
draws the dashed placeholder square instead of an icon. Nothing is logged: the icon
decoder fails silently, and no `debinarized_Bloks.svg` is left in
`data/cache/icons/<device>/`, because `decode_mif_icon` deletes a cache file whose
conversion didn't produce a loadable document.

## Narrowing it down

The registration points at `!:\resource\apps\Bloks.mif`, so the failure is in the MIF
path rather than in applist or the SIS installer. Reading the file by hand:

```
uid=0x34232342 version=2 offset=16 array_len=2
  idx[0] offset=32 len=76657
icon uid=0x34232343 type=1 (svg) depth=11
```

Type 1 with magic `0x03FA56CE` — an ordinary SVGB (binarised SVG) entry, the same
shape Angry Birds and the ROM icons use. It is not the gzip-wrapped variant handled in
[`mif-gzipped-svg-icons.md`](./mif-gzipped-svg-icons.md).

Running `convert_svgb_to_svg` directly (a small host harness linked against
`libepocloader`) is far more informative than the emulator, because the error list
carries a byte offset:

```
svgb convert: FAILED, errors=1
  reason=7 (unexpected_attribute) offset=46727 data1=12288
```

Attribute id 12288 is `0x3000`, which is two adjacent bytes read one position out of
phase — the classic signature of a stream desync, not of an unknown attribute. The
partially written SVG says exactly where the reader lost sync:

```xml
  <g display="inline" id="layer2">
    <text x="7296"
```

`x="7296"` on a 96×96 icon is nonsense, so `x` is where the decoder stopped agreeing
with the encoder. The bytes are:

```
19 | 31 00 | 01 | 00 80 1c 00 | 30 00 | 01 | 00 80 35 00 | 05 00 | 00 00 18 00 | ...
^text  ^x      ^n  ^28.5          ^y      ^n  ^53.5          ^font-size ^24
```

`svgb_decode_float` consumed `01 00 80 1c` as one float (`real=1`, `integ=0x1c80`),
which is where 7296 comes from. With a one-byte count in front, everything lines up:
`x=28.5`, `y=53.5`, `font-size=24` — plausible values, and each following attribute id
lands on a real attribute. SVG lets `<text>` carry a coordinate *list* (one position
per glyph), so the binariser writes a count there and a bare float everywhere else.
The same count-prefixed layout already exists in this decoder for `stroke-dasharray`.

Fixing that exposed the second half of the same bug, three bytes later:

```
reason=9 (cdata_ignored) offset=46866
reason=4 (element_unimplemented) horizOriginX offset=46869
```

The `SVG_CDATA` branch in `convert_svgb_to_svg` was a commented-out block from the
reference decoder that only recorded "ignored" — it never read the string. The text
body's bytes stayed in the stream and were parsed as elements, and the icon was again
lost from that point on. Note that neither failure is fatal to the *file*: the
converter had already emitted 84 KB of perfectly good SVG. It just stops at the first
`<text>`, and Bloks happens to draw its wordmark with real text elements while every
icon that worked so far uses paths only.

## Fix

`src/emu/loader/src/svgb.cpp`:

* `x`/`y` decode through a new `svgb_decode_coord`, which reads the one-byte count and
  emits a space-separated list when the enclosing element is `<text>`, and otherwise
  falls back to the single-float path.
* The `SVG_CDATA` branch reads the string (one-byte length + UCS-2), XML-escapes it and
  writes it as element content. The enclosing element's closing tag is no longer
  indented when it holds character data, so nothing is injected into the text.

## Verification

The decisive check was differential, not visual: convert every SVG-typed entry of every
`.mif` reachable from the emulator's drives (1473 files, 320 SVGB entries across six
ROMs and all installed packages) with the old and the new converter, and hash the
output. Exactly two lines differ — the two entries of `bloks.mif`, from
`ok=0 errors=1` to `ok=1 errors=0`. Every other icon converts byte-identically, so the
count prefix really is `<text>`-only.

The converted SVG renders as the expected BLOKS wordmark over coloured blocks under
lunasvg, the icon appears in the iOS home list, and `scripts/ios_regression_test.sh`
passes 12/12.

## Still open

`dx`, `dy` and `rotate` — the other list-valued `<text>` attributes — are still mapped
to `svgb_decode_fail` and would abort a conversion outright. No icon in the corpus uses
them, so they were left alone rather than guessed at.
