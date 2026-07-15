#!/usr/bin/env python3
"""Re-render extracted measure primitives to SVG for visual verification.

Reads the JSON produced by extract.py and renders it two ways:

  match   - every measure at its original position, so the output can be
            overlaid pixel-for-pixel on Verovio's own SVG
  reflow  - measures packed into systems of a given width, with the
            clef/key-signature header re-inserted at each system start;
            this is the layout algorithm the firmware will run

Also emits compare.html: Verovio's original (red) and the match re-render
(blue) stacked and overlaid with multiply blending — any misalignment shows
as separated red/blue fringes instead of dark overlap.

Staff lines are NOT stored as primitives; both modes draw them from the
measure width alone, which validates the "firmware draws staff lines"
design decision in docs/music/cpmx-format-draft.md.
"""

import argparse
import json
import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape

STAFF_LINE_W = 0.0722  # staff-space units; Verovio default (13/180)


def fmt(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


class SvgRenderer:
    """Renders measure primitive lists into SVG fragments at a given scale."""

    def __init__(self, doc, staff_space):
        self.doc = doc
        self.ss = staff_space
        # Glyph <use> scale factors in the JSON are relative to the source
        # render's staff space; rescale them for the requested size.
        self.glyph_scale = staff_space / doc["meta"]["unitsPerStaffSpace"]

    def defs(self):
        out = ["<defs>"]
        for codepoint, glyph in self.doc["glyphs"].items():
            transform = f' transform="{glyph["transform"]}"' if glyph["transform"] else ""
            out.append(f'<g id="{codepoint}"><path{transform} d="{glyph["d"]}"/></g>')
        out.append("</defs>")
        return "\n".join(out)

    def staff_lines(self, ox, oy, width):
        ss = self.ss
        w = STAFF_LINE_W * ss
        return [
            f'<path d="M {fmt(ox)} {fmt(oy + i * ss)} L {fmt(ox + width * ss)} '
            f'{fmt(oy + i * ss)}" stroke-width="{fmt(w)}"/>'
            for i in range(5)
        ]

    def measure(self, record, ox, oy, with_staff=True, roles=None):
        """Render one measure with its origin (top staff line, left edge) at (ox, oy)."""
        ss = self.ss
        out = []
        if with_staff:
            out.extend(self.staff_lines(ox, oy, record["width"]))
        for p in record["primitives"]:
            if roles is not None and p["role"] not in roles:
                continue
            out.append(self.primitive(p, ox, oy))
        return out

    def primitive(self, p, ox, oy):
        ss = self.ss

        def X(v):
            return fmt(ox + v * ss)

        def Y(v):
            return fmt(oy + v * ss)

        if p["type"] == "glyph":
            s = p["scale"] * self.glyph_scale
            return (f'<use href="#{p["codepoint"]}" xlink:href="#{p["codepoint"]}" '
                    f'transform="translate({X(p["x"])}, {Y(p["y"])}) '
                    f'scale({s:.4f}, {s:.4f})"/>')
        if p["type"] == "line":
            return (f'<path d="M {X(p["x1"])} {Y(p["y1"])} L {X(p["x2"])} '
                    f'{Y(p["y2"])}" stroke-width="{fmt(p["w"] * ss)}"/>')
        if p["type"] == "beam":
            pts = " ".join(f'{X(x)},{Y(y)}' for x, y in p["points"])
            return f'<polygon points="{pts}"/>'
        if p["type"] == "curve":
            pts = " ".join(f'{X(x)},{Y(y)}' for x, y in p["points"])
            return (f'<polygon points="{pts}" stroke-width="{fmt(p["w"] * ss)}" '
                    f'stroke-linejoin="round"/>')
        if p["type"] == "polyline":
            pts = " ".join(f'{X(x)},{Y(y)}' for x, y in p["points"])
            return (f'<polyline points="{pts}" fill="none" '
                    f'stroke="currentColor" stroke-width="{fmt(p["w"] * ss)}" '
                    f'stroke-linecap="square" stroke-linejoin="miter"/>')
        if p["type"] == "dot":
            return (f'<ellipse cx="{X(p["x"])}" cy="{Y(p["y"])}" '
                    f'rx="{fmt(p["r"] * ss)}" ry="{fmt(p["r"] * ss)}"/>')
        if p["type"] == "text":
            anchor = (f' text-anchor="{p["anchor"]}"'
                      if p.get("anchor", "start") != "start" else "")
            weight = (f' font-weight="{p["weight"]}"' if p.get("weight") else "")
            return (f'<text x="{X(p["x"])}" y="{Y(p["y"])}" '
                    f'font-size="{fmt(p["size"] * ss)}"{anchor}{weight} '
                    f'font-family="Times, serif">{escape(p["text"])}</text>')
        raise ValueError(f"unknown primitive type {p['type']!r}")


def svg_document(body, width, height, color):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{width:.0f}" height="{height:.0f}" '
        # No background fill: pixeldiff.html treats every opaque pixel as ink,
        # so a painted background would saturate the mask and zero the diff.
        f'viewBox="0 0 {width:.0f} {height:.0f}" color="{color}" '
        f'style="fill:currentColor;stroke:none">\n'
        f'<style>path[stroke-width], polygon[stroke-width], ellipse '
        f'{{stroke:currentColor}}</style>\n'
        f"{body}\n</svg>"
    )


