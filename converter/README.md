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
