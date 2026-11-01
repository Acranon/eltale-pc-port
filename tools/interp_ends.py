#!/usr/bin/env python3
"""Checks that pictures between ticks start and end on the ticks' own pictures (R6, docs/renderer/NATIVE_DRAW.md
"Pictures between ticks"): in a --dump-frames folder made with ELTALE_DUMP_BETWEEN=N and ELTALE_INTERP_ENDS=1,
ends_<a>_0.png (the interpolator at t = 0) must equal frame_<a>.png and ends_<a>_1.png (t = 1) frame_<b>.png, b
the next tick dumped after a.

    python tools/interp_ends.py FOLDER

Prints one line per pair that differs (the pixels that differ) and a summary; exits 1 if any differ.
Needs Pillow and numpy.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def main():
    folder = sys.argv[1]
    names = os.listdir(folder)
    ticks = sorted(int(m.group(1)) for m in (re.match(r"frame_(\d+)\.png$", n) for n in names) if m)
    checked = differ = 0
    for n in sorted(names):
        m = re.match(r"ends_(\d+)_([01])\.png$", n)
        if not m:
            continue
        a, end = int(m.group(1)), int(m.group(2))
        if end == 0:
            want = a
        else:
            later = [t for t in ticks if t > a]
            if not later:
                continue
            want = later[0]
        ref = os.path.join(folder, "frame_%d.png" % want)
        if not os.path.exists(ref):
            continue
        d = int((np.abs(load(os.path.join(folder, n)) - load(ref)).max(axis=2) > 0).sum())
        checked += 1
        if d:
            differ += 1
            print("%s vs frame_%d.png: %d pixels differ" % (n, want, d))
    print("interp_ends: %d pairs checked, %d differ" % (checked, differ))
    sys.exit(1 if differ or not checked else 0)


if __name__ == "__main__":
    main()