HEADER_ROLES = {"clef", "keySig", "keyAccid"}


def render_match(doc, staff_space, mx, my):
    """Re-render every measure at its source position for overlay comparison."""
    r = SvgRenderer(doc, staff_space)
    src_ss = doc["meta"]["unitsPerStaffSpace"]
    k = staff_space / src_ss  # page-unit -> output-pixel factor
    parts = [r.defs()]
    max_x = max_y = 0.0
    for m in doc["measures"]:
        ox = mx + m["sourceX"] * k
        oy = my + m["sourceY"] * k
        parts.extend(r.measure(m, ox, oy))
        max_x = max(max_x, ox + m["width"] * staff_space)
        max_y = max(max_y, oy + 4 * staff_space)
    body = "\n".join(parts)
    return svg_document(body, max_x + mx, max_y + my + 4 * staff_space, "#0033cc")


def render_reflow(doc, staff_space, page_width, margin, order=None):
    """Pack measures into systems — the layout the firmware will perform.

    With `order` (a playOrder list of measure indices), measures are laid out
    in performance sequence — repeats unrolled, so "next page" on the device
    always shows what the player performs next.
    """
    r = SvgRenderer(doc, staff_space)
    measures = doc["measures"]
    if order is not None:
        measures = [doc["measures"][i] for i in order]
    if not measures:
        return svg_document("", page_width, 100, "black")

    # System header = clef + current key signature. In .cpmx these become
    # SystemHeaderBlocks referenced per measure; here the clef comes from
    # measure 0 (it never changes in our use case) and the key signature is
    # tracked as measures carrying a new one go by (trio modulations).
    clef_prims = [p for p in measures[0]["primitives"] if p["role"] == "clef"]
    key_prims = [p for p in measures[0]["primitives"]
                 if p["role"] in ("keySig", "keyAccid")]
    # Where the key signature starts, right of the clef; reused when a
    # mid-piece key change replaces the signature.
    key_anchor = min((p["x"] for p in key_prims), default=2.5)

    def header():
        return clef_prims + key_prims

    def header_width():
        return max((p["x"] for p in header()), default=0) + 2.0

    usable = (page_width - 2 * margin) / staff_space  # in staff spaces
    system_gap = 10 * staff_space
    parts = [r.defs()]
    x = 0.0  # staff spaces within current system
    oy = margin + 2 * staff_space
    system_start = True

    for i, m in enumerate(measures):
        if not system_start and (x + m["width"]) > usable:
            # New system: redraw clef + current key signature at line start.
            oy += 4 * staff_space + system_gap
            x = header_width()
            for p in header():
                parts.append(r.primitive(p, margin, oy))
            parts.extend(r.staff_lines(margin, oy, header_width()))
        parts.extend(r.measure(m, margin + x * staff_space, oy))
        x += m["width"]
        system_start = False
        # A measure carrying its own key signature (mid-piece change, e.g.
        # a trio modulation) replaces the signature used on later lines.
        if i > 0:
            new_key = [p for p in m["primitives"]
                       if p["role"] in ("keySig", "keyAccid")]
            if new_key:
                shift = key_anchor - min(p["x"] for p in new_key)
                key_prims = [{**p, "x": round(p["x"] + shift, 4)} for p in new_key]

    height = oy + 4 * staff_space + margin + 2 * staff_space
    return svg_document("\n".join(parts), page_width, height, "black")


def recolor_verovio_svg(svg_text, color):
    # Verovio only sets stroke:currentColor; fills default to black. Set both
    # on the inner svg so the whole render takes the comparison color.
    return svg_text.replace('color="black"', f'color="{color}" fill="{color}"', 1)


