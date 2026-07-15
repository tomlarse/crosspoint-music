# Sheet Music Support — Project Plan

**Status**: Planning complete, Phase 1 not started (2026-07-15)
**Repo**: This is a standalone fork of [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) dedicated to this prototype. `upstream` remote points at the main project for pulling updates (`git fetch upstream && git merge upstream/develop`). Do NOT open PRs against upstream from this work.

## Vision

Display sheet music on the Xteink X4 the way EPUBs are displayed: the device reflows the score to fit its screen (orientation, zoom level), instead of showing fixed pre-paginated pages. Reading music on e-ink today means panning around a scaled PDF; we want measure-aware reflow.

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
2. **`.cpmx` spec**: freeze v1 of the binary format (version field from day one, like `section.bin`). Convert the JSON dump to binary emit.
3. **Firmware viewer**: new activity + Bravura glyphs via the EpdFont pipeline + measure-packing layout + `.crosspoint` caching. Follow all existing HAL/heap/activity-lifecycle rules in the root CLAUDE.md.
4. **OMR pipeline**: wire Audiveris/oemer in front of the converter for PDF/image input.

## Known hard problems

- **Slurs/ties across line breaks**: when a measure lands at a system start/end the curve must be redrawn split. Plan: converter pre-generates split variants ("whole", "split-at-start", "split-at-end") per affected measure so firmware does no curve geometry.
- **System headers**: clef + key signature must be re-inserted at every system start; converter emits them as separate primitive blocks per (clef, key) combination.
- **Multi-staff (piano/grand staff)**: measure widths must sync across staves in a system. **v1 is monophonic, single staff.**
- **Zoom**: 2–3 fixed staff sizes (bitmap glyphs), not free scaling.

## Constraints to watch

- **Flash budget**: the default env build is already at **82% flash** (5.4/6.5MB, measured 2026-07-15). Bravura glyph sets at multiple sizes cost flash — measure before/after, consider trimming to the glyph ranges actually used.
- **RAM**: 380KB hard ceiling; boot-time usage was 15.6% (51KB) at fork time. Measure primitives must stream from SD, never hold a whole score in RAM.
- Root `CLAUDE.md` rules apply unchanged (HAL only, `makeUniqueNoThrow`, `tr()` for UI strings, no bare `new`, etc.). Its scope philosophy ("dedicated e-reader, not a Swiss Army knife") is overridden here: sheet music IS the mission of this fork.

## Open questions (decide during Phase 1)

- Converter language: Python + `verovio` PyPI bindings (fastest iteration) vs C++ linking Verovio directly. Leaning Python for the prototype.
- Is SVG harvesting robust enough, or do we need to hook Verovio's internal object model (its C++ API exposes the layout tree)?
- How ABC ingest enters: Verovio reads ABC natively, so possibly free.
- Repeats/voltas, lyrics, chord symbols: which make v1?
