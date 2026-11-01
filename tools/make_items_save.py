#!/usr/bin/env python3
"""Make the "items" quick save (tests/render_frames_2d.txt, run "items") from the battle-stuck one.

    python tools/make_items_save.py [--saves C:/code/eltale/out/bugsaves]

battle-stuck's inventory is empty, so its item menu draws no item icons. This copies <saves>/battle-stuck to
<saves>/items with twelve items in the inventory (the SaveGameStateBlock's inventory bytes, port/p0/quicksave.c
"File") and the file's hash made again. Only item numbers are written: nothing from the ROM.
"""

import argparse
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECORD = 0x18 + 44          # the record after the header and the PC block
INVENTORY = RECORD + 0x80 + 0x30
ITEMS = bytes([5, 1, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11])


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--saves", type=Path, default=ROOT / "out/bugsaves")
    args = ap.parse_args()
    src, dst = args.saves / "battle-stuck", args.saves / "items"
    q = bytearray((src / "quicksaves/quick0.elq").read_bytes())
    if q[:8] != b"ELTQUICK":
        raise SystemExit(f"{src}: not a quick save")
    q[INVENTORY:INVENTORY + 0x97] = ITEMS + b"\xff" * (0x97 - len(ITEMS))
    q[0x10:0x18] = struct.pack("<Q", fnv(bytes(q[0x18:])))
    (dst / "quicksaves").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src / "controller-pak.mpk", dst / "controller-pak.mpk")
    (dst / "quicksaves/quick0.elq").write_bytes(q)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
