#!/usr/bin/env python3
"""Contact sheets of the area map against the truth (docs/AUTOMAP.md).

    python tools/automap_sheet.py SHOTS_DIR OUT_DIR [--size 400] [--maps 0,13]

SHOTS_DIR holds a run's pictures (ELTALE_AUTOMAP_SHOTS=1 with ELTALE_PRESENT_DUMP, in a hidden window, usually with
ELTALE_MAP_TOUR=1): per sub-area am_MM_SS_truth.png (the sub-area's real geometry drawn by the native renderer from
straight above, every person drawn), am_MM_SS_over.png (the map's walls in magenta, floor edges in yellow and every
mark, named, over that picture, framed the same; the battle zones shaded red, numbered), am_MM_SS_map.png (the full map as the player sees it) and
am_MM_SS_plan.json (the plan's bounds and the over picture's marks). OUT_DIR gets per map:
  sheet_MM.png        a row per sub-area: truth, over, map;
  marks_MM.png        a close-up per mark (doors, ways out, people, chests, spirits, the boss): the truth view and the
                      over picture side by side around the mark, so the mark can be seen sitting on the real door,
                      person or chest;
and index.html showing them all.
"""

import argparse
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw

KINDS = {1: "door", 2: "way", 3: "person", 4: "chest", 5: "spirit", 6: "boss", 7: "sign"}
SERVICES = {2: "inn", 4: "wings", 8: "food"}


def over_xform(plan: dict):
    """World (x, z) to the over picture's pixels: the truth camera's framing (nd_automap.c, automap.cpp draw_over)."""
    g, b = plan["rect"], plan["bounds"]
    gw, gh = g[2] - g[0], g[3] - g[1]
    cx, cz = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
    half = max(max((b[3] - b[1]) / 2, (b[2] - b[0]) / 2 / (gw / gh)) * 1.04, 100.0)
    s = gh * 0.5 / half
    ox, oy = (g[0] + g[2]) / 2, (g[1] + g[3]) / 2
    return (lambda x, z: (ox - (x - cx) * s, oy - (z - cz) * s)), s


