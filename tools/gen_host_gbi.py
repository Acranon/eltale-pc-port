#!/usr/bin/env python3
"""Generate a host-width-safe copy of the pinned SDK's display-list header.

The N64 SDK uses `long` for the matrix words and `unsigned long` for five
32-bit packet words. Windows Clang also widens two mixed bitfield packet
views. The generator keeps their host representations at N64 widths. The
generated header stays in out/; no SDK source is copied into this repo.
"""

from pathlib import Path
import sys
import re

root = Path(__file__).resolve().parent.parent
src = root / "decomp/include/ultra64/PR/gbi.h"
dst = root / (sys.argv[1] if len(sys.argv) > 1 else "out/p0/include/PR/gbi.h")
text = src.read_text()
assert text.count("unsigned long\tcolor;") == 1
assert sum(text.count(f"unsigned long w{i};") for i in range(4)) == 4
assert text.count("typedef long\tMtx_t[4][4];") == 1
text = text.replace("typedef long\tMtx_t[4][4];", "typedef s32\tMtx_t[4][4];")
text = text.replace("unsigned long\tcolor;", "u32\tcolor;")
for i in range(4):
    text = text.replace(f"unsigned long w{i};", f"u32 w{i};")
# Clang's Windows ABI widens these mixed-type bitfield views to 12 bytes.
# The GBI macros address packet words, not these individual union views.
packets = {
    "Gpopmtx": r"typedef struct \{\s*int\s+cmd:8;\s*int\s+pad1:24;\s*int\s+pad2:24;\s*unsigned char\s+param:8;\s*\} Gpopmtx;",
    "Gsetcolor": r"typedef struct \{\s*int\s+cmd:8;\s*unsigned char\s+pad;\s*unsigned char\s+prim_min_level;\s*unsigned char\s+prim_level;\s*u32\s+color;\s*\} Gsetcolor;",
}
for packet, pattern in packets.items():
    text, count = re.subn(pattern, f"typedef struct {{ u32 w0; u32 w1; }} {packet};", text)
    assert count == 1, packet
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text(text)
print(dst.relative_to(root))
