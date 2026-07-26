#!/usr/bin/env python3
"""Firmware simulator: read a .cpmx v2 binary and render reflowed SVG.

This is the round-trip proof for the format: it parses the binary
independently of the emitter, performs the measure-packing layout the
firmware will do (using only data from the binary: widths, header blocks,
split variants, playOrder), and renders SVG for comparison against
preview.py's JSON-based reflow.

Only the glyph *outlines* are borrowed from the primitives JSON — they are
deliberately not part of .cpmx (firmware has Bravura rasterized per size).
"""

import argparse
import json
import struct
import sys
from pathlib import Path

from cpmx_emit import (COORD_FP, SCALE_FP, MAGIC, VERSION,
                       PRIM_GLYPH, PRIM_LINE, PRIM_BEAM, PRIM_CURVE,
                       PRIM_POLYLINE, PRIM_DOT, PRIM_RECT, PRIM_TEXT,
                       MEASURE_FLAG_SPLIT_START, MEASURE_FLAG_SPLIT_END,
                       TEXT_FLAG_BOLD, TEXT_ANCHOR_SHIFT)
from preview import SvgRenderer, render_measure_at_break, svg_document


MAX_PRIMS_PER_LIST = 4096
MAX_MEASURES = 4096
MAX_PLAY_ORDER = 16384
MAX_TITLE_LEN = 256


class Reader:
    """Bounds-checked reader — models the defensive contract the firmware
    reader must enforce against corrupt/truncated files on the SD card.
    A `limit` bounds reads to one record (a record ends where the next
    offset begins, per the spec's validation rules)."""

    def __init__(self, data, limit=None):
        self.data = data
        self.pos = 0
        self.limit = len(data) if limit is None else limit

    def take(self, fmt):
        size = struct.calcsize("<" + fmt)
        if self.pos + size > self.limit:
            raise ValueError(f"truncated read at offset {self.pos}")
        vals = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += size
        return vals if len(vals) > 1 else vals[0]

    def take_bytes(self, n):
        if self.pos + n > self.limit:
            raise ValueError(f"truncated read at offset {self.pos}")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b


ANCHOR_NAMES = {0: "start", 1: "middle", 2: "end"}


def read_points(r):
    n = r.take("B")
    return [[x / COORD_FP, y / COORD_FP] for x, y in
            (r.take("hh") for _ in range(n))]


def read_primitive(r):
    t = r.take("B")
    if t == PRIM_GLYPH:
        cp, x, y, scale = r.take("HhhH")
        return {"type": "glyph", "codepoint": f"{cp:04X}",
                "x": x / COORD_FP, "y": y / COORD_FP, "scale": scale / SCALE_FP}
    if t == PRIM_LINE:
        x1, y1, x2, y2, w = r.take("hhhhH")
        return {"type": "line", "x1": x1 / COORD_FP, "y1": y1 / COORD_FP,
                "x2": x2 / COORD_FP, "y2": y2 / COORD_FP, "w": w / COORD_FP}
    if t == PRIM_BEAM:
        return {"type": "beam", "points": read_points(r)}
    if t == PRIM_CURVE:
        w = r.take("H")
        return {"type": "curve", "w": w / COORD_FP, "points": read_points(r)}
    if t == PRIM_POLYLINE:
        w = r.take("H")
        return {"type": "polyline", "w": w / COORD_FP, "points": read_points(r)}
    if t == PRIM_DOT:
        x, y, rad = r.take("hhH")
        return {"type": "dot", "x": x / COORD_FP, "y": y / COORD_FP,
                "r": rad / COORD_FP}
    if t == PRIM_RECT:
        x, y, w, h = r.take("hhHH")
        return {"type": "rect", "x": x / COORD_FP, "y": y / COORD_FP,
                "w": w / COORD_FP, "h": h / COORD_FP}
    if t == PRIM_TEXT:
        x, y, size, flags, n = r.take("hhHBB")
        text = r.take_bytes(n).decode("utf-8")
        p = {"type": "text", "x": x / COORD_FP, "y": y / COORD_FP,
             "size": size / COORD_FP, "text": text,
             "anchor": ANCHOR_NAMES.get((flags >> TEXT_ANCHOR_SHIFT) & 0x3, "start")}
        if flags & TEXT_FLAG_BOLD:
            p["weight"] = "bold"
        return p
    raise ValueError(f"unknown primitive tag {t} at offset {r.pos - 1}")