def tile_sheet(tiles, t: int, path: Path, per_row: int = 4) -> None:
    """Close-ups (label, truth, over) side by side, `per_row` to a row."""
    cell_w, cell_h = t * 2 + 4 + 12, t + 34
    rows = (len(tiles) + per_row - 1) // per_row
    ms = Image.new("RGB", (cell_w * per_row + 12, cell_h * rows + 12), (24, 22, 20))
    md = ImageDraw.Draw(ms)
    for i, (label, a, b) in enumerate(tiles):
        x = 12 + (i % per_row) * cell_w
        y = 12 + (i // per_row) * cell_h
        ms.paste(a, (x, y))
        ms.paste(b, (x + t + 4, y))
        md.text((x, y + t + 3), label[:60], fill=(230, 215, 170))
        if len(label) > 60:
            md.text((x, y + t + 15), label[60:120], fill=(200, 190, 150))
    ms.save(path)


def mark_tiles(shots: Path, mp: int, sub: int, tile: int, only=None):
    """A close-up per mark: (label, truth crop, over crop); `only(mark)` picks some."""
    pj = shots / f"am_{mp:02d}_{sub:02d}_plan.json"
    if not pj.exists():
        return []
    plan = json.loads(pj.read_text(encoding="utf-8"))
    marks = plan.get("marks", [])
    if not marks:
        return []
    truth = Image.open(shots / f"am_{mp:02d}_{sub:02d}_truth.png").convert("RGB")
    over = Image.open(shots / f"am_{mp:02d}_{sub:02d}_over.png").convert("RGB")
    at, s = over_xform(plan)
    out = []
    for m in marks:
        if only and not only(m):
            continue
        # +-30 units around it (a door: its width too), at least 24 pixels
        r = max(30.0 * s, (m["half_w"] * 2.2 if m["kind"] in (1, 2) else 0) * s, 24.0)
        px, py = at(m["x"], m["z"])
        box = (int(px - r), int(py - r), int(px + r), int(py + r))
        crops = [im.crop(box).resize((tile, tile), Image.LANCZOS) for im in (truth, over)]
        svc = "+".join(v for k, v in SERVICES.items() if m.get("services", 0) & k)
        kind = KINDS.get(m["kind"], "?") + (f" [{svc}]" if svc else "")
        out.append((f"{mp}/{sub} {kind}: " + m["label"].replace(chr(10), " | "), crops[0], crops[1]))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shots", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--size", type=int, default=400, help="each picture's width in the sheet")
    ap.add_argument("--tile", type=int, default=150, help="each close-up's side")
    ap.add_argument("--maps", help="only these maps (comma-separated)")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    only = {int(v) for v in args.maps.split(",")} if args.maps else None
    areas: dict[int, set[int]] = {}
    for p in args.shots.glob("am_*_*_*.png"):
        m = re.match(r"am_(\d+)_(\d+)_(truth|over|map)\.png$", p.name)
        if m and (only is None or int(m.group(1)) in only):
            areas.setdefault(int(m.group(1)), set()).add(int(m.group(2)))
    w = args.size
    h = w * 3 // 4
    label_h = 22
    kinds = ("truth", "over", "map", "mini")
    sheets = []
    for mp in sorted(areas):
        subs = sorted(areas[mp])
        sheet = Image.new("RGB", (w * 3 + h + 8 * 5, (h + label_h + 8) * len(subs) + 8), (24, 22, 20))
        d = ImageDraw.Draw(sheet)
        for row, sub in enumerate(subs):
            y = 8 + row * (h + label_h + 8)
            for col, kind in enumerate(kinds):
                x = 8 + col * (w + 8)
                p = args.shots / f"am_{mp:02d}_{sub:02d}_{kind}.png"
                d.text((x, y + 4), f"map {mp} sub {sub}: {kind}", fill=(230, 215, 170))
                if p.exists():
                    img = Image.open(p).convert("RGB")
                    if kind == "mini":  # the minimap alone, as big as the row
                        r = json.loads(p.with_suffix(".json").read_text(encoding="utf-8"))["rect"]
                        img = img.crop(tuple(int(v) for v in r)).resize((h, h), Image.LANCZOS)
                    img.thumbnail((w, h), Image.LANCZOS)
                    sheet.paste(img, (x + (w - img.width) // 2, y + label_h + (h - img.height) // 2))
                else:
                    d.text((x + 10, y + label_h + h // 2), "(no picture)", fill=(160, 80, 80))
        name = f"sheet_{mp:02d}.png"
        sheet.save(args.out / name)
        # the marks' close-ups: truth | over, four to a row
        tiles = []
        for sub in subs:
            if (args.shots / f"am_{mp:02d}_{sub:02d}_truth.png").exists():
                tiles += mark_tiles(args.shots, mp, sub, args.tile)
        marks_name = None
        if tiles:
            marks_name = f"marks_{mp:02d}.png"
            tile_sheet(tiles, args.tile, args.out / marks_name)
        sheets.append((mp, name, len(subs), marks_name, len(tiles)))
    # the services (every door to an inn, the Wings or food, and every person offering one) and the signs, close up
    extra = []
    for what, pick in (("services", lambda m: m.get("services", 0) or m.get("service", 0)),
                       ("signs", lambda m: m["kind"] == 7)):
        tiles = []
        for mp in sorted(areas):
            for sub in sorted(areas[mp]):
                if (args.shots / f"am_{mp:02d}_{sub:02d}_truth.png").exists():
                    tiles += mark_tiles(args.shots, mp, sub, args.tile, pick)
        if tiles:
            tile_sheet(tiles, args.tile, args.out / f"{what}.png")
            extra.append((what, len(tiles)))
    # the battle zones: every sub-area with zones, its over picture (the zones shaded and numbered) and its labels
    zoned = []
    for mp in sorted(areas):
        for sub in sorted(areas[mp]):
            pj = args.shots / f"am_{mp:02d}_{sub:02d}_plan.json"
            if pj.exists() and json.loads(pj.read_text(encoding="utf-8")).get("zones"):
                zoned.append((mp, sub, json.loads(pj.read_text(encoding="utf-8"))["zones"]))
    if zoned:
        zw, zh, per_row = 640, 480, 3
        rows = (len(zoned) + per_row - 1) // per_row
        zs = Image.new("RGB", (per_row * (zw + 8) + 8, rows * (zh + 120) + 8), (24, 22, 20))
        zd = ImageDraw.Draw(zs)
        for i, (mp, sub, zones) in enumerate(zoned):
            x, y = 8 + (i % per_row) * (zw + 8), 8 + (i // per_row) * (zh + 120)
            img = Image.open(args.shots / f"am_{mp:02d}_{sub:02d}_over.png").convert("RGB")
            img.thumbnail((zw, zh), Image.LANCZOS)
            zs.paste(img, (x, y))
            zd.text((x, y + zh + 4), f"map {mp} sub {sub}: {len(zones)} zones", fill=(255, 200, 190))
            for k, z in enumerate(zones[:7]):
                zd.text((x, y + zh + 18 + k * 13), f"{k + 1}: {z['cells']} cells, {z['label']}"[:100], fill=(230, 215, 170))
        zs.save(args.out / "zones.png")
        extra.append(("zones", len(zoned)))
    with open(args.out / "index.html", "w", encoding="utf-8", newline="\n") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Area map vs truth</title>"
                "<style>body{background:#181614;color:#e6d7aa;font:14px sans-serif}img{max-width:100%}</style>\n")
        f.write("<h1>Area map against the truth view</h1><p>Per sub-area: the real geometry from above (truth), the "
                "map's walls (magenta), floor edges (yellow) and marks (named) over it (over), the full map as shown "
                "(map), the battle zones shaded red (where the game's rule lets a random battle start, numbered). Then per "
                "map a close-up of every mark: truth | over (a door to an inn, the Wings or food has a coloured halo, and "
                "its service named).</p>\n")
        for what, n in extra:
            f.write(f"<h2>All {what} ({n})</h2><img src='{what}.png'>\n")
        for mp, name, n, marks_name, nt in sheets:
            f.write(f"<h2 id=m{mp}>Map {mp} ({n} sub-areas)</h2><img src='{name}'>\n")
            if marks_name:
                f.write(f"<h3>Map {mp}: the marks close up ({nt})</h3><img src='{marks_name}'>\n")
    print(f"{len(sheets)} sheets in {args.out}")


if __name__ == "__main__":
    main()
