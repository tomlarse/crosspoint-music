# Sheet Music Support — Project Plan

**Status**: Phase 1 prototype working and validated on a real march part (2026-07-15) — see [converter/README.md](../../converter/README.md). Synthetic test pieces round-trip at 0.000% ink-pixel mismatch; a real Bb-trumpet part (Under blågul fana, via Audiveris OMR at 300 DPI) harvests at 0.035% with playOrder correctly unrolled for both strains. Every remaining extraction warning is a cross-measure tie/slur — the known Phase-2 split problem. The Phase-4 OMR pipeline (Audiveris) is already spiked and works; see the OMR section in converter/README.md for resolution pitfalls.
**Repo**: This is a standalone fork of [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) dedicated to this prototype. `upstream` remote points at the main project for pulling updates (`git fetch upstream && git merge upstream/develop`). Do NOT open PRs against upstream from this work.

## Vision

Display sheet music on the Xteink X4 the way EPUBs are displayed: the device reflows the score to fit its screen (orientation, zoom level), instead of showing fixed pre-paginated pages. Reading music on e-ink today means panning around a scaled PDF; we want measure-aware reflow.

## Use case (decided 2026-07-15)

**Marching band parts, read on-instrument in the field.** The player always has exactly one voice in front of them. This fixes the scope:

- **Always monophonic, single staff.** Multi-staff (piano/grand staff), multi-voice, and concert scores are OUT of scope for this fork — not deferred, out.
- **Clef is fixed per piece** (G or F), never changes mid-piece.
- **Key changes mid-piece ARE in scope** — trio modulation is standard in marches.
- **Core v1 artifacts**: notes/rests, accidentals, key signatures, time signatures, ties/slurs (incl. cross-barline splits), trills.
- **v1: repeats and endings ("hus") with navigation** (decided 2026-07-15). Marches live on repeated strains with first/second endings, so paging must follow *performance order*: page forward at a repeat and the device jumps back to the right spot, second time through it shows the second ending. The converter unrolls the repeat structure into a `playOrder` list of measure indices (prototyped, see converter/); firmware just pages linearly through it. Repeat barlines/dots and volta brackets render from ordinary primitives. D.C./D.S. al Fine jumps are not unrolled yet — same mechanism, Phase 2.
- **v1.x**: text under notes/staff via the TEXT primitive — drill cues written into the part (and it doubles as lyrics support).
- **Out**: chord symbols, dynamics-driven layout complexity beyond plain glyphs.

## Key insight

Music reflows **per measure**: line breaks happen between measures, exactly like text breaks between words. If every measure arrives pre-engraved as a self-contained unit with a known width, the device only does "word wrapping" with measures as words, plus re-inserting clef/key-signature headers at each system start. That is the same complexity class as the existing EPUB layout engine (`section.bin` pipeline).

## Format decision (researched 2026-07-15)

No existing engine fits on the ESP32-C3 (380KB RAM, single core):