COMPARE_HTML = """<!doctype html>
<meta charset="utf-8">
<title>cpmx extraction check — {name}</title>
<style>
  body {{ font: 14px system-ui; margin: 20px; background: #fff; color: #111; }}
  h2 {{ margin: 24px 0 8px; }}
  .pane {{ overflow-x: auto; border: 1px solid #ccc; padding: 8px; background: #fff; }}
  svg {{ display: block; }}
</style>
<h1>Extraction check: {name}</h1>
<p>{summary}</p>
<h2>1. Verovio original (red)</h2>
<div class="pane">{original}</div>
<h2>2. Re-rendered from extracted primitives (blue)</h2>
<div class="pane">{rerender}</div>
<h2>3. Pixel diff</h2>
<p>Open <a href="../pixeldiff.html?stem=out/{name}">pixeldiff.html?stem=out/{name}</a>
(serve the converter/ directory over HTTP) for a quantitative ink-pixel
comparison of the two renders above.</p>
<h2>4. Reflowed to narrow page (the point of the whole exercise)</h2>
<div class="pane">{reflow}</div>
{unrolled_section}
"""

UNROLLED_SECTION = """<h2>5. Performance order (repeats unrolled: {order})</h2>
<p>The sequence the firmware pages through — "next page" always shows what
the player performs next, including jumps back to repeats and the correct
ending ("hus").</p>
<div class="pane">{unrolled}</div>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("json_file", type=Path, help="primitives JSON from extract.py")
    ap.add_argument("--staff-space", type=float, default=12.0,
                    help="reflow staff space in px (default 12)")
    ap.add_argument("--reflow-width", type=float, default=480.0,
                    help="reflow page width in px (default 480 — X4 portrait width)")
    args = ap.parse_args()

    doc = json.loads(args.json_file.read_text())
    out_dir = args.json_file.parent
    stem = args.json_file.name.replace(".primitives.json", "")

    raw_svg_path = out_dir / f"{stem}.verovio.svg"
    raw_svg = raw_svg_path.read_text()

    # Derive Verovio's px scale and page margin from the raw SVG itself
    # (outer px width vs inner viewBox, and the page-margin translate) so the
    # match render aligns regardless of Verovio version or options.
    m_w = re.search(r'<svg[^>]*\bwidth="([\d.]+)px"', raw_svg)
    m_vb = re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', raw_svg)
    m_pm = re.search(
        r'class="page-margin" transform="translate\(([\d.]+),\s*([\d.]+)\)"', raw_svg)
    if not (m_w and m_vb and m_pm):
        sys.exit("error: could not locate scale/margin markers in the raw Verovio SVG")
    px_per_unit = float(m_w.group(1)) / float(m_vb.group(1))
    src_ss = doc["meta"]["unitsPerStaffSpace"]
    match_ss = src_ss * px_per_unit
    mx = float(m_pm.group(1)) * px_per_unit
    my = float(m_pm.group(2)) * px_per_unit

    match_svg = render_match(doc, match_ss, mx, my)
    reflow_svg = render_reflow(doc, args.staff_space, args.reflow_width, 20.0)

    (out_dir / f"{stem}.rerender.svg").write_text(match_svg)
    (out_dir / f"{stem}.reflow.svg").write_text(reflow_svg)

    play_order = doc.get("playOrder")
    unrolled_section = ""
    if play_order and play_order != list(range(len(doc["measures"]))):
        unrolled_svg = render_reflow(doc, args.staff_space, args.reflow_width,
                                     20.0, order=play_order)
        (out_dir / f"{stem}.unrolled.svg").write_text(unrolled_svg)
        print(f"unrolled  -> {out_dir / f'{stem}.unrolled.svg'}")
        unrolled_section = UNROLLED_SECTION.format(
            order=" ".join(str(i + 1) for i in play_order),
            unrolled=unrolled_svg)

    n_prims = sum(len(m["primitives"]) for m in doc["measures"])
    warn = doc.get("warnings", [])
    summary = (f'{len(doc["measures"])} measures, {n_prims} primitives, '
               f'{len(warn)} extraction warning(s). '
               f'Source: {doc["meta"]["source"]}, '
               f'Verovio {doc["meta"]["verovioVersion"]}.')
    html = COMPARE_HTML.format(
        name=stem,
        summary=summary,
        original=recolor_verovio_svg(raw_svg, "#cc2200"),
        rerender=match_svg,
        reflow=reflow_svg,
        unrolled_section=unrolled_section,
    )
    html_path = out_dir / f"{stem}.compare.html"
    html_path.write_text(html)
    print(f"re-render -> {out_dir / f'{stem}.rerender.svg'}")
    print(f"reflow    -> {out_dir / f'{stem}.reflow.svg'}")
    print(f"compare   -> {html_path}")


if __name__ == "__main__":
    main()
