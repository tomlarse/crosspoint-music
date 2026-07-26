#!/usr/bin/env python3
"""Dump a .cpmx file as canonical JSON for conformance testing.

Reuses the reference reader (cpmx_render.load_cpmx) and emits every decoded
value as raw fixed-point integers, in a shape that the C++ dump tool
(test/conformance/cpmx_dump_main.cpp) reproduces exactly. The two outputs
are compared structurally by test/conformance/run_conformance.py.

Text and titles are emitted as hex-encoded UTF-8 bytes so no JSON string
escaping differences can creep in. Exit code 1 on any invalid file.
"""

import json
import struct
import sys
from pathlib import Path

from cpmx_emit import COORD_FP, SCALE_FP
from cpmx_render import load_cpmx

ANCHOR_CODES = {"start": 0, "middle": 1, "end": 2}


def fp(v):
    n = round(v * COORD_FP)
    assert abs(n - v * COORD_FP) < 1e-6, v
    return n


def dump_prim(p):
    t = p["type"]
    if t == "glyph":
        return {"t": 1, "cp": int(p["codepoint"], 16), "x": fp(p["x"]), "y": fp(p["y"]),
                "s": round(p["scale"] * SCALE_FP)}
    if t == "line":
        return {"t": 2, "x1": fp(p["x1"]), "y1": fp(p["y1"]),
                "x2": fp(p["x2"]), "y2": fp(p["y2"]), "w": fp(p["w"])}
    if t == "beam":
        return {"t": 3, "pts": [[fp(x), fp(y)] for x, y in p["points"]]}
    if t == "curve":
        return {"t": 4, "w": fp(p["w"]), "pts": [[fp(x), fp(y)] for x, y in p["points"]]}
    if t == "polyline":
        return {"t": 5, "w": fp(p["w"]), "pts": [[fp(x), fp(y)] for x, y in p["points"]]}
    if t == "dot":
        return {"t": 6, "x": fp(p["x"]), "y": fp(p["y"]), "r": fp(p["r"])}
    if t == "rect":
        return {"t": 7, "x": fp(p["x"]), "y": fp(p["y"]), "w": fp(p["w"]), "h": fp(p["h"])}
    if t == "text":
        flags = (1 if p.get("weight") == "bold" else 0) | (ANCHOR_CODES[p["anchor"]] << 1)
        return {"t": 8, "x": fp(p["x"]), "y": fp(p["y"]), "size": fp(p["size"]),
                "flags": flags, "text": p["text"].encode("utf-8").hex()}
    raise ValueError(f"unknown primitive type {t!r}")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: cpmx_dump.py <file.cpmx>")
    try:
        cpmx = load_cpmx(Path(sys.argv[1]))
    except (ValueError, UnicodeDecodeError, struct.error) as e:
        sys.exit(f"error: invalid .cpmx file: {e}")

    doc = {
        "title": cpmx["title"].encode("utf-8").hex(),
        "composer": cpmx["composer"].encode("utf-8").hex(),
        "arranger": cpmx["arranger"].encode("utf-8").hex(),
        "units": cpmx["unitsPerStaffSpace"],
        "playOrder": cpmx["playOrder"],
        "blocks": [{"advance": fp(b["advance"]),
                    "prims": [dump_prim(p) for p in b["primitives"]]}
                   for b in cpmx["blocks"]],
        "measures": [{"width": fp(m["width"]),
                      "yMin": fp(m["yMin"]), "yMax": fp(m["yMax"]),
                      "headerIdx": m["headerIdx"], "beatsX8": m["beatsX8"],
                      "prims": [dump_prim(p) for p in m["primitives"]],
                      "splitStart": [dump_prim(p) for p in m.get("splitAtStart", [])],
                      "splitEnd": [dump_prim(p) for p in m.get("splitAtEnd", [])]}
                     for m in cpmx["measures"]],
    }
    json.dump(doc, sys.stdout, separators=(",", ":"))
    print()


if __name__ == "__main__":
    main()
