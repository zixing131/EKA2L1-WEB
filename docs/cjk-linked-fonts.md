# Chinese text drew as boxes because nobody assembled the ROM's linked fonts

## Symptom

X-plore on the X7 (rm-707), switched to Chinese through Menu → Tools →
Configuration → Language, drew part of its UI as `.notdef` boxes. Not all of it:
the row labels, the title and the right softkey were boxes, while the values in
the same rows (`按文件名`, `小字体`) and the help line at the bottom
(`更换程序语言.`) were perfectly readable Chinese.

Importing a CJK TTF through the new font importer changed nothing, which is what
prompted the investigation.

## Narrowing it down

The split between working and broken text was the useful part of the symptom: a
font capable of Chinese was clearly in play, so this was never "no CJK font".

Two probes settled it. One in `freetype_font_adapter::get_glyph_bitmap`, logging
whenever `FT_Get_Char_Index` returned 0 for a codepoint above U+2000, named the
culprit immediately — every missing glyph was being asked of `Nokia Sans S60`, a
Latin-only face. The other in `font_store::seek_the_open_font`, logging the
requested name, height, style and the winner, showed why:

```
seek '' h=20 style=20002 -> SCORED 'Nokia Sans S60 SemiBold'      (score=7940)
seek '' h=20 style=20000 -> SCORED 'MHeiM-C-GB18030-S60 Regular'  (score=3640)
```

Same empty typeface name, same height; only bit 1 of the style — bold — differs.
X-plore asks for its labels in bold, and the store's scoring paid 5000 for a
weight match while character coverage was worth only 100 per bit, so any Latin
bold face beat every CJK regular one. The imported font was regular too, which is
why importing it helped nothing.

That explains the choice, but not what a real device does differently. The answer
was in the ROM: `Z:\Resource\Fonts\link.ini`, a UTF-16 file listing, per product
variant, groups of typefaces to present as one:

```
[SCHR_LINK_START]
Nokia Sans S60 SemiBold : GROUP2 : CANONICAL1 : REGULAR : SNSemiBold : FNNOKSCHRSANSSBLF :
MHeiM-C-GB18030-S60     : GROUP1 : CANONICAL0 : REGULAR : SNSemiBold : FNNOKSCHRSANSSBLF :
[SCHR_LINK_STOP]
```

`AknFontProvider::InitializeSystemL` (oss.FCL.sf.mw.uiresources,
`fontsupport/fontprovider`) parses this and registers each `FN` name with the
font store through `CLinkedTypefaceSpecification::RegisterLinkedTypefaceL`. From
then on `NOKSCHRSANSSBLF` is a real typeface that renders Latin from Nokia Sans
and everything else from MHei.

The emulator log had been saying this all along: the very first font lookups
after boot were for `NOKSCHRSANSSBLF`, and resolved to whatever the scoring
picked, because no such typeface existed.

## Dead ends worth avoiding

**Glyph-level fallback is not how Symbian does it.** The obvious fix — when the
chosen font lacks a codepoint, borrow the glyph from another font in the store —
has no counterpart in the OS. `CBitmapFont::GetCharacterData`
(oss.FCL.sf.os.textandloc, `fontservices/fontstore/src/FNTSTORE.CPP`) returns
`EFalse` for a glyph its open font does not have, and that is the end of it. CJK
coverage comes from the font being a linked one, not from the store searching
around.

**Aligning the scoring alone does not fix it.** `MatchFontSpecsInPixels` pays 2
for a weight match and nothing for coverage, but for an empty name it still ranks
Nokia Sans S60 SemiBold (20) above MHeiM-C-GB18030-S60 Regular (18). Only the
presence of the linked typeface changes the outcome.

**Waiting for the guest to register the linked fonts does not work either.**
`fbs_register_linked_typeface` never arrives: `AknFontProvider` gates the whole
thing on `KFeatureIdFfLinkedFontsChinese` (id 159) plus the PRC-font and hi-res
features, and EKA2L1's feature manager finds no `Z:\private\102744CA\featreg.cfg`
in these ROM dumps — the log says `Feature registration config file not present!`
— so it only reports the handful of features `do_feature_scanning` hardcodes.
Setting the emulator's system language to PRC Chinese enables `feature_id_chinese`
and nothing else, and no registration IPC follows.

## Conclusion

Two changes, both matching what the OS does with the same data.

`fbs_server::load_linked_fonts` reads `link.ini` from each drive's font folder
once every font is loaded, and `linked_font_file_adapter` presents the components
as a single face: a glyph comes from the first component that has it, in file
order — which is why the Latin component being listed first matters — while the
element flagged `CANONICAL` supplies the attributes, metrics and sizes. Typefaces
whose components this ROM does not ship simply fail to assemble, which is how the
other variants' sections drop out without any feature flag. Linked typefaces are
inserted at the front of the store because `CFontStore::LoadFontsAtStartupL`
loads them before the rest of `resource\fonts`, and equally good candidates are
settled by taking the first.

