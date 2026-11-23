# Every character a linked font's fallback supplied drew as .notdef

## Symptom

On the Chinese N95 ROM (`rm-321`), ZipManager rendered its whole UI as empty
boxes — title, list items, softkey labels. The Calculator on the same device was
the same story: the digits in the result field were fine, but the `选项` /
`退出` softkeys were boxes. Other devices (5320, X7) were unaffected, and so was
every app whose text is Latin-only on those devices.

The user had imported a Chinese TrueType font (`MHei18030C5.ttf`) into
`<storage>/fonts`, which the font store attaches to every ROM face as a
fallback component, so the glyphs were unquestionably available somewhere.

## Narrowing it down

The device's `Z:\resource\Fonts` holds only three files: `s60sc.ccc`,
`s60tchk.ccc` and `s60zdigi.ttf`. The ROM's Font Provider resource asks for
`Nokia Hindi S60`, which is not among them, so the store falls through to
`CFontStore`-style similarity matching and picks **Series 60 ZDigi** — a
72-glyph digits face. That is a bad pick, but not the bug: the imported font is
linked in behind it, and every CJK codepoint did resolve to that component.

Server-side instrumentation confirmed the whole rasterizing path was healthy:

- `linked_font_file_adapter::component_index_for()` routed every CJK codepoint
  to the imported font, not to the canonical ZDigi face.
- `fbscli::rasterize_glyph()` was called 65 times and returned a correct bitmap
  each time — dumping the 8bpp buffer as ASCII art printed recognisable 管 / 理 /
  器 glyphs with sane metrics (15×15, advance 15).

Two experiments then ruled the rasterize path out entirely:

- Overwriting every CJK glyph bitmap with solid `0xFF` before it reached the
  guest changed nothing on screen.
- Forcing `horizontal_advance` to 30 for those glyphs changed neither the boxes
  nor their spacing (measured at ~9 guest pixels either way).

So the guest never used any of it. Measuring the boxes gave the answer: 8×12 at
advance 9 is exactly ZDigi's `.notdef` at that size. And re-reading the
screenshot, the Latin title `Zip manager` was boxed too — this was not a CJK
problem at all, it was *every* character ZDigi itself could not draw.

The path that actually paints this text is EKA2L1's own. `CWindowGc::DrawText`
becomes a `gdi_store_command_draw_text`, and `gdi_command_builder` renders it on
the host through `font_atlas` (`gstore.cpp`), never touching the guest-side
`CFbsBitGc` rasterizer. `font_atlas` talks to one adapter through
`begin_get_atlas` / `get_glyph_atlas`, and `linked_font_file_adapter` forwarded
all three atlas calls to the canonical component only — with a comment claiming
nothing in the font store used that path. It does: it is *the* text path for
every S60 UI. Anything the canonical face lacked came back as glyph 0.

The 65 rasterize calls were real, just not for drawing — measurement (text
width) goes through them.

## Fix

The packing moved to where the buffer is. `font_atlas` now owns the rectangle
packer; adapters only measure and draw.

The old interface — `begin_get_atlas` / `get_glyph_atlas` / `end_get_atlas` —
kept the packer inside the adapter, so one atlas could only ever be filled by
one of them. That is what made a linked typeface impossible to serve: its
components are separate adapters, and each would have laid the same buffer out
from scratch on top of the others. Nor can they share a packer, since FreeType
and gdr keep a bare `stbrp_context` while stb keeps a `stbtt_pack_context`
wrapped around one.

The three calls are replaced by two that ask an adapter only for what it alone
knows:

    measure_atlas_glyphs(idx, codes, count, metric_id, sizes);
    render_atlas_glyphs(idx, codes, count, metric_id, atlas, atlas_size, positions, info);

