#!/usr/bin/env python3
"""Contact sheets of the area map against the truth (docs/AUTOMAP.md).

    python tools/automap_sheet.py SHOTS_DIR OUT_DIR [--size 400]

SHOTS_DIR holds a run's pictures (ELTALE_AUTOMAP_SHOTS=1 with ELTALE_PRESENT_DUMP, in a hidden window, usually with
ELTALE_MAP_TOUR=1): per sub-area am_MM_SS_truth.png (the sub-area's real geometry drawn by the native renderer from
straight above), am_MM_SS_over.png (the map's walls in magenta and floor edges in yellow over that picture, framed
the same) and am_MM_SS_map.png (the full map as the player sees it). OUT_DIR gets one sheet per map,
sheet_MM.png (a row per sub-area: truth, over, map), and index.html showing them all.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shots", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--size", type=int, default=400, help="each picture's width in the sheet")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    areas: dict[int, set[int]] = {}
    for p in args.shots.glob("am_*_*_*.png"):
        m = re.match(r"am_(\d+)_(\d+)_(truth|over|map)\.png$", p.name)
        if m:
            areas.setdefault(int(m.group(1)), set()).add(int(m.group(2)))
    w = args.size
    h = w * 3 // 4
    label_h = 22
    kinds = ("truth", "over", "map")
    sheets = []
    for mp in sorted(areas):
        subs = sorted(areas[mp])
        sheet = Image.new("RGB", (w * 3 + 8 * 4, (h + label_h + 8) * len(subs) + 8), (24, 22, 20))
        d = ImageDraw.Draw(sheet)
        for row, sub in enumerate(subs):
            y = 8 + row * (h + label_h + 8)
            for col, kind in enumerate(kinds):
                x = 8 + col * (w + 8)
                p = args.shots / f"am_{mp:02d}_{sub:02d}_{kind}.png"
                d.text((x, y + 4), f"map {mp} sub {sub}: {kind}", fill=(230, 215, 170))
                if p.exists():
                    img = Image.open(p).convert("RGB")
                    img.thumbnail((w, h), Image.LANCZOS)
                    sheet.paste(img, (x + (w - img.width) // 2, y + label_h + (h - img.height) // 2))
                else:
                    d.text((x + 10, y + label_h + h // 2), "(no picture)", fill=(160, 80, 80))
        name = f"sheet_{mp:02d}.png"
        sheet.save(args.out / name)
        sheets.append((mp, name, len(subs)))
    with open(args.out / "index.html", "w", encoding="utf-8", newline="\n") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Area map vs truth</title>"
                "<style>body{background:#181614;color:#e6d7aa;font:14px sans-serif}img{max-width:100%}</style>\n")
        f.write("<h1>Area map against the truth view</h1><p>Per sub-area: the real geometry from above (truth), the "
                "map's walls (magenta) and floor edges (yellow) over it (over), the full map as shown (map).</p>\n")
        for mp, name, n in sheets:
            f.write(f"<h2 id=m{mp}>Map {mp} ({n} sub-areas)</h2><img src='{name}'>\n")
    print(f"{len(sheets)} sheets in {args.out}")


if __name__ == "__main__":
    main()