Separately, `seek_the_open_font` now follows
`CFontStore::GetNearestFontToDesignHeightInPixels`: the requested name first,
against the full face name then the family, both also requiring the slant and
weight asked for; then `MatchFontSpecsInPixels` scoring — 10 for the name,
`10 - |diff|` for the height, 3 for monospace, 2 each for weight and slant, 1 for
serif. The bitmap-type and outline/shadow terms are left out rather than
reweighted: no face attribute here carries them, and they would score every
candidate alike.

## The 5320, which needed three more things

The X7's fix did nothing for the 5320 (rm-409), whose ROM carries no
link.ini at all.

Its Chinese fonts were not even loaded: they ship as `s60sc.ccc` and
`s60tchk.ccc`, TrueType files with an extension `add_font` did not
recognise. `CFontStore::AddFileL` hands a file to every rasterizer and keeps
whichever one recognises it, so the extension is only a hint; loading by
content picks them up.

An unnamed request could then be answered by `Series 60 ZDigi`, a
digits-only face. `MatchFontSpecsInPixels` leaves such a request to height
and style alone, so every candidate ties and the winner is whichever font
the store met first -- load order on a device, host directory order here.
Ties are broken on coverage instead, which is also what the invented
scoring this replaced was doing with its coverage term.

Even then nothing asked for the font the user had imported. A CJK variant
pairs its Latin and CJK typefaces through link.ini; a ROM shipping no CJK
font ships no link either, and nothing in a request says which script it is
about to draw. Imported fonts are attached to every device face as trailing
components of a linked typeface -- the same arrangement link.ini describes,
with the device's face canonical, so only glyphs it lacks reach the import.

Worth knowing when reproducing this: X-plore only persists its language
setting when the configuration page is left, and its own text is drawn
through requests that name no typeface at all, which is why it lands on
whatever the store's ranking happens to prefer.

## The N-Gage, whose glyphs are a different shape entirely

The N-Gage (nem-4) ships three European `.gdr` bitmap fonts and nothing else,
so an imported font is its only source of CJK glyphs -- and it first drew them
one pixel tall, then as streaks.

Both come from a component being addressed as if it were the face it backs. A
metric identifier is issued by an adapter and only means something to that
adapter: FreeType's is a pixel size, gdr's an index into the face's bitmaps.
Passing the canonical's identifier straight through therefore asked FreeType
to render at "bitmap number 1". The linked adapter now records, in
`get_nearest_supported_metric` where the requested size is still known, what
each component calls that size, and translates per call.

The streaks are the format. `open_font_character_metric::bitmap_type` is
commented "always 1 bit bitmap type on EKA1", and the gdr adapter duly emits
Symbian's run-length monochrome -- a mode bit, a four bit count, then either
one scanline that repeats or that many verbatim, all least significant bit
first. FreeType emits 8 bits per pixel, which such a guest decodes as runs.

So `get_glyph_bitmap`'s bitmap type parameter became a request as well as an
answer: the linked adapter asks its components for the format the face
declares, and FreeType obliges with `FT_LOAD_TARGET_MONO` and the encoder that
moved out of the gdr adapter into `compress_monochrome_glyph`. Its own
monochrome rasteriser, hinted, is far kinder to a 12 pixel CJK glyph than
thresholding a grey render would be, which is what the linked adapter still
does for a component that ignores the request. A component is only attached
where its glyphs can be presented as the face's format at all; the reverse
direction, monochrome up to antialiased, is not implemented and nothing asks
for it.

Hinting is also why the first attempt crashed the guest with KERN-EXEC 3
reading past the shared chunk. Grid-fitting means the bitmap is not the size
the outline metrics describe, and the client decodes the runs using the
metrics it was handed, so a monochrome glyph has to report what was actually
rasterised -- `bitmap.width`, `bitmap.rows` and `bitmap_left` / `bitmap_top`
rather than the values scaled from `glyph->metrics`.

Extracting that encoder turned up a bug in it. `common::extract_bits` numbers
bits from one, and the scanline comparison passed zero based positions, so
`num >> (0 - 1)` shifted by an underflowed count and identical scanlines never
compared equal. Every glyph was written out in full -- decodable, which is why
nothing looked wrong, but the compression had never once fired.

One deliberate deviation: S60's own `link.ini` parser treats every unrecognised
token as the component name, so `SNSemiBold` overwrites the typeface name read
moments earlier. The `SN` field is skipped here and the first unrecognised token
kept, that being the one which names a typeface the device actually ships.
