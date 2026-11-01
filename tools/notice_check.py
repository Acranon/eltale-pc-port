#!/usr/bin/env python3
"""Check a notice picture (ELTALE_PRESENT_DUMP's native_notice.png, tests/host_checks.sh's quick save run).

    python tools/notice_check.py out/host_checks/native-notice/native_notice.png

The F2/F4 notices ("Quick saved", "Quick loaded", the flash recorder's) are an ImGui window drawn by the presenter
over the game's picture (port/host/menu.cpp build_hint): at the top left, a font's 0.6 in from the window's corner,
over the HP meters, a darkened box (the dark style's window colour at 60%) holding a line of white text. The
renderer writes the window as shown once a notice is on it. Checked here: the picture is there and not one colour,
and the top-left corner (a quarter of the width, a tenth of the height: above the HP meters' white numbers) holds
the text: enough nearly white pixels (all channels 230 and up) whose box starts near the corner. The lines of text are counted (1:
the notice; 2: the start-up hint "F1 or Back: menu..." above it, while it is still up). The picture is the third
shown with the notice (ImGui sizes the box from the picture before). Prints NOTICE_CHECK OK (with the text's box and
lines) or NOTICE_CHECK FAIL and why; exit 1 on a fail.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"NOTICE_CHECK FAIL no picture {path} (no notice shown, or ELTALE_PRESENT_DUMP not passed)")
        return 1
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    h, w = im.shape[:2]
    if (im == im[0, 0]).all():
        print(f"NOTICE_CHECK FAIL {w}x{h} one colour")
        return 1
    corner = im[: max(h // 10, 40), : w // 4]
    white = (corner >= 230).all(axis=2)
    n = int(white.sum())
    if n < 40:
        print(f"NOTICE_CHECK FAIL {w}x{h}: {n} text pixels in the top-left corner")
        return 1
    ys, xs = np.nonzero(white)
    x0, y0, x1, y1 = int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())
    # the text starts a little in from the corner (the window's padding), not at the picture's own edge
    if x0 > w // 16 or y0 > h // 20:
        print(f"NOTICE_CHECK FAIL {w}x{h}: the text box ({x0},{y0})-({x1},{y1}) is not at the corner")
        return 1
    # lines: runs of rows holding text, split by blank runs of at least 4 rows (gaps inside a line are shorter)
    rows = white.any(axis=1)[y0:y1 + 1]
    lines, blank = 1, 0
    for r in rows:
        if r:
            lines += blank >= 4
            blank = 0
        else:
            blank += 1
    print(f"NOTICE_CHECK OK {w}x{h} text {n} px in ({x0},{y0})-({x1},{y1}) lines={lines}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