`font_atlas` measures every glyph it is about to add, packs them in one pass,
and then tells each component where to draw. All three adapters already worked
in these two phases internally — FreeType measured with
`FT_LOAD_BITMAP_METRICS_ONLY` before rendering, and stb_truetype publishes
`PackFontRangesGatherRects` / `PackFontRangesRenderIntoRects` around the packing
step precisely so a caller can substitute its own — so each was a matter of
splitting an existing function in two.

`linked_font_file_adapter` reduces to routing a codepoint to the component that
has it, through the same `component_index_for()` dispatch `get_glyph_bitmap()`
was already using. It no longer has any notion of who owns which part of the
buffer.

One guard survives that routing: every component writes into the one buffer the
caller uploads as a single texture, in whatever pixel format it writes, so a
component whose format differs from the canonical's is not allowed into the
atlas and its codepoints fall back to the canonical face. That is not
hypothetical — a gdr ROM face is 8 bits per pixel while an imported TrueType one
is 32, which is exactly the pairing `attach_user_font_fallbacks()` builds on an
S60v2 device.

Two more defects fell out of the rewrite:

- The rebuild path, taken when the atlas fills up, wrote `characters_.size() - 5`
  entries into an array sized `to_rast.size()` — an overflow whenever more
  characters were cached than were being added — and never added the characters
  that triggered the rebuild in the first place. It now refills from the hottest
  cached characters plus the new ones, into a correctly sized array.
- Glyph padding was per-adapter and inconsistent (5 pixels in FreeType, 1 in
  stb, none in gdr). It belongs to whoever packs, so it is now uniform.

Verified on `rm-321`: ZipManager and the Calculator render their Chinese text.
The regression suite (Final Battle, Calculator, N95 Calculator) stays at PASS,
which covers the FreeType path and, through `rm-321`, a linked typeface whose
components share a format.

The gdr path has no suite coverage, so it was checked by hand on the 6680
(`rm-36`), whose ROM fonts are `.gdr`: its Application manager renders bitmap-font
text normally. That app also raises an access violation in its own `appmngr`
thread — stashing this work and rebuilding reproduced the same fault at the same
address, so it is pre-existing and unrelated. The 6680 with an imported font is
also the mismatched-format case above, where the guard reduces the behaviour to
what it was before this work.

The stb path is only reachable from the Qt front-end's button-map overlay. It
compiles and links; there is no automated coverage of it on either side of this
change.

X-Plore switched to Chinese renders correctly on both the N-Gage QD and the N70,
and ZipManager on `rm-321` is unchanged.

## A second bug behind the same symptom

Testing X-Plore switched to Chinese on the two EKA1-era devices turned up a
split that the atlas work does not explain: the N-Gage QD (`nem-4`) rendered its
Chinese fine, while the N70 (`rm-84`) drew nothing at all — not boxes, nothing,
as if the characters had zero width.

Neither device uses the atlas for this. Both draw through the guest's own
`CFbsBitGc`, so every glyph goes through `fbs_rasterize` and
`get_glyph_bitmap()`, whose dispatch was never broken. Instrumenting the
returned bitmaps showed what actually differed:

    nem-4   cjk 0x4f53 w=10 h=9    0x56de w=8 h=8    0x5927 w=10 h=9
    rm-84   cjk 0x4f53 w=1  h=1    0x56de w=1 h=1    0x5927 w=1  h=1

One pixel square. The imported font was being rendered at a size of about two
pixels, which is the shape of a metric identifier reaching FreeType as
something it is not — the very hazard `get_nearest_supported_metric()` builds
its translation table to avoid.

The table was there and it was being hit. The fault was in what it was built
from: every component was asked to translate **the size the client requested**.
A gdr canonical stocks only a handful of fixed sizes and simply takes the
nearest, so it is unbothered by a strange request — the N70's X-Plore asks for a
height of 2 while the canonical happily renders its bitmap font at 13. FreeType,
asked for the same 2, obliges exactly.

A linked typeface has to look like one face, so the fallbacks now translate the
size the canonical **settled on** (`metrics->max_height`) rather than the one
that was asked for. On `nem-4` the two happened to agree, which is why only one
of the two devices ever showed the fault.

