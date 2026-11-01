#!/usr/bin/env python3
"""Compare two sets of 320x240 pictures (a renderer against the paraLLEl-RDP reference) at 1x.

    python tools/render_compare.py REF_DIR TEST_DIR [--out DIFF_DIR] [--name TEST]
                                   [--baseline tests/render_baselines/TEST.txt [--write-baseline]]

REF and TEST are folders of PNGs matched by file name (*.vi.png are skipped unless --vi), or two
PNG files. The rule (docs/renderer/REFERENCE.md):
  - each channel is quantised to 5 bits (the N64 framebuffer is RGBA5551);
  - a pixel differs when any channel is more than 1 step away (the RDP dithers, a modern renderer
    need not);
  - only differing pixels whose whole 3x3 neighbourhood differs count ("solid" areas), so single
    pixels along edges (edge rules, coverage) don't.
Per frame it prints the differing pixels (raw and solid), then the sum and the worst frame. With
--out it writes a diff picture per frame that differs: the test picture greyed, solid differences
red, other differences yellow.

A baseline is numbers only (never pictures): "frame solid_px" lines and a "sum" line. With
--baseline the run fails (exit 1) if any frame, or the sum, has more solid pixels than the
baseline allows; --write-baseline writes the current numbers instead.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LEVEL = 1   # 5-bit steps allowed per channel
SOLID = 1   # radius of the neighbourhood that must differ too (1 = 3x3)


def load(path: Path) -> np.ndarray:
    im = Image.open(path).convert("RGB")
    return np.asarray(im, dtype=np.int16) >> 3


def erode(mask: np.ndarray, r: int) -> np.ndarray:
    """True where the whole (2r+1)^2 neighbourhood is True (the picture's edge counts as False)."""
    h, w = mask.shape
    p = np.zeros((h + 2 * r, w + 2 * r), dtype=bool)
    p[r:r + h, r:r + w] = mask
    out = np.ones_like(mask)
    for dy in range(2 * r + 1):
        for dx in range(2 * r + 1):
            out &= p[dy:dy + h, dx:dx + w]
    return out


def compare(a: np.ndarray, b: np.ndarray):
    if a.shape != b.shape:
        raise SystemExit(f"sizes differ: {a.shape} / {b.shape}")
    diff = np.abs(a - b).max(axis=2) > LEVEL
    solid = erode(diff, SOLID)
    return diff, solid


def diff_picture(test_rgb: np.ndarray, diff: np.ndarray, solid: np.ndarray) -> Image.Image:
    grey = (test_rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)) * 0.4
    out = np.repeat(grey[:, :, None], 3, axis=2).astype(np.uint8)
    out[diff & ~solid] = (255, 220, 0)
    out[solid] = (255, 0, 0)
    return Image.fromarray(out, "RGB")


def pairs(ref: Path, test: Path, vi: bool):
    if ref.is_file() and test.is_file():
        return [(ref.stem, ref, test)]
    out = []
    for r in sorted(ref.glob("*.png")):
        if r.name.endswith(".vi.png") != vi:
            continue
        t = test / r.name
        if t.is_file():
            out.append((r.name[:-len(".vi.png")] if vi else r.stem, r, t))
    return out


def read_baseline(path: Path):
    frames, total = {}, None
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if len(line) != 2:
            continue
        if line[0] == "sum":
            total = int(line[1])
        else:
            frames[line[0]] = int(line[1])
    return frames, total


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref", type=Path)
    ap.add_argument("test", type=Path)
    ap.add_argument("--out", type=Path, help="write diff pictures here")
    ap.add_argument("--name", default="", help="the test's name, for the summary and a new baseline")
    ap.add_argument("--baseline", type=Path, help="numbers file under tests/ to check against (or write)")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--vi", action="store_true", help="compare the VI pictures (*.vi.png) instead")
    args = ap.parse_args()

    ps = pairs(args.ref, args.test, args.vi)
    if not ps:
        print("render_compare: no pictures in common", file=sys.stderr)
        return 2
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for name, r, t in ps:
        a, b = load(r), load(t)
        diff, solid = compare(a, b)
        nd, ns = int(diff.sum()), int(solid.sum())
        results.append((name, nd, ns))
        print(f"RENDER_COMPARE {args.name or '-'} {name} differ={nd} solid={ns}")
        if args.out and nd:
            rgb = np.asarray(Image.open(t).convert("RGB"))
            diff_picture(rgb, diff, solid).save(args.out / f"{name}.diff.png")
    total = sum(s for _, _, s in results)
    worst = max(results, key=lambda x: x[2])
    print(f"RENDER_COMPARE_SUM {args.name or '-'} frames={len(results)} solid={total} "
          f"worst={worst[0]}:{worst[2]} differ={sum(d for _, d, _ in results)}")

    if args.baseline and args.write_baseline:
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        with open(args.baseline, "w", newline="\n") as f:
            f.write(f"# render_compare baseline{': ' + args.name if args.name else ''}: solid differing "
                    f"pixels per frame (3x3 rule, 5-bit, 1 step), and their sum. Numbers only.\n")
            for name, _, s in results:
                f.write(f"{name} {s}\n")
            f.write(f"sum {total}\n")
        print(f"render_compare: wrote {args.baseline}")
        return 0
    if args.baseline:
        base, base_total = read_baseline(args.baseline)
        worse = [(n, s, base[n]) for n, _, s in results if n in base and s > base[n]]
        missing = [n for n, _, _ in results if n not in base]
        for n, s, b in worse:
            print(f"RENDER_COMPARE_WORSE {n} solid={s} baseline={b}")
        if missing:
            print(f"render_compare: not in the baseline: {' '.join(missing)}")
        if base_total is not None and total > base_total:
            print(f"RENDER_COMPARE_WORSE sum solid={total} baseline={base_total}")
            worse.append(("sum", total, base_total))
        print("RENDER_COMPARE_BASELINE " + ("WORSE" if worse else "OK"))
        return 1 if worse else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
