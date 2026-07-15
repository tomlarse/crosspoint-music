# `.cpmx` — CrossPoint Music Format v1

**Status**: v1 frozen 2026-07-15, implemented and round-trip-validated in
[converter/cpmx_emit.py](../../converter/cpmx_emit.py) /
[converter/cpmx_render.py](../../converter/cpmx_render.py) (0.000% ink-pixel
mismatch against the JSON pipeline on all test pieces, incl. a real march
part). Modelled on the `book.bin`/`section.bin` conventions in
[file-formats.md](../file-formats.md): little-endian, version byte, bump the
version before any structural change.

## Design goals

- Measures are self-contained pre-engraved units with known widths →
  firmware layout is measure-packing only ("word wrap" with measures as words).
- Streamable from SD: seek table up front, one measure record per read;
  the whole score never sits in RAM. (Real 92-measure march part: 12.7 KB
  total, so individual records are tens to hundreds of bytes.)
- No on-device curve math: split tie/slur variants are pre-engraved by the
  converter (harvested from a forced-break Verovio render).
- Page turns follow **performance order**: the header carries `playOrder`,
  the unrolled repeat structure (hus/voltas resolved). Reading progress =
  position in playOrder.

## Units and types

- Little-endian throughout. Firmware must `memcpy` multi-byte fields —
  records are packed, not aligned (RISC-V faults on unaligned loads,
  see root CLAUDE.md).
- Coordinates: measure-local **staff spaces** (1 unit = gap between two
  adjacent staff lines; a 5-line staff is 4 units tall), x right from the
  measure's left edge, y **down** from the top staff line.
- Fixed point: coords are `i16` (unsigned dims `u16`) in **1/64 staff
  space** (≈0.28 px at 18 px/ss). Glyph scale is `u16` in 1/1024.
- Staff lines are NOT stored; the renderer draws 5 lines from the measure
  width (thickness convention: 0.072 ss ≈ Verovio default).
- Glyph outlines are NOT stored; firmware rasterizes Bravura per size from
  SMuFL codepoints via the `lib/EpdFont/` pipeline.

## Layout

```
Header:
  char[4] magic = "CPMX"
  u8      version = 1
  u8      flags = 0                (reserved)
  u16     measureCount
  u16     playOrderLen             (0 = linear, page measures in order)
  u16     headerBlockCount
  u16     unitsPerStaffSpace       (source-render units, for glyph scaling)
  u16     titleLen
  u8[titleLen]        title        (UTF-8)
  u16[playOrderLen]   playOrder    (measure indices, repeats unrolled)
  u32[headerBlockCount + measureCount] offsets
          (absolute file offsets: header blocks first, then measures)

SystemHeaderBlock:                 (drawn at each continuation-line start)
  u16     advanceWidth             (fixed point 1/64 ss, like all dims)
  PrimitiveList                    (clef + key signature primitives)

MeasureRecord:
  u16     width                    (staff spaces, fixed point)
  i16     yMin, i16 yMax           (vertical extent; glyph anchors only —
                                    proper bboxes need Bravura metrics, v2)
  u8      systemHeaderIdx          (block in effect at measure START: a
                                    measure carrying a key change references
                                    the PREVIOUS block — the new signature
                                    is drawn by the measure's own inline
                                    primitives; the NEXT measure references
                                    the new block)
  u8      flags                    (bit0: splitAtStart list present,
                                    bit1: splitAtEnd list present)
  PrimitiveList                    (default content)
  [PrimitiveList splitAtStart]     (if flags bit0)
  [PrimitiveList splitAtEnd]       (if flags bit1)

PrimitiveList:
  u16     count
  Primitive[count]

Primitive:  u8 tag, then payload:
  1 GLYPH    u16 smuflCodepoint, i16 x, i16 y, u16 scale
             (scale 1/1024, relative to the source render — firmware maps
              it to a rasterized size class: normal/grace/cue)
  2 LINE     i16 x1, y1, x2, y2, u16 thickness      (stems, barlines)
  3 BEAM     u8 n, (i16 x, i16 y)[n]                (filled polygon)
  4 CURVE    u16 thickness, u8 n, points[n]         (filled outline polygon
              w/ rounded stroke: ties/slurs, beziers flattened offline)
  5 POLYLINE u16 thickness, u8 n, points[n]         (stroked open strip:
              hairpin wedges, tuplet brackets)
  6 DOT      i16 x, y, u16 r                        (augmentation dots)
  7 RECT     i16 x, y, u16 w, h                     (multi-measure rest bars)
  8 TEXT     i16 x, y, u16 size, u8 flags, u8 len, utf8[len]
             (flags bit0: bold; bits1-2: anchor 0=start 1=middle 2=end;
              volta labels now, drill cues/lyrics later)
```

