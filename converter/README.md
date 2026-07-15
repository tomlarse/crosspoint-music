# converter/ — desktop MusicXML → measure-primitive prototype

Phase 1 of the sheet-music project (see [docs/music/PLAN.md](../docs/music/PLAN.md)).
Renders MusicXML with [Verovio](https://www.verovio.org/) as one endless
system (`breaks: none`) and harvests its SVG into per-measure primitives —
the data that will later become the `.cpmx` binary format
([docs/music/cpmx-format-draft.md](../docs/music/cpmx-format-draft.md)).

## Setup

```bash
cd converter
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Usage

```bash
# 1. Extract primitives (also dumps Verovio's raw SVG for comparison)
.venv/bin/python extract.py testdata/simple-melody.musicxml
#    -> out/simple-melody.primitives.json
#    -> out/simple-melody.verovio.svg

# 2. Re-render from the primitives
.venv/bin/python preview.py out/simple-melody.primitives.json
#    -> out/simple-melody.rerender.svg   (same layout as Verovio, for diffing)
#    -> out/simple-melody.reflow.svg     (measures re-packed into systems)
#    -> out/simple-melody.compare.html   (side-by-side report)

# 3. Inspect (file:// is blocked in some browsers for the pixel diff's fetch)
python3 -m http.server 8643            # from converter/
# open http://localhost:8643/out/simple-melody.compare.html
# open http://localhost:8643/pixeldiff.html?stem=out/simple-melody
```

## What the pipeline validates

The core risk of the whole project is whether Verovio's SVG can be harvested
robustly. The prototype measures this three ways:

1. **Extraction warnings**: `extract.py` reports every SVG element inside a
   measure that it did not convert into a primitive, every glyph reference it
   could not resolve, and every primitive whose x-extent leaks outside its
   measure. An empty warning list means full coverage of the input.
2. **Pixel diff**: `pixeldiff.html` rasterizes Verovio's original SVG and the
   primitive re-render and counts ink pixels present in only one of them.
   The 8-measure test melody (clef, key/time signature, beams, dotted note,
   accidental, tie, slur, rests, final barline) matches at **0.000%**.
   Scope of that number: it proves the *harvest* is faithful (nothing lost or
   misplaced) on this input. It does not validate firmware rasterization
   (the preview reuses Verovio's own glyph outlines) and says nothing about
   notation the test piece doesn't contain.
3. **Reflow render**: `preview.py` re-packs measures into systems of a given
   width, re-inserting the clef/key-signature header at each system start —
   the exact layout algorithm the firmware will run.

## Primitive model (JSON, precursor of `.cpmx`)

Coordinates are measure-local **staff spaces** (1 unit = gap between staff
lines): x right from the measure's left edge, y **down** from the top staff
line. Staff lines are not stored; the renderer draws them from the measure
width (validates the "firmware draws staff lines" format decision).

| type    | fields                          | source in Verovio SVG            |
|---------|---------------------------------|----------------------------------|
| `glyph` | codepoint, x, y, scale          | `<use xlink:href="#E0A4-...">` (incl. repeat dots E044) |
| `line`  | x1, y1, x2, y2, w               | straight `<path>` (stems, barlines, volta brackets) |
| `beam`  | points[]                        | `<polygon>`                      |
| `curve` | points[] (flattened bezier), w  | `<path>` with `C` segments (ties, slurs) |
| `dot`   | x, y, r                         | `<ellipse>` (augmentation dots)  |
| `text`  | text, x, y, size, anchor, weight | `<text>/<tspan>` (volta labels)  |
| `polyline` | points[], w (stroked, open)  | `<polyline>` (hairpin wedges, tuplet brackets) |
| `rect`  | x, y, w, h (filled)             | `<rect>` (multi-measure rest bars) |

Volta brackets ("hus") are emitted by Verovio as `<g class="ending">`
*siblings* of the measure groups (milestone pattern); the extractor attaches
each bracket to its measure by midpoint. The MusicXML source is also parsed
for repeat barlines and endings, and the repeat structure is unrolled into a
top-level `playOrder` — the performance sequence of measure indices that the
firmware pages through (so "next page" follows the jumps a player makes).
`preview.py` renders this as `<stem>.unrolled.svg`. Per-measure repeat
metadata sits on each measure record under `repeat`.

Each primitive carries the `role` of its enclosing SVG group (`notehead`,
`stem`, `clef`, `keyAccid`, `barLine`, ...) so later stages can build
system-header blocks and slur-split variants without re-parsing.

Glyph outlines harvested from Verovio's `<defs>` are embedded in the JSON so
the preview reproduces exact shapes without needing the Bravura font. The
firmware will instead rasterize Bravura glyphs by codepoint through the
`lib/EpdFont/` pipeline.

## OMR: PDF → MusicXML (Audiveris)

Real parts arrive as PDF; [Audiveris](https://github.com/Audiveris/audiveris)
converts them to MusicXML. Setup (once): download the macOS dmg from the
GitHub releases, copy `Audiveris.app` into `converter/tools/` (gitignored),
`xattr -dr com.apple.quarantine` it.

```bash
# Rasterize the part page yourself — Audiveris caps input at 20 Mpx and
# band parts are often engraved on A2, so crop to the music region to get
# real 300 DPI under the cap:
pdftoppm -r 300 -png -singlefile -x 0 -y 0 -W <w> -H <h> part.pdf out/page
tools/Audiveris.app/Contents/MacOS/Audiveris -batch -export \
    -output out/omr out/page.png
unzip -d out/mxl out/omr/page.mxl
.venv/bin/python extract.py out/mxl/page.xml -o out
```

Lessons from the first real part (Under blågul fana, Bb-trompet 3):

- **Resolution matters**: at 200 DPI Audiveris missed the initial key
  signature (piece read in C instead of F — wrong pitches throughout).
  At 300 DPI (cropped) the key, both repeats and both ending pairs came
  out correct. Always eyeball the result against the PDF.
- Multi-measure rests survive structurally but the count may be misread
  ("32" bars read as 2). Verovio renders an N-bar rest as one measure;
  `parse_repeats` maps MusicXML indices to svg measure indices accordingly.
  **Correcting the count**: edit the MusicXML — set `<multiple-rest>` to the
  true value and duplicate the block's plain rest measures to match (then
  renumber). This is the general shape of OMR correction: fix the MusicXML
  intermediate, never the primitives.
- No OCR languages installed → text directions (TRIO, tempo) are dropped.
  Volta numbers and dynamics still work (they are symbols, not OCR).
- Expect manual correction of OMR output in MuseScore for real use;
  the extractor's warnings + pixel diff catch structural surprises.

The source PDF page and OMR output stay out of git (the composition is
public domain, but the engraving/arrangement may not be).

## Known limitations (fine for Phase 1)

- Monophonic, single staff only (per plan); exactly 5 staff lines assumed
  (percussion/tab staves are skipped with a warning).
- Text (lyrics, tempo, part names) is skipped with a warning.
- Every non-line path becomes a *filled* polygon. Correct for slur/tie
  outlines; wrong for stroked open curves (wedges, brackets, glissandi) —
  those need a fill/stroke distinction when they appear.
- Only absolute M/L/C path commands, translate+scale transforms, and
  uniform glyph scales are accepted; anything else warns instead of parsing
  approximately.
- Reflow borrows the clef/key header from measure 0 — mid-score clef/key
  changes need real SystemHeaderBlocks (Phase 2).
- `yMin`/`yMax` use glyph anchor points, not glyph bounding boxes; proper
  vertical extents need SMuFL/Bravura metrics (Phase 2).
- Cross-measure slurs/ties are not split. Detection: the out-of-bounds
  warning. Generation of split variants needs more than detection — likely
  re-rendering with forced system breaks or reading Verovio's MEI/timemap.
- Repeat unrolling handles single-level repeats with numbered endings
  (the march case). D.C./D.S. al Fine jumps are not unrolled yet; volta
  brackets spanning multiple measures warn instead of splitting.
