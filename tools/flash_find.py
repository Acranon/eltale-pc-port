#!/usr/bin/env python3
"""Find the "one picture off" pictures among pictures between ticks (R6, docs/renderer/NATIVE_DRAW.md "Pictures
between ticks"): a --dump-frames folder made with ELTALE_DUMP_BETWEEN=N (frame_<a>.png the ticks' own pictures,
frame_<a>_<ppp>.png the pictures between tick a and the next at ppp/1000), or a flash recorder's folder (F8: 0000.png
... and pictures.txt with each picture's retrace and phase). After King's Field IV's tools/flash_find.py.

    python tools/flash_find.py FOLDER [--threshold X] [--level L] [--width W] [--top N] [--sheet FILE] [--neighbours]

Default (KF4's --frames): each picture between is compared with the two ticks around it and with their blend at
its time. It is off where it differs by more than --level (24) from the blend and from every pixel within --reach
(6: half of 12 pixels a tick, a fast sprite near the camera) of its place in both ticks: a shard, a hole, a
flash, something at neither tick's place (a thin thing that moves, a candle while the camera turns, is near its
place in both and is not counted). Only off pixels whose whole 3 x 3 neighbourhood is off count (solid shapes,
not the edges a turn leaves). --neighbours instead compares each picture with the pictures k before and after it
(k up to 8) where those two agree (KF4's first mode; for a flash recorder's folder, whose pictures are at any
phase: its retrace-own pictures are not ticks).

A picture whose off share is at least --threshold (0.2% by default) is flagged. Runs of pictures are split where
the tick numbers jump (a sampled dump). The flagged pictures go into a contact sheet (FOLDER/flashes.png, or
--sheet): the tick before, the picture, the tick after, where it is off. Needs Pillow and numpy.
"""
import argparse
import os
import re
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("flash_find.py needs Pillow and numpy (pip install pillow numpy)")

LEVEL = 24
SOLID = 1  # the neighbourhood's half size: 3 x 3


def load_small(path, width):
    im = Image.open(path).convert("RGB")
    if width <= 0 or width == im.width:
        return np.asarray(im, dtype=np.int16)
    h = max(1, round(im.height * width / im.width))
    return np.asarray(im.resize((width, h), Image.BILINEAR), dtype=np.int16)


def erode(mask, r):
    h, w = mask.shape
    pad = np.zeros((h + 2 * r, w + 2 * r), dtype=bool)
    pad[r:r + h, r:r + w] = mask
    out = np.ones_like(mask)
    for dy in range(2 * r + 1):
        for dx in range(2 * r + 1):
            out &= pad[dy:dy + h, dx:dx + w]
    return out


def far(P, A, reach, level):
    """where P's pixel is more than level from every pixel of A within reach (a square): what is in neither tick
    near its place. A thin thing that moves (a candle while the camera turns) is near its place in A and in B; a
    shard, a hole or a flash is not."""
    h, w = P.shape[:2]
    pad = np.pad(A, ((reach, reach), (reach, reach), (0, 0)), mode="edge")
    best = None
    for dy in range(2 * reach + 1):
        for dx in range(2 * reach + 1):
            d = np.abs(P - pad[dy:dy + h, dx:dx + w]).max(axis=2)
            best = d if best is None else np.minimum(best, d)
    return best > level


def pictures(folder):
    """[(name, time)] in time order: time = tick + phase (dumps) or retrace + phase (a recorder's pictures.txt)"""
    listed = os.path.join(folder, "pictures.txt")
    out = []
    if os.path.exists(listed):
        for line in open(listed):
            f = line.split()
            if len(f) >= 3 and not line.startswith("#"):
                out.append((f[0], int(f[1]) + float(f[2])))
    else:
        for n in os.listdir(folder):
            m = re.match(r"frame_(\d+)(?:_(\d+))?\.png$", n)
            if m:
                out.append((n, int(m.group(1)) + (int(m.group(2)) / 1000.0 if m.group(2) else 0.0)))
    out.sort(key=lambda p: p[1])
    return out