## Layout algorithm (firmware contract)

1. Page through `playOrder` (or linearly if empty). Pack measures into
   systems: a measure fits if `x + width <= usableWidth`.
2. At each continuation-line start, draw
   `SystemHeaderBlock[systemHeaderIdx of the line's first measure]`
   (primitives + staff lines over `advanceWidth`), then continue packing.
3. A whole curve that reaches past `width + 0.15 ss` is drawn only when the
   next measure sits on the same line. At a line break, IF the measure has
   a `splitAtEnd` list (flags bit1): skip its crossing whole curves and
   draw `splitAtEnd`; the following measure (line start) additionally draws
   its `splitAtStart` list (flags bit0). If the split list is absent, draw
   the whole curves as-is (graceful degradation — the converter emits
   variants for every crossing curve, so absence means converter fallback,
   not an invalid file). Verovio engraves no arriving stub for broken
   ties, so `splitAtStart` is often absent — matching its convention.
4. Vertical layout (page height, system spacing, margins, systems per
   page) is deliberately NOT part of the format: it is viewer policy.
   `yMin`/`yMax` are the per-measure inputs for the viewer's own vertical
   pagination. The simulator uses a fixed system gap; firmware will pack
   systems into pages using max(yMax)-min(yMin) per system.
5. If a single measure (or header + measure) exceeds `usableWidth`, place
   it alone on the line anyway and let it overflow — never split a measure.

## Validation rules (v1 readers MUST enforce)

Files come off a user's SD card and may be truncated or corrupt; the
firmware reader must fail cleanly (LOG_ERR + refuse to open), never crash.

- `magic == "CPMX"`, `version == 1`; nonzero reserved header flags → reject.
- `measureCount >= 1`; `1 <= headerBlockCount <= 255` (systemHeaderIdx is
  u8, so more blocks than 255 is unaddressable and invalid).
- Offset table: every offset strictly greater than the previous one and
  strictly inside the file. A record is implicitly bounded by the next
  offset (or EOF for the last one); reads past that bound → reject.
- Every multi-byte read bounds-checked against file size (and `memcpy`'d,
  never cast — RISC-V alignment).
- `playOrder[i] < measureCount` for all i.
- `systemHeaderIdx < headerBlockCount` for every measure.
- Unknown primitive tag, unknown measure flag bits (above bit1), or a
  truncated primitive payload → reject.
- Practical ceilings for the 380KB target (reject above, BEFORE
  allocating): measureCount ≤ 4096, playOrderLen ≤ 16384, titleLen ≤ 256,
  primitives per list ≤ 4096, points per primitive ≤ 255 (u8),
  text ≤ 255 bytes (u8), single record ≤ 32 KB.

The converter's reader/simulator (`cpmx_render.py`) implements this same
contract so corrupt-file behavior can be exercised on the desktop.

## Rendering policy left to the viewer (v1)

- GLYPH `scale` → rasterized size-class mapping (normal/grace/cue
  thresholds) is defined by the viewer when the Bravura/EpdFont spike
  lands; scale is stored losslessly so the mapping can evolve.
- TEXT: `y` is the alphabetic baseline; font choice and exact metrics are
  viewer policy (device UI serif). Labels are short ASCII/Latin-1-ish
  UTF-8 (volta numbers, later drill cues); the viewer may substitute
  glyphs it cannot render.

## Deliberately not in v1

- Repeat/volta *metadata* (forward/backward/times): consumed offline when
  the converter unrolls into playOrder; firmware never interprets repeats.
- Roles (notehead/stem/clef/...): pipeline-internal, consumed at emit time.
- Multi-staff: out of scope for the fork (marching-band single parts).
- D.C./D.S. al Fine: extend the converter's unroller (same playOrder
  mechanism); no format change expected.
- Glyph bounding boxes for exact vertical pagination (yMin/yMax are
  anchor-based); needs Bravura metrics in the converter, additive change.

## Open questions

- Where it lives on SD: alongside the source file, or under `.crosspoint/`
  like other caches? (It's a conversion artifact, not a cache — leaning
  alongside.)
- Multiple staff-size classes for GLYPH scale → firmware size-class mapping
  table. Glyph spike landed (see PLAN.md Constraints): three classes at
  staff space 10/12/16 px, ~63 KB flash for a full marching-band glyph
  set. The mapping table itself is Phase-3 firmware work.
