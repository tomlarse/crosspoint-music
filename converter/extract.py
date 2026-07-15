#!/usr/bin/env python3
"""Extract per-measure engraving primitives from Verovio SVG output.

Phase 1 prototype for the .cpmx pipeline (see docs/music/PLAN.md).

Renders a MusicXML file with Verovio using `breaks: none` (one endless
system), then harvests the SVG into measure-local primitives:

  glyph  - SMuFL glyph reference (codepoint, position, scale)
  line   - straight line segment (stems, barlines)
  beam   - filled polygon (beams)
  curve  - flattened bezier outline as polygon points (slurs, ties)
  dot    - filled circle (augmentation dots)

Coordinates are measure-local staff-space units (1 unit = distance between
adjacent staff lines), x from the measure's left edge, y DOWN from the top
staff line. Staff lines themselves are NOT emitted as primitives: they are
validated to span the measure exactly and are meant to be drawn by the
renderer from the measure width alone.

Output is a single JSON file that also embeds the SVG glyph outlines
harvested from Verovio's <defs>, so a preview renderer can reproduce the
exact glyph shapes without the Bravura font. Anything the parser does not
recognize is reported in `warnings` — that list is the robustness signal
this prototype exists to measure.
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"

VEROVIO_OPTIONS = {
    "breaks": "none",          # one endless system: no layout decisions by Verovio
    "adjustPageWidth": True,
    "adjustPageHeight": True,
    "header": "none",
    "footer": "none",
    "scale": 100,
}

# Number of line segments used to flatten one cubic bezier span.
BEZIER_SEGMENTS = 8

TRANSLATE_RE = re.compile(r"translate\(\s*(-?[\d.]+)[,\s]+(-?[\d.]+)\s*\)")
SCALE_RE = re.compile(r"scale\(\s*(-?[\d.]+)(?:[,\s]+(-?[\d.]+))?\s*\)")
# A path that is a single straight segment: "M x1 y1 L x2 y2"
LINE_D_RE = re.compile(
    r"^\s*M\s*(-?[\d.]+)[,\s]+(-?[\d.]+)\s*L\s*(-?[\d.]+)[,\s]+(-?[\d.]+)\s*$"
)
NUMBER_RE = re.compile(r"-?[\d.]+")


def tag_name(el):
    return el.tag.rsplit("}", 1)[-1]


def render_with_verovio(musicxml_path):
    import verovio

    tk = verovio.toolkit()
    tk.setOptions(VEROVIO_OPTIONS)
    if not tk.loadFile(str(musicxml_path)):
        sys.exit(f"error: Verovio could not load {musicxml_path}")
    page_count = tk.getPageCount()
    if page_count != 1:
        sys.exit(f"error: expected 1 page with breaks=none, got {page_count}")
    return tk.renderToSVG(1), tk.getVersion()


def parse_glyph_defs(root):
    """Map SMuFL codepoint (hex string, e.g. 'E0A4') -> glyph outline."""
    glyphs = {}
    defs = root.find(f"{{{SVG_NS}}}defs")
    if defs is None:
        return glyphs
    for g in defs:
        gid = g.get("id", "")
        codepoint = gid.split("-", 1)[0]
        path = g.find(f"{{{SVG_NS}}}path")
        if not codepoint or path is None:
            continue
        glyphs[codepoint] = {
            "d": path.get("d", ""),
            "transform": path.get("transform", ""),
        }
    return glyphs


def parse_transform(transform):
    """Return (tx, ty, sx, sy, residue) from an SVG transform string.

    Only `translate` followed by optional `scale` is understood — that is all
    Verovio emits inside measures today. Anything else (rotate, matrix,
    nesting, other ordering) comes back in `residue` so the caller can warn
    instead of silently mis-placing the primitive.
    """
    tx = ty = 0.0
    sx = sy = 1.0
    t = transform or ""
    m = TRANSLATE_RE.search(t)
    if m:
        tx, ty = float(m.group(1)), float(m.group(2))
    m = SCALE_RE.search(t)
    if m:
        sx = float(m.group(1))
        sy = float(m.group(2)) if m.group(2) is not None else sx
    residue = SCALE_RE.sub("", TRANSLATE_RE.sub("", t)).strip()
    return tx, ty, sx, sy, residue


def flatten_path(d, warnings, ctx):
    """Flatten an SVG path of absolute M/L/C commands into a point list.

    Verovio only emits absolute M/L/C(/Z) inside measures today. Everything
    else — relative commands, H/V/S/Q/A, implicit repeated coordinates —
    is rejected with a warning rather than parsed approximately, so new
    Verovio output surfaces as a robustness signal instead of bad geometry.
    """
    tokens = re.findall(r"[A-Za-z]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", d)
    points = []
    pos = 0
    cur = None

    def take(n):
        nonlocal pos
        if pos + n > len(tokens):
            raise ValueError("path ended mid-command")
        vals = [float(t) for t in tokens[pos : pos + n]]
        pos += n
        return vals

    try:
        while pos < len(tokens):
            cmd = tokens[pos]
            pos += 1
            if cmd in ("M", "L"):
                x, y = take(2)
                cur = (x, y)
                points.append(cur)
            elif cmd == "C":
                if cur is None:
                    raise ValueError("C command before any M")
                x1, y1, x2, y2, x3, y3 = take(6)
                x0, y0 = cur
                for i in range(1, BEZIER_SEGMENTS + 1):
                    t = i / BEZIER_SEGMENTS
                    mt = 1 - t
                    bx = (mt**3) * x0 + 3 * (mt**2) * t * x1 + 3 * mt * (t**2) * x2 + (t**3) * x3
                    by = (mt**3) * y0 + 3 * (mt**2) * t * y1 + 3 * mt * (t**2) * y2 + (t**3) * y3
                    points.append((bx, by))
                cur = (x3, y3)
            elif cmd in ("Z", "z"):
                pass
            else:
                raise ValueError(f"unsupported path command {cmd!r}")
    except ValueError as e:
        warnings.append(f"{ctx}: {e} in d={d[:60]}...")
        return None
    return points


def rebase_prims(prims, x0, top, staff_space):
    """Rebase page-unit primitives to measure-local staff spaces."""

    def sx(v):
        return round((v - x0) / staff_space, 4)

    def sy(v):
        return round((v - top) / staff_space, 4)

    def su(v):
        return round(v / staff_space, 4)

    rebased = []
    for p in prims:
        q = {"type": p["type"], "role": p["role"]}
        if p["type"] == "glyph":
            q.update(codepoint=p["codepoint"], x=sx(p["x"]), y=sy(p["y"]),
                     scale=p["scale"])
        elif p["type"] == "line":
            q.update(x1=sx(p["x1"]), y1=sy(p["y1"]),
                     x2=sx(p["x2"]), y2=sy(p["y2"]), w=su(p["w"]))
        elif p["type"] in ("beam", "curve"):
            q["points"] = [[sx(x), sy(y)] for x, y in p["points"]]
            if "w" in p:
                q["w"] = su(p["w"])
        elif p["type"] == "dot":
            q.update(x=sx(p["x"]), y=sy(p["y"]), r=su(p["r"]))
        elif p["type"] == "text":
            q.update(text=p["text"], x=sx(p["x"]), y=sy(p["y"]),
                     size=su(p["size"]), anchor=p["anchor"])
            if p.get("weight"):
                q["weight"] = p["weight"]
        rebased.append(q)
    return rebased


def apply_vertical_extents(measures):
    """Set yMin/yMax per measure for system spacing/pagination.

    Glyphs contribute only their anchor point — real bounding boxes need
    SMuFL font metrics (Bravura metadata), which is Phase 2 work.
    """
    for m in measures:
        ys = [0.0, 4.0]
        for p in m["primitives"]:
            if p["type"] in ("glyph", "dot", "text"):
                ys.append(p["y"])
            elif p["type"] == "line":
                ys.extend((p["y1"], p["y2"]))
            elif p["type"] in ("beam", "curve"):
                ys.extend(pt[1] for pt in p["points"])
        m["yMin"] = round(min(ys), 4)
        m["yMax"] = round(max(ys), 4)


class MeasureExtractor:
    def __init__(self, glyphs):
        self.glyphs = glyphs
        self.warnings = []

    def extract(self, measure_el, index):
        staff_el = measure_el.find(f"{{{SVG_NS}}}g[@class='staff']")
        if staff_el is None:
            self.warnings.append(f"measure {index}: no staff group, skipped")
            return None

        staff_lines = self._staff_lines(staff_el, index)
        if staff_lines is None:
            return None
        x0, x1, top, staff_space, line_paths = staff_lines

        prims = []
        ctx = f"measure {index}"
        for child in measure_el:
            self._walk(child, None, prims, ctx, skip_paths=line_paths)

        rebased = rebase_prims(prims, x0, top, staff_space)
        width = round((x1 - x0) / staff_space, 4)
        self._check_bounds(rebased, width, index)

        return {
            "index": index,
            "sourceX": x0,
            "sourceY": top,
            "width": width,
            "primitives": rebased,
        }

    def _staff_lines(self, staff_el, index):
        """Validate the 5 staff lines and derive measure geometry from them."""
        lines = []
        line_paths = set()
        for path in staff_el.findall(f"{{{SVG_NS}}}path"):
            m = LINE_D_RE.match(path.get("d", ""))
            if m:
                px1, py1, px2, py2 = (float(v) for v in m.groups())
                if py1 == py2:
                    lines.append((px1, px2, py1))
                    line_paths.add(id(path))
        if len(lines) != 5:
            self.warnings.append(
                f"measure {index}: expected 5 staff lines, found {len(lines)}, skipped")
            return None
        ys = sorted(y for _, _, y in lines)
        gaps = [round(b - a, 3) for a, b in zip(ys, ys[1:])]
        if len(set(gaps)) != 1:
            self.warnings.append(f"measure {index}: uneven staff line gaps {gaps}")
        x0 = min(l[0] for l in lines)
        x1 = max(l[1] for l in lines)
        if any(l[0] != x0 or l[1] != x1 for l in lines):
            self.warnings.append(f"measure {index}: staff lines have uneven extents")
        return x0, x1, ys[0], gaps[0], line_paths

    def _walk(self, el, role, prims, ctx, skip_paths):
        tag = tag_name(el)
        if tag == "g":
            # Group transforms are not composed into child coordinates; if
            # Verovio ever emits one inside a measure, positions would shift.
            if el.get("transform"):
                self.warnings.append(
                    f"{ctx}: <g class={el.get('class')!r}> has a transform "
                    f"({el.get('transform')!r}) which is not applied")
            cls = el.get("class")
            for child in el:
                self._walk(child, cls or role, prims, ctx, skip_paths)
            return
        if id(el) in skip_paths:
            return

        if tag == "use":
            href = el.get(f"{{{XLINK_NS}}}href") or el.get("href") or ""
            codepoint = href.lstrip("#").split("-", 1)[0]
            if codepoint not in self.glyphs:
                self.warnings.append(f"{ctx}: <use> references unknown glyph {href!r}")
                return
            tx, ty, sx_, sy_, residue = parse_transform(el.get("transform"))
            if residue:
                self.warnings.append(
                    f"{ctx}: unsupported transform content {residue!r} on <use>")
            if sx_ != sy_:
                self.warnings.append(f"{ctx}: non-uniform glyph scale {sx_}x{sy_}")
            # <use> may also position via x/y attributes (post-transform).
            tx += float(el.get("x", 0)) * sx_
            ty += float(el.get("y", 0)) * sy_
            prims.append({"type": "glyph", "role": role, "codepoint": codepoint,
                          "x": tx, "y": ty, "scale": sx_})
        elif tag == "path":
            d = el.get("d", "")
            w = float(el.get("stroke-width", 0))
            m = LINE_D_RE.match(d)
            if m:
                px1, py1, px2, py2 = (float(v) for v in m.groups())
                prims.append({"type": "line", "role": role,
                              "x1": px1, "y1": py1, "x2": px2, "y2": py2, "w": w})
            else:
                points = flatten_path(d, self.warnings, ctx)
                if points:
                    prims.append({"type": "curve", "role": role,
                                  "points": points, "w": w})
        elif tag == "polygon":
            pts = [float(v) for v in NUMBER_RE.findall(el.get("points", ""))]
            prims.append({"type": "beam", "role": role,
                          "points": list(zip(pts[0::2], pts[1::2]))})
        elif tag == "ellipse":
            rx, ry = float(el.get("rx", 0)), float(el.get("ry", 0))
            if rx != ry:
                self.warnings.append(f"{ctx}: non-circular ellipse rx={rx} ry={ry}")
            prims.append({"type": "dot", "role": role,
                          "x": float(el.get("cx", 0)), "y": float(el.get("cy", 0)),
                          "r": rx})
        elif tag == "text":
            label = "".join(el.itertext()).strip()
            if not label:
                return
            if el.get("x") is None:
                self.warnings.append(f"{ctx}: text {label!r} without coordinates")
                return
            # Verovio puts font-size 0 on <text> and the real size on an
            # inner <tspan>.
            size = 0.0
            for tspan in el.iter(f"{{{SVG_NS}}}tspan"):
                fs = (tspan.get("font-size") or "").rstrip("px")
                if fs and float(fs) > 0:
                    size = float(fs)
                    break
            if size == 0.0:
                self.warnings.append(f"{ctx}: text {label!r} has no font size")
                return
            prims.append({"type": "text", "role": role, "text": label,
                          "x": float(el.get("x")), "y": float(el.get("y", 0)),
                          "size": size,
                          "anchor": el.get("text-anchor", "start")})
        else:
            self.warnings.append(f"{ctx}: unhandled element <{tag} class={el.get('class')!r}>")

    def _check_bounds(self, prims, width, index):
        """Flag primitives that leak outside their measure (slack: 1 staff space).

        Curves get a much tighter check: a tie/slur reaching past the barline
        is exactly the cross-measure case that needs split variants in .cpmx,
        so it must surface even though it fits within the general slack.
        """
        for p in prims:
            if p["type"] == "curve":
                xs = [pt[0] for pt in p["points"]]
                if min(xs) < -0.15 or max(xs) > width + 0.15:
                    self.warnings.append(
                        f"measure {index}: {p['role']} curve crosses the measure "
                        f"boundary (x=[{min(xs):.2f}, {max(xs):.2f}], width={width:.2f}) "
                        f"— needs split variants (Phase 2)")
        slack = 1.0
        for p in prims:
            xs = []
            if p["type"] in ("glyph", "text"):
                xs = [p["x"]]
            elif p["type"] == "line":
                xs = [p["x1"], p["x2"]]
            elif p["type"] in ("beam", "curve"):
                xs = [pt[0] for pt in p["points"]]
            elif p["type"] == "dot":
                xs = [p["x"]]
            if xs and (min(xs) < -slack or max(xs) > width + slack):
                self.warnings.append(
                    f"measure {index}: {p['type']} ({p['role']}) outside bounds "
                    f"x=[{min(xs):.1f}, {max(xs):.1f}], width={width:.1f}")


def attach_endings(root, extractor, measures, staff_space):
    """Harvest volta brackets ("hus") and attach them to their measures.

    Verovio emits `<g class="ending ...">` as a *sibling* of measure groups
    (milestone pattern), not a child, so the main per-measure walk never sees
    them. Each ending holds a voltaBracket group (3 lines + a number label);
    we rebase it into the measure it starts over.
    """
    for g in root.iter(f"{{{SVG_NS}}}g"):
        if "ending" not in (g.get("class") or "").split():
            continue
        raw = []
        for child in g:
            extractor._walk(child, "voltaBracket", raw, "ending", skip_paths=set())
        if not raw:
            continue
        # Verovio's stylesheet renders ending labels bold (g.ending rule).
        for p in raw:
            if p["type"] == "text":
                p["weight"] = "bold"
        xs = []
        for p in raw:
            if p["type"] == "line":
                xs.extend((p["x1"], p["x2"]))
            elif p["type"] == "text":
                xs.append(p["x"])
        bx0, bx1 = min(xs), max(xs)
        # Attach by bracket midpoint: brackets start exactly on the barline,
        # a hair left of the measure's own staff start, so edge-matching is
        # ambiguous but the midpoint lands inside the right measure.
        mid = (bx0 + bx1) / 2
        target = None
        for m in measures:
            m_x1 = m["sourceX"] + m["width"] * staff_space
            if m["sourceX"] <= mid < m_x1:
                target = m
                break
        if target is None:
            extractor.warnings.append(
                f"ending bracket at x={bx0:.0f} matches no measure, dropped")
            continue
        t_x1 = target["sourceX"] + target["width"] * staff_space
        if bx1 > t_x1 + 0.15 * staff_space or bx0 < target["sourceX"] - 0.25 * staff_space:
            extractor.warnings.append(
                f"measure {target['index']}: volta bracket spans beyond the "
                f"measure — cross-measure bracket needs splitting (Phase 2)")
        target["primitives"].extend(
            rebase_prims(raw, target["sourceX"], target["sourceY"], staff_space))


def parse_repeats(musicxml_path, measure_count, warnings):
    """Read repeat barlines and endings ("hus") from the MusicXML source.

    Returns (per_measure, play_order): per_measure maps index -> repeat
    metadata; play_order is the unrolled performance sequence of measure
    indices — what the firmware pages through so "next page" always shows
    what the player performs next.
    """
    part = ET.parse(musicxml_path).getroot().find("part")
    if part is None:
        warnings.append("MusicXML: no <part>, playOrder skipped")
        return {}, None
    mx_measures = part.findall("measure")
    if len(mx_measures) != measure_count:
        warnings.append(
            f"MusicXML has {len(mx_measures)} measures but the SVG produced "
            f"{measure_count}; playOrder skipped")
        return {}, None

    fwd = {}
    back_times = {}
    volta = {}
    active = None
    for i, m in enumerate(mx_measures):
        starts, stops = set(), False
        for bl in m.findall("barline"):
            rep = bl.find("repeat")
            if rep is not None:
                if rep.get("direction") == "forward":
                    fwd[i] = True
                elif rep.get("direction") == "backward":
                    back_times[i] = int(rep.get("times") or 2)
            end = bl.find("ending")
            if end is not None:
                nums = {int(n) for n in re.findall(r"\d+", end.get("number", ""))}
                if end.get("type") == "start":
                    starts = nums
                elif end.get("type") in ("stop", "discontinue"):
                    stops = True
        if starts:
            active = starts
        if active:
            volta[i] = sorted(active)
        if stops:
            active = None

    per_measure = {}
    for i in range(measure_count):
        info = {}
        if fwd.get(i):
            info["repeatForward"] = True
        if i in back_times:
            info["repeatBackwardTimes"] = back_times[i]
        if i in volta:
            info["volta"] = volta[i]
        if info:
            per_measure[i] = info

    play_order = unroll_repeats(measure_count, fwd, back_times, volta)
    if play_order is None:
        warnings.append("repeat structure did not converge, playOrder skipped")
    return per_measure, play_order


def unroll_repeats(n, fwd, back_times, volta):
    """Unroll single-level repeats with endings into performance order.

    Marches don't nest repeats; D.C./D.S. jumps are not handled yet.
    """
    order = []
    i = 0
    repeat_start = 0
    pass_no = 1
    steps = 0
    while i < n:
        steps += 1
        if steps > 10 * n + 10:
            return None
        nums = volta.get(i)
        if nums and pass_no not in nums:
            i += 1
            continue
        if fwd.get(i) and i != repeat_start:
            repeat_start = i
            pass_no = 1
        order.append(i)
        times = back_times.get(i)
        if times and pass_no < times:
            pass_no += 1
            i = repeat_start
            continue
        if times:
            pass_no = 1
            repeat_start = i + 1
        i += 1
    return order


def extract(musicxml_path, out_dir):
    svg_text, verovio_version = render_with_verovio(musicxml_path)

    out_dir.mkdir(parents=True, exist_ok=True)
    stem = musicxml_path.stem
    raw_svg_path = out_dir / f"{stem}.verovio.svg"
    raw_svg_path.write_text(svg_text)

    root = ET.fromstring(svg_text)
    glyphs = parse_glyph_defs(root)

    extractor = MeasureExtractor(glyphs)
    measures = []
    for measure_el in root.iter(f"{{{SVG_NS}}}g"):
        if measure_el.get("class") != "measure":
            continue
        record = extractor.extract(measure_el, len(measures))
        if record:
            measures.append(record)

    # Staff space in page units is constant for the whole render; the meta
    # block records it so a preview can rescale glyph transforms.
    staff_space = _first_staff_space(root)

    attach_endings(root, extractor, measures, staff_space)
    apply_vertical_extents(measures)

    repeat_info, play_order = parse_repeats(
        musicxml_path, len(measures), extractor.warnings)
    for i, info in repeat_info.items():
        measures[i]["repeat"] = info

    doc = {
        "meta": {
            "source": musicxml_path.name,
            "verovioVersion": verovio_version,
            "verovioOptions": VEROVIO_OPTIONS,
            "unitsPerStaffSpace": staff_space,
            "coordinates": "measure-local staff spaces, x right from measure left "
                           "edge, y down from top staff line",
        },
        "playOrder": play_order,
        "measures": measures,
        "glyphs": glyphs,
        "warnings": extractor.warnings,
    }

    json_path = out_dir / f"{stem}.primitives.json"
    json_path.write_text(json.dumps(doc, indent=1))

    counts = {}
    for m in measures:
        for p in m["primitives"]:
            counts[p["type"]] = counts.get(p["type"], 0) + 1
    print(f"{musicxml_path.name}: {len(measures)} measures, "
          f"{sum(counts.values())} primitives {counts}")
    if play_order is not None and play_order != list(range(len(measures))):
        print(f"  play order (unrolled repeats): "
              f"{' '.join(str(i + 1) for i in play_order)}")
    print(f"  raw SVG   -> {raw_svg_path}")
    print(f"  extracted -> {json_path}")
    if extractor.warnings:
        print(f"  {len(extractor.warnings)} warning(s):")
        for w in extractor.warnings:
            print(f"    - {w}")
    else:
        print("  no warnings — every SVG element in every measure was harvested")
    return json_path


def _first_staff_space(root):
    for staff_el in root.iter(f"{{{SVG_NS}}}g"):
        if staff_el.get("class") != "staff":
            continue
        ys = []
        for path in staff_el.findall(f"{{{SVG_NS}}}path"):
            m = LINE_D_RE.match(path.get("d", ""))
            if m and m.group(2) == m.group(4):
                ys.append(float(m.group(2)))
        if len(ys) == 5:
            ys.sort()
            return round(ys[1] - ys[0], 4)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("musicxml", type=Path, help="input MusicXML file")
    ap.add_argument("-o", "--out-dir", type=Path, default=Path("out"),
                    help="output directory (default: out/)")
    args = ap.parse_args()
    extract(args.musicxml, args.out_dir)


if __name__ == "__main__":
    main()
