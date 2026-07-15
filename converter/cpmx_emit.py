#!/usr/bin/env python3
"""Emit a .cpmx v1 binary from a primitives JSON (extract.py output).

Format spec: docs/music/cpmx-format-draft.md. Key properties:

- little-endian, magic "CPMX", version byte
- streamable: all offsets up front, firmware reads one measure at a time
- coordinates are measure-local staff spaces as i16 fixed point (1/64 ss)
- playOrder (unrolled repeats) in the header — firmware pages through it
- SystemHeaderBlocks: clef + key-signature primitives to draw at each
  continuation-line start, one block per key region; measures reference
  the block in effect at their start via systemHeaderIdx
- split tie/slur variants stored per measure (splitAtStart / splitAtEnd)

Roles are a pipeline concept and are NOT emitted: by emit time they have
been consumed (header blocks built, split variants classified). Glyph
outlines are not emitted either — the firmware rasterizes Bravura by
codepoint; the simulator borrows outlines from the JSON for rendering.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b"CPMX"
VERSION = 1

PRIM_GLYPH = 1
PRIM_LINE = 2
PRIM_BEAM = 3
PRIM_CURVE = 4
PRIM_POLYLINE = 5
PRIM_DOT = 6
PRIM_RECT = 7
PRIM_TEXT = 8

COORD_FP = 64      # staff-space fixed point: 1/64 ss (0.28 px at 18 px/ss)
SCALE_FP = 1024    # glyph scale fixed point

HEADER_ROLES = ("keySig", "keyAccid")

MEASURE_FLAG_SPLIT_START = 0x01
MEASURE_FLAG_SPLIT_END = 0x02

TEXT_FLAG_BOLD = 0x01
TEXT_ANCHOR_SHIFT = 1  # bits 1-2: 0=start 1=middle 2=end
TEXT_ANCHORS = {"start": 0, "middle": 1, "end": 2}


def fp(v):
    n = round(v * COORD_FP)
    if not -32768 <= n <= 32767:
        raise ValueError(f"coordinate {v} out of i16 fixed-point range")
    return n


def ufp(v):
    n = round(v * COORD_FP)
    if not 0 <= n <= 65535:
        raise ValueError(f"unsigned value {v} out of u16 fixed-point range")
    return n


def pack_points(points):
    if len(points) > 255:
        raise ValueError(f"{len(points)} points exceed u8 count")
    out = [struct.pack("<B", len(points))]
    for x, y in points:
        out.append(struct.pack("<hh", fp(x), fp(y)))
    return b"".join(out)


def pack_primitive(p):
    t = p["type"]
    if t == "glyph":
        scale = round(p["scale"] * SCALE_FP)
        if not 0 <= scale <= 65535:
            raise ValueError(f"glyph scale {p['scale']} out of range")
        return struct.pack("<BHhhH", PRIM_GLYPH, int(p["codepoint"], 16),
                           fp(p["x"]), fp(p["y"]), scale)
    if t == "line":
        return struct.pack("<BhhhhH", PRIM_LINE, fp(p["x1"]), fp(p["y1"]),
                           fp(p["x2"]), fp(p["y2"]), ufp(p["w"]))
    if t == "beam":
        return struct.pack("<B", PRIM_BEAM) + pack_points(p["points"])
    if t == "curve":
        return (struct.pack("<BH", PRIM_CURVE, ufp(p.get("w", 0)))
                + pack_points(p["points"]))
    if t == "polyline":
        return (struct.pack("<BH", PRIM_POLYLINE, ufp(p.get("w", 0)))
                + pack_points(p["points"]))
    if t == "dot":
        return struct.pack("<BhhH", PRIM_DOT, fp(p["x"]), fp(p["y"]), ufp(p["r"]))
    if t == "rect":
        return struct.pack("<BhhHH", PRIM_RECT, fp(p["x"]), fp(p["y"]),
                           ufp(p["w"]), ufp(p["h"]))
    if t == "text":
        data = p["text"].encode("utf-8")
        if len(data) > 255:
            raise ValueError("text too long")
        flags = TEXT_FLAG_BOLD if p.get("weight") == "bold" else 0
        flags |= TEXT_ANCHORS.get(p.get("anchor", "start"), 0) << TEXT_ANCHOR_SHIFT
        return (struct.pack("<BhhHBB", PRIM_TEXT, fp(p["x"]), fp(p["y"]),
                            ufp(p["size"]), flags, len(data)) + data)
    raise ValueError(f"unknown primitive type {t!r}")


def pack_prim_list(prims):
    if len(prims) > 4096:
        raise ValueError(f"{len(prims)} primitives exceed the per-list "
                         f"ceiling (4096) — see format validation rules")
    return struct.pack("<H", len(prims)) + b"".join(pack_primitive(p) for p in prims)


def build_header_blocks(measures):
    """One SystemHeaderBlock per key region; per-measure block index.

    Mirrors the reflow logic: the clef comes from measure 0 and never
    changes (use-case decision); a measure carrying a key signature starts
    a new region for the lines that follow it.
    """
    m0p = measures[0]["primitives"]
    clef = [p for p in m0p if p["role"] == "clef"]
    key = [p for p in m0p if p["role"] in HEADER_ROLES]
    clef_x = min((p["x"] for p in clef), default=0.5)
    anchor = min((p["x"] for p in key), default=clef_x + 3.5)

    blocks = [clef + key]
    index = []
    for i, m in enumerate(measures):
        index.append(len(blocks) - 1)
        if i > 0:
            new_key = [p for p in m["primitives"] if p["role"] in HEADER_ROLES]
            if new_key:
                shift = anchor - min(p["x"] for p in new_key)
                key = [{**p, "x": round(p["x"] + shift, 4)} for p in new_key]
                blocks.append(clef + key)
    return blocks, index


def pack_header_block(prims):
    advance = max((p.get("x", 0) for p in prims), default=0) + 2.0
    return struct.pack("<H", ufp(advance)) + pack_prim_list(prims)


def pack_measure(m, header_idx):
    flags = 0
    if m.get("splitAtStart"):
        flags |= MEASURE_FLAG_SPLIT_START
    if m.get("splitAtEnd"):
        flags |= MEASURE_FLAG_SPLIT_END
    out = [struct.pack("<HhhBB", ufp(m["width"]), fp(m["yMin"]), fp(m["yMax"]),
                       header_idx, flags)]
    out.append(pack_prim_list(m["primitives"]))
    if m.get("splitAtStart"):
        out.append(pack_prim_list(m["splitAtStart"]))
    if m.get("splitAtEnd"):
        out.append(pack_prim_list(m["splitAtEnd"]))
    return b"".join(out)


def emit(doc, title):
    measures = doc["measures"]
    if not measures:
        raise ValueError("empty score: .cpmx requires at least one measure")
    play_order = doc.get("playOrder") or []
    if any(not 0 <= i < len(measures) for i in play_order):
        raise ValueError("playOrder index out of range")
    if play_order == list(range(len(measures))):
        play_order = []  # identity: firmware pages linearly

    blocks, header_idx = build_header_blocks(measures)
    if len(blocks) > 255:
        raise ValueError("more than 255 header blocks")

    block_bytes = [pack_header_block(b) for b in blocks]
    measure_bytes = [pack_measure(m, header_idx[i])
                     for i, m in enumerate(measures)]

    title_data = title.encode("utf-8")
    units = round(doc["meta"]["unitsPerStaffSpace"])
    fixed = struct.pack("<4sBBHHHHH", MAGIC, VERSION, 0, len(measures),
                        len(play_order), len(blocks), units, len(title_data))
    pre_offset_size = (len(fixed) + len(title_data) + 2 * len(play_order)
                       + 4 * len(blocks) + 4 * len(measures))

    offsets = []
    pos = pre_offset_size
    for b in block_bytes + measure_bytes:
        offsets.append(pos)
        pos += len(b)

    out = [fixed, title_data]
    out.append(struct.pack(f"<{len(play_order)}H", *play_order))
    out.append(struct.pack(f"<{len(offsets)}I", *offsets))
    out.extend(block_bytes)
    out.extend(measure_bytes)
    return b"".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("json_file", type=Path, help="primitives JSON from extract.py")
    ap.add_argument("-o", "--output", type=Path, help="output .cpmx path")
    ap.add_argument("--title", default=None, help="score title (default: source name)")
    args = ap.parse_args()

    doc = json.loads(args.json_file.read_text())
    title = args.title or doc["meta"]["source"]
    data = emit(doc, title)

    out_path = args.output or args.json_file.with_name(
        args.json_file.name.replace(".primitives.json", "") + ".cpmx")
    out_path.write_bytes(data)
    json_size = args.json_file.stat().st_size
    print(f"{out_path}: {len(data)} bytes "
          f"({len(doc['measures'])} measures, JSON was {json_size} bytes)")


if __name__ == "__main__":
    main()
