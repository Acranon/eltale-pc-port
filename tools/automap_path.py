#!/usr/bin/env python3
"""The player's path on the area map's truth pictures (docs/AUTOMAP.md, "Accuracy").

    python tools/automap_path.py LOG SHOTS_DIR [OUT_DIR]

LOG is a run's stderr with ELTALE_AUTOMAP_TRACE=1 (P1_AUTOMAP_POS lines: where the player is every tick) and
SHOTS_DIR its ELTALE_AUTOMAP_SHOTS pictures (am_MM_SS_over.png and am_MM_SS_plan.json). For each sub-area walked in,
the path is drawn in cyan over the map's lines on the truth view, framed as the host frames that picture
(port/host/automap.cpp draw_over), into OUT_DIR/am_MM_SS_path.png (OUT_DIR defaults to SHOTS_DIR). It prints
AUTOMAP_PATH map=M sub=S ticks=N moved=D crossings=C per sub-area: C counts the steps that cross one of the plan's
wall segments, which the game's own wall push never lets happen (0 when the plan's walls are the game's).
"""

import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

MX = -1.0  # screen x = -world x (port/host/automap.cpp)


def cross(a, b, c, d):
    def orient(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    o1, o2, o3, o4 = orient(a, b, c), orient(a, b, d), orient(c, d, a), orient(c, d, b)
    return o1 * o2 < 0 and o3 * o4 < 0


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    log, shots = Path(sys.argv[1]), Path(sys.argv[2])
    out = Path(sys.argv[3]) if len(sys.argv) > 3 else shots
    out.mkdir(parents=True, exist_ok=True)
    paths: dict[tuple[int, int], list[tuple[float, float, float]]] = {}
    pat = re.compile(r"P1_AUTOMAP_POS frame=\d+ map=(\d+) sub=(\d+) x=(\S+) y=\S+ z=(\S+) rot=(\S+)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.search(line)
        if m:
            paths.setdefault((int(m.group(1)), int(m.group(2))), []).append(
                (float(m.group(3)), float(m.group(4)), float(m.group(5))))
    for (mp, sub), pts in sorted(paths.items()):
        plan_p = shots / f"am_{mp:02d}_{sub:02d}_plan.json"
        over_p = shots / f"am_{mp:02d}_{sub:02d}_over.png"
        if not plan_p.exists() or not over_p.exists():
            print(f"AUTOMAP_PATH map={mp} sub={sub} ticks={len(pts)} (no picture)")
            continue
        plan = json.loads(plan_p.read_text())
        g, b = plan["rect"], plan["bounds"]
        gw, gh = g[2] - g[0], g[3] - g[1]
        aspect = gw / gh
        cx, cz = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
        half = max(max((b[3] - b[1]) / 2, (b[2] - b[0]) / 2 / aspect) * 1.04, 100.0)
        s = gh / 2 / half
        ox, oy = (g[0] + g[2]) / 2, (g[1] + g[3]) / 2

        def at(x, z):
            return (ox + MX * (x - cx) * s, oy - (z - cz) * s)

        walls = plan["walls"]
        crossings = 0
        moved = 0.0
        for (x0, z0, _), (x1, z1, _) in zip(pts, pts[1:]):
            d = ((x1 - x0) ** 2 + (z1 - z0) ** 2) ** 0.5
            if d > 60:  # a warp (an exit, a scene): not a step
                continue
            moved += d
            for w in walls:
                if cross((x0, z0), (x1, z1), (w[0], w[1]), (w[2], w[3])):
                    crossings += 1
                    break
        img = Image.open(over_p).convert("RGB")
        dr = ImageDraw.Draw(img)
        sc = [at(x, z) for x, z, _ in pts]
        dr.line(sc, fill=(0, 255, 255), width=3)
        for i, p in enumerate(sc[::15]):
            dr.ellipse((p[0] - 3, p[1] - 3, p[0] + 3, p[1] + 3), fill=(0, 200, 255))
        dr.ellipse((sc[0][0] - 6, sc[0][1] - 6, sc[0][0] + 6, sc[0][1] + 6), outline=(0, 255, 0), width=3)
        dr.ellipse((sc[-1][0] - 6, sc[-1][1] - 6, sc[-1][0] + 6, sc[-1][1] + 6), outline=(255, 80, 80), width=3)
        img.save(out / f"am_{mp:02d}_{sub:02d}_path.png")
        print(f"AUTOMAP_PATH map={mp} sub={sub} ticks={len(pts)} moved={moved:.0f} crossings={crossings}")


if __name__ == "__main__":
    main()