That fix immediately hung the emulator, exposing a third defect:
`derive_design_height_from_max_height()` (ported from Symbian's
`FTRAST2.CPP`) walks its height down in a loop with no lower bound.
`FT_Set_Pixel_Sizes` fails at zero and leaves the loop variable unchanged, so it
spins forever. It now stops at one pixel.

Both are older than this work — the translation table and that loop are
untouched by the atlas rewrite.

## Left unbuilt

Where an atlas' components write different pixel formats, the imported font
still cannot join it, so a device drawing UI text through the atlas with a gdr
canonical gains no CJK glyphs from an imported font. Converting a component's
output to the atlas' format during `render_atlas_glyphs` would close that, and
is the natural place for it now that the atlas owns the buffer. Note this is
narrower than it first appears: the EKA1-era devices tested here draw through
the guest rasteriser, not the atlas, and are unaffected.

## Why the ROM's own Chinese fonts do not step in

The obvious question is why `s60sc.ccc` — a 2.3 MB Simplified Chinese face
sitting right next to ZDigi — is not chosen instead. Three separate reasons, and
fixing only the first would have made the screen worse, not better.

**It loses the similarity match by one point, and correctly so.** The ROM's Font
Provider names `Nokia Hindi S60`, which this device does not carry (the Indian
N95 dump, `rm-320`, does), so the name lookup fails and scoring decides. ZDigi
scores 18 to `Sans MT 936_S60`'s 17, and the point is the serif term:
`s60sc.ccc` has `OS/2` panose[1] = 10 (triangle serif) against ZDigi's 11
(normal sans), while the request is for a sans face. That classification is not
ours to second-guess — the official FreeType rasteriser does exactly the same
thing (`fontservices/freetypefontrasteriser/src/FTRAST2.CPP:480`):

    const TInt serif_style = tt_face->os2.panose[1];
    attrib.SetSerif((serif_style >= 2 && serif_style <= 10) || serif_style >= 14);

`CFontStore::MatchFontSpecsInPixels` was re-read against ours at the same time;
the weights and the two-stage name-then-similarity order match. On a device that
has the font the ROM asks for, none of this is ever reached.

**Choosing it anyway renders nothing.** Skipping ZDigi at load time to force the
issue leaves the UI blank rather than boxed. `s60sc.ccc` is a genuine TrueType
file whose `loca` table is compressed: 70738 bytes where 28294 glyphs in long
format need 113180, and the entries read as noise (0, 0, 0, 5632, 41943148, …).
The outlines are Monotype's, and a retail S60 device rasterises them with
Monotype's iType. FreeType — the rasteriser both this emulator and Symbian's own
open-source port use — cannot, and returns empty outlines at every size.

**Its embedded bitmaps do not reach the screen either.** The file does carry
EBDT/EBLC strikes, five of them, 1bpp at 12/14/16/18/20 ppem. Snapping the
requested pixel size onto the nearest strike works (15 → 14, 17 → 16 were
observed), but the text stayed blank: `get_glyph_atlas` renders with
`FT_RENDER_MODE_LCD` and reads the result as three bytes per pixel while
dividing the width by three, so an `FT_PIXEL_MODE_MONO` bitmap is not handled at
all. Making that ROM's own fonts usable needs both the snap and a monochrome
path through the atlas, and even then only five sizes exist, so a UI asking for
15 or 17 pixels would be laid out at 14 or 16.

## Left standing

The similarity match still lands on ZDigi, so Latin text is drawn with a digits
face and its spacing looks off. An imported font remains the only source of
Chinese glyphs on this device.

One real deviation from `MatchFontSpecsInPixels` was found while comparing:
`KFontMatchScoreForBitmapType` (2 points) is dropped here on the grounds that it
scores every candidate identically. That holds while every candidate comes from
FreeType, but not on a device mixing gdr faces (monochrome) with imported
TrueType ones (antialiased). It does not affect this bug.
