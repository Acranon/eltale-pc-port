#!/usr/bin/env python3
"""Generate a host-width-safe copy of the pinned SDK's display-list header.

The N64 SDK uses `long` for the matrix words and `unsigned long` for five
32-bit packet words. On a 64-bit host those double their sizes. The
generated header stays in out/; no SDK source is copied into this repo.
"""

from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = root / "decomp/include/ultra64/PR/gbi.h"
dst = root / "out/p0/include/PR/gbi.h"
text = src.read_text()
assert text.count("unsigned long\tcolor;") == 1
assert sum(text.count(f"unsigned long w{i};") for i in range(4)) == 4
assert text.count("typedef long\tMtx_t[4][4];") == 1
text = text.replace("typedef long\tMtx_t[4][4];", "typedef s32\tMtx_t[4][4];")
text = text.replace("unsigned long\tcolor;", "u32\tcolor;")
for i in range(4):
    text = text.replace(f"unsigned long w{i};", f"u32 w{i};")
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text(text)
print(dst.relative_to(root))