def runs(pics):
    """split where the ticks jump by more than one"""
    out, cur = [], []
    for p in pics:
        if cur and int(p[1]) > int(cur[-1][1]) + 1:
            out.append(cur)
            cur = []
        cur.append(p)
    if cur:
        out.append(cur)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder")
    ap.add_argument("--threshold", type=float, default=0.002, help="share of the picture off to flag it (0.002)")
    ap.add_argument("--level", type=int, default=LEVEL, help="levels off to count a pixel (24)")
    ap.add_argument("--width", type=int, default=0, help="compare at this width (0: as they are)")
    ap.add_argument("--top", type=int, default=24, help="at most this many rows in the sheet (24)")
    ap.add_argument("--sheet", help="the contact sheet (FOLDER/flashes.png)")
    ap.add_argument("--reach", type=int, default=6,
                    help="pixels a thing may have moved and still be near its place in a tick (6)")
    ap.add_argument("--neighbours", action="store_true", help="compare with the neighbouring pictures instead")
    ap.add_argument("--span", type=int, default=8, help="--neighbours: up to this many pictures away (8)")
    args = ap.parse_args()

    pics = pictures(args.folder)
    if len(pics) < 3:
        sys.exit("fewer than 3 pictures in %s" % args.folder)
    cache = {}

    def img(name):
        if name not in cache:
            cache[name] = load_small(os.path.join(args.folder, name), args.width)
        return cache[name]

    found = []  # (off, name, before, after, mask)
    between = repeats = 0
    for run in runs(pics):
        if args.neighbours:
            for i in range(1, len(run) - 1):
                best = None
                for k in range(1, args.span + 1):
                    if i - k < 0 or i + k >= len(run):
                        break
                    P, A, B = img(run[i][0]), img(run[i - k][0]), img(run[i + k][0])
                    off = erode((np.abs(A - B).max(axis=2) <= args.level) & (np.abs(P - A).max(axis=2) > args.level) &
                                (np.abs(P - B).max(axis=2) > args.level), SOLID)
                    s = float(off.mean())
                    if best is None or s > best[0]:
                        best = (s, run[i][0], run[i - k][0], run[i + k][0], off)
                between += 1
                if best and best[0] >= args.threshold:
                    found.append(best)
            continue
        ticks = [p for p in run if p[1] == int(p[1])]
        for a, b in zip(ticks, ticks[1:]):
            A, B = img(a[0]), img(b[0])
            for name, t in run:
                if not a[1] < t < b[1]:
                    continue
                between += 1
                u = (t - a[1]) / (b[1] - a[1])
                P = img(name)
                if np.array_equal(P, A):
                    repeats += 1
                blend = A * (1 - u) + B * u
                off = (np.abs(P - blend).max(axis=2) > args.level) & (np.abs(P - A).max(axis=2) > args.level) & \
                    (np.abs(P - B).max(axis=2) > args.level)
                # (the search within reach only where the plain test flags enough: it can only take pixels away)
                if float(erode(off, SOLID).mean()) >= args.threshold and args.reach > 0:
                    off &= far(P, A, args.reach, args.level) & far(P, B, args.reach, args.level)
                off = erode(off, SOLID)
                s = float(off.mean())
                if s >= args.threshold:
                    found.append((s, name, a[0], b[0], off))
        if len(ticks) < 2:
            continue

    names = [p[0] for p in pics]
    flagged = sorted(names.index(f[1]) for f in found)
    flashes = []
    for i in flagged:
        if flashes and i <= flashes[-1][1] + 2:
            flashes[-1][1] = i
        else:
            flashes.append([i, i])
    print("%d pictures, %d between ticks (%d the same as the tick before), %d flagged (off >= %.2f%%) in %d flashes" %
          (len(pics), between, repeats, len(found), args.threshold * 100, len(flashes)))
    for a, b in flashes:
        print("  flash: %s%s" % (names[a], "" if a == b else " to " + names[b]))
    found.sort(key=lambda f: -f[0])
    for off, name, a, b, _ in found:
        print("  %-24s off %6.2f%%  (between %s and %s)" % (name, off * 100, a, b))
    if not found:
        return
    shown = found[: args.top]
    h, w = img(shown[0][1]).shape[:2]
    sheet = Image.new("RGB", (w * 4, (h + 14) * len(shown)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for row, (off, name, a, b, mask) in enumerate(shown):
        y = row * (h + 14)
        for col, n in enumerate((a, name, b)):
            sheet.paste(Image.fromarray(img(n).astype("uint8"), "RGB"), (col * w, y + 14))
        sheet.paste(Image.fromarray((mask * 255).astype("uint8"), "L").convert("RGB"), (3 * w, y + 14))
        draw.text((2, y + 1), "%s | %s (off %.2f%%) | %s | where" % (a, name, off * 100, b), fill=(255, 220, 0))
    out = args.sheet or os.path.join(args.folder, "flashes.png")
    sheet.save(out)
    print("sheet: %s" % out)


if __name__ == "__main__":
    main()