- [Verovio](https://github.com/rism-digital/verovio) — best open-source engraving engine, pure C++20, no deps, but ~1.2MB as JS build; the low-memory "light" variant was discontinued. Too big on-device, **ideal as an offline library**.
- [OpenSheetMusicDisplay](https://github.com/opensheetmusicdisplay/opensheetmusicdisplay) — TypeScript/browser; irrelevant on-device but the best reference for MusicXML→reflow behaviour.
- [abcm2ps](https://manpages.debian.org/testing/abcm2ps/abcm2ps.1.en.html) — C but built as a CLI for PostScript/SVG, not an embeddable lib.
- No prior art found for embedded/microcontroller music reflow. Greenfield.

**Decision**:
- **Ingest**: MusicXML (ecosystem standard — MuseScore/Dorico export it, OMR tools produce it). ABC notation as optional secondary ingest.
- **Heavy lifting offline**: a desktop converter does all engraving.
- **On-device**: a custom measure-based binary format, `.cpmx` (see [cpmx-format-draft.md](cpmx-format-draft.md)); firmware only packs measures into systems/pages and blits glyphs.
- **Glyphs**: [SMuFL](https://w3c.github.io/smufl/latest/) standard, [Bravura](https://github.com/steinbergmedia/bravura) font (SIL OFL), rasterized at a few fixed staff sizes through the existing `lib/EpdFont/` conversion pipeline — same trade-off as text font sizes (no free scaling on e-ink).
- **PDF/image ingest**: offline OMR in the converter pipeline — [Audiveris](https://audiveris.net/) (mature, Java) or [oemer](https://github.com/topics/oemer) (ML) → MusicXML → `.cpmx`. We do not write OMR ourselves.

## Architecture

```
MusicXML / ABC ──┐
                 ├─> converter (desktop) ──> .cpmx ──> SD card ──> firmware viewer activity
PDF/image ─OMR──┘        │
                         └─ uses Verovio as engraving library
```

**Converter approach (Phase 1)**: use Verovio with `--breaks none` (render the whole piece as one endless system), then extract per-measure geometry from its SVG output — Verovio groups elements as `<g class="measure">` and references SMuFL codepoints, so measures, glyph placements, and line segments can be harvested and re-based to measure-local coordinates. Validate by re-rendering measures from the extracted primitives into a preview SVG and comparing against Verovio's own output.

## Phases

1. **Converter prototype** (desktop, no hardware — ~80% of the risk lives here):
   MusicXML in → per-measure primitive dump (JSON first, binary later) → SVG preview re-renderer for visual verification. Start with a simple monophonic melody.
2. **`.cpmx` spec**: ~~freeze v1~~ **done 2026-07-15** — v1 frozen and implemented (converter/cpmx_emit.py + cpmx_render.py reader/simulator); binary round-trips at 0.000% against the JSON pipeline on all test pieces.
3. **Firmware viewer**: new activity + Bravura glyphs via the EpdFont pipeline + measure-packing layout + `.crosspoint` caching. Follow all existing HAL/heap/activity-lifecycle rules in the root CLAUDE.md.
4. **OMR pipeline**: wire Audiveris/oemer in front of the converter for PDF/image input. Spiked early (works, see converter/README.md); what remains is packaging.
5. **Sheet music library app** (vision, 2026-07-25): a desktop "music library" application with conversion and device sync built in — the Calibre + CrossPoint-plugin model, but for scores. Import PDF/MusicXML into the library, conversion (incl. OMR) happens inside the app, and the repertoire syncs to the device over USB/Wi-Fi. Today's CLI converter becomes the app's backend; the firmware's existing wireless transfer endpoints are the sync target. One band member curates, everyone gets the same small `.cpmx` files.

## Known hard problems

- ~~**Slurs/ties across line breaks**~~: **solved in the converter prototype** (2026-07-15). The converter re-renders the piece once with a forced system break before every measure (`<print new-system="yes"/>` + `breaks: encoded`); Verovio then engraves every crossing curve split, and the halves are harvested as `splitAtEnd`/`splitAtStart` variants per measure, aligned to the master render via notehead anchors. The layout stage swaps whole curve ↔ halves depending on where breaks fall. Note: Verovio only draws the *departing* half at a break (no arriving stub in the continuation measure) — we match that convention.
- **System headers**: clef + key signature must be re-inserted at every system start; converter emits them as separate primitive blocks per (clef, key) combination.
- ~~**Multi-staff (piano/grand staff)**~~: no longer a problem — out of scope entirely (see Use case). The format stays single-staff.
- **Zoom**: 2–3 fixed staff sizes (bitmap glyphs), not free scaling.

## Constraints to watch

- **Flash budget**: the default env build is already at **82% flash** (5.4/6.5MB, measured 2026-07-15). Bravura glyph sets at multiple sizes cost flash — measure before/after, consider trimming to the glyph ranges actually used.
  - **Measured (glyph spike, 2026-07-15): flash is a non-issue.** A realistic full marching-band set (240 Bravura glyphs: clefs, time sigs, noteheads, flags, accidentals, articulations, fermatas, rests, dynamics, ornaments, repeats/segno/coda, tuplet digits) through the `lib/EpdFont/` pipeline (2-bit + DEFLATE, same as builtin text fonts) at THREE staff sizes costs **~63 KB** total — ~6% of the ~1.1 MB headroom. The minimal set actually used by the test corpus (51 glyphs) is 16 KB. Conversion: `fontconvert.py <name> <pt> Bravura.otf --2bit --compress --additional-intervals 0xE050,0xE063 ...` where pt = staff-space-px × 4 × 72/150 (SMuFL: em = staff height). Size classes chosen: ss 10/12/16 px → 19/23/31 pt. G clef bitmap verified visually (correct shape and proportions). fontconvert.py needed one fix: symbol-only fonts lack the `'|'` glyph it probes for line metrics.
- **RAM**: 380KB hard ceiling; boot-time usage was 15.6% (51KB) at fork time. Measure primitives must stream from SD, never hold a whole score in RAM.
- Root `CLAUDE.md` rules apply unchanged (HAL only, `makeUniqueNoThrow`, `tr()` for UI strings, no bare `new`, etc.). Its scope philosophy ("dedicated e-reader, not a Swiss Army knife") is overridden here: sheet music IS the mission of this fork.

## Open questions

Decided during Phase 1 (2026-07-15):

- **Converter language**: Python + `verovio` PyPI bindings. Iteration speed won; nothing needed the C++ API.
- **SVG harvesting robustness**: promising but only proven on trivial input. On the monophonic test piece every element inside `<g class="measure">` maps cleanly to one of five primitives (glyph/line/beam/curve/dot), glyph codepoints are recoverable from `<use>` ids, and the re-render pixel-diffs at 0.000% against Verovio's own output. Safety nets in the pipeline: extract.py warns on any unhandled element/transform/path command instead of guessing, and pixeldiff.html quantifies ink mismatch (with a saturation guard). Known gaps to close as coverage grows (codex review 2026-07-15): stroked-vs-filled curve semantics (wedges, brackets), group transforms, non-5-line staves, richer notation (tuplets, dynamics, lyrics, grace notes), and per-glyph bounding boxes for vertical pagination.

- **Repeats/voltas/lyrics/chord symbols** (2026-07-15, use-case decision): repeats + voltas are v1 *with navigation* (see Use case); text under staff (drill cues/lyrics) v1.x via TEXT primitive; chord symbols out.
- **How Verovio emits voltas** (verified 2026-07-15): repeat dots are SMuFL glyph E044 inside the owning measure's barLine group — harvested for free. Volta brackets are `<g class="ending">` *siblings* of measures (milestone pattern) containing 3 straight lines + a bold text label; the converter attaches them to the right measure by bracket midpoint. TEXT primitive implemented for the labels.

Still open:

- **`.cpmx` conformance test** (task, added 2026-07-25 after the RECT size
  bug): the C++ reader is a third implementation of the format and was never
  diffed against the Python reference — a 2-byte skip-table mismatch shipped
  and stalled paging mid-piece. Build a host-side test that decodes every
  test `.cpmx` with `lib/Cpmx/CpmxReader` (native build) and compares
  field-by-field against `cpmx_render.py` output. Must run before any
  format change.
- How ABC ingest enters: Verovio reads ABC natively, so possibly free.
- D.C./D.S. al Fine: extend the unroller with jump directives (Fine, segno, coda) — same playOrder mechanism, needs `<sound>`/direction parsing.
- Volta brackets spanning multiple measures need per-measure splitting (same family as slur splits).