def read_prim_list(r):
    n = r.take("H")
    if n > MAX_PRIMS_PER_LIST:
        raise ValueError(f"primitive list of {n} exceeds ceiling")
    return [read_primitive(r) for _ in range(n)]


def load_cpmx(path):
    data = path.read_bytes()
    r = Reader(data)
    (magic, version, flags, n_measures, n_order, n_blocks, units,
     title_len, composer_len, arranger_len) = r.take("4sBBHHHHHHH")
    if magic != MAGIC:
        sys.exit(f"error: not a CPMX file: {path}")
    if version != VERSION:
        sys.exit(f"error: unsupported CPMX version {version}")
    if flags != 0:
        sys.exit(f"error: nonzero reserved header flags 0x{flags:02x}")
    if n_measures == 0 or n_measures > MAX_MEASURES:
        sys.exit(f"error: measureCount {n_measures} outside [1, {MAX_MEASURES}]")
    if n_blocks == 0 or n_blocks > 255:
        sys.exit(f"error: headerBlockCount {n_blocks} outside [1, 255]")
    if n_order > MAX_PLAY_ORDER or title_len > MAX_TITLE_LEN or \
            composer_len > MAX_TITLE_LEN or arranger_len > MAX_TITLE_LEN:
        sys.exit("error: playOrder or metadata string exceeds ceiling")
    title = r.take_bytes(title_len).decode("utf-8")
    composer = r.take_bytes(composer_len).decode("utf-8")
    arranger = r.take_bytes(arranger_len).decode("utf-8")
    if "\x00" in title or "\x00" in composer or "\x00" in arranger:
        sys.exit("error: NUL byte in metadata string")
    play_order = [r.take("H") for _ in range(n_order)]
    if any(i >= n_measures for i in play_order):
        sys.exit("error: playOrder index out of range")
    offsets = [r.take("I") for _ in range(n_blocks + n_measures)]
    prev = r.pos - 1
    for off in offsets:
        if off <= prev or off >= len(data):
            sys.exit(f"error: offset table not strictly increasing within "
                     f"the file (offset {off})")
        prev = off

    # Per the spec, a record is bounded by the next offset (EOF for the
    # last); reads past that bound are invalid.
    def record_reader(i):
        end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
        rr = Reader(data, limit=end)
        rr.pos = offsets[i]
        return rr

    blocks = []
    for i in range(n_blocks):
        br = record_reader(i)
        advance = br.take("H") / COORD_FP
        blocks.append({"advance": advance, "primitives": read_prim_list(br)})

    measures = []
    for i in range(n_blocks, len(offsets)):
        mr = record_reader(i)
        width, y_min, y_max, header_idx, flags, beats_x8 = mr.take("HhhBBH")
        if header_idx >= n_blocks:
            sys.exit(f"error: systemHeaderIdx {header_idx} >= "
                     f"headerBlockCount {n_blocks}")
        if flags & ~(MEASURE_FLAG_SPLIT_START | MEASURE_FLAG_SPLIT_END):
            sys.exit(f"error: unknown measure flag bits 0x{flags:02x}")
        m = {"width": width / COORD_FP, "yMin": y_min / COORD_FP,
             "yMax": y_max / COORD_FP, "headerIdx": header_idx,
             "beatsX8": beats_x8,
             "primitives": read_prim_list(mr)}
        if flags & MEASURE_FLAG_SPLIT_START:
            m["splitAtStart"] = read_prim_list(mr)
        if flags & MEASURE_FLAG_SPLIT_END:
            m["splitAtEnd"] = read_prim_list(mr)
        measures.append(m)

    return {"title": title, "composer": composer, "arranger": arranger,
            "unitsPerStaffSpace": units,
            "playOrder": play_order or list(range(n_measures)),
            "blocks": blocks, "measures": measures}


def render_reflow(cpmx, glyphs, staff_space, page_width, margin, order=None):
    """The firmware layout: pack measures, headers from SystemHeaderBlocks."""
    doc_like = {"meta": {"unitsPerStaffSpace": cpmx["unitsPerStaffSpace"]},
                "glyphs": glyphs}
    r = SvgRenderer(doc_like, staff_space)
    measures = cpmx["measures"]
    if order is not None:
        measures = [cpmx["measures"][i] for i in order]

    usable = (page_width - 2 * margin) / staff_space
    systems = []
    cur = {"block": None, "entries": []}
    x = 0.0
    for m in measures:
        if cur["entries"] and (x + m["width"]) > usable:
            systems.append(cur)
            block = cpmx["blocks"][m["headerIdx"]]
            cur = {"block": block, "entries": []}
            x = block["advance"]
        cur["entries"].append((m, x))
        x += m["width"]
    systems.append(cur)

    parts = [r.defs()]
    system_gap = 10 * staff_space
    oy = margin + 2 * staff_space
    for si, system in enumerate(systems):
        if system["block"]:
            for p in system["block"]["primitives"]:
                parts.append(r.primitive(p, margin, oy))
            parts.extend(r.staff_lines(margin, oy, system["block"]["advance"]))
        for mi, (m, mx) in enumerate(system["entries"]):
            broken_after = (mi == len(system["entries"]) - 1
                            and si < len(systems) - 1)
            broken_before = mi == 0 and si > 0
            parts.extend(render_measure_at_break(
                r, m, margin + mx * staff_space, oy,
                broken_before, broken_after))
        oy += 4 * staff_space + system_gap

    height = oy - system_gap + 4 * staff_space + margin + 2 * staff_space
    return svg_document("\n".join(parts), page_width, height, "black")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cpmx_file", type=Path)
    ap.add_argument("glyphs_json", type=Path,
                    help="primitives JSON supplying glyph outlines for display")
    ap.add_argument("--staff-space", type=float, default=12.0)
    ap.add_argument("--reflow-width", type=float, default=480.0)
    args = ap.parse_args()

    try:
        cpmx = load_cpmx(args.cpmx_file)
    except (ValueError, UnicodeDecodeError, struct.error) as e:
        sys.exit(f"error: invalid .cpmx file: {e}")
    glyphs = json.loads(args.glyphs_json.read_text())["glyphs"]

    n_prims = sum(len(m["primitives"]) for m in cpmx["measures"])
    print(f"{args.cpmx_file.name}: '{cpmx['title']}', "
          f"{len(cpmx['measures'])} measures, {n_prims} primitives, "
          f"{len(cpmx['blocks'])} header block(s), "
          f"playOrder length {len(cpmx['playOrder'])}")

    svg = render_reflow(cpmx, glyphs, args.staff_space,
                        args.reflow_width, 20.0)
    out = args.cpmx_file.with_suffix(".cpmx.reflow.svg")
    out.write_text(svg)
    print(f"binary reflow -> {out}")

    if cpmx["playOrder"] != list(range(len(cpmx["measures"]))):
        svg = render_reflow(cpmx, glyphs, args.staff_space,
                            args.reflow_width, 20.0, order=cpmx["playOrder"])
        out = args.cpmx_file.with_suffix(".cpmx.unrolled.svg")
        out.write_text(svg)
        print(f"binary unrolled -> {out}")


if __name__ == "__main__":
    main()
