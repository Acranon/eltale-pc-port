#!/usr/bin/env python3
"""Generate host-data-address <-> N64-address maps for display lists.

The game keeps host pointers in 32-bit Gfx words (the image is linked below
4 GiB). Port_NormalizeDlAddress maps those words back to pinned decomp symbol
addresses (deterministic cross-platform hashes, and the frame image).
Port_N64ToNative goes the other way, for display lists in the ROM that name a
native object by its fixed N64 address (the backdrop's texture and palette): a
symbol extends to the next symbol's N64 address.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1])
objects = sys.argv[2:]
symbols = dict(re.findall(r"^([A-Za-z_]\w*) = (0x[0-9A-Fa-f]+);",
                          (ROOT / "decomp/symbol_addrs.txt").read_text(), re.M))
raw = re.compile(r"D_([0-9A-F]{5,8})(?:_ovl_\w+)?$")
nm = shutil.which("llvm-nm") or shutil.which("nm") or "nm"
defined = set()
for obj in objects:
    output = subprocess.check_output([nm, "-g", obj], text=True)
    for line in output.splitlines():
        words = line.split()
        if len(words) == 3 and words[1].upper() in {"B", "C", "D", "R", "S", "G"}:
            defined.add(words[2])
entries = []
for name in sorted(defined):
    address = symbols.get(name)
    if address is None:
        match = raw.fullmatch(name)
        if match:
            address = "0x" + match.group(1)
    if address is not None:
        entries.append((name, address))

# Port_N64ToNative covers the main segment's .bss only: what is there exists only at run time, so
# RDRAM never holds it, while the rest of RDRAM (the IPL copy, DMA'd blocks) is N64 data that
# display lists in the ROM read as it is.
config = (ROOT / "decomp/eltale.jp.yaml").read_text()
bss_start = int(re.search(r"type: \.bss, vram: (0x[0-9A-Fa-f]+)", config).group(1), 16)
bss_end = bss_start + int(re.search(r"bss_size: (0x[0-9A-Fa-f]+)", config).group(1), 16)
reverse = [(n, a) for n, a in entries if bss_start <= int(a, 16) < bss_end]

lines = [
    "#include <stdint.h>",
    "#include <stdlib.h>",
    "typedef struct { uintptr_t host; uint32_t n64; } PortAddressEntry;",
]
lines.extend(f"extern char {name}[];" for name, _ in entries)
lines.append("static PortAddressEntry addresses[] = {")
lines.extend(f"    {{ (uintptr_t){name}, {address}u }}," for name, address in entries)
lines += [
    "};",
    "static int compare_address(const void *a, const void *b) {",
    "    uintptr_t x = ((const PortAddressEntry *)a)->host;",
    "    uintptr_t y = ((const PortAddressEntry *)b)->host;",
    "    return (x > y) - (x < y);",
    "}",
    "uint32_t Port_NormalizeDlAddress(uint32_t word) {",
    "    static int sorted;",
    "    const size_t count = sizeof addresses / sizeof addresses[0];",
    "    if (word >= 0x00600000u && word < 0x00800000u) return word;",
    "    if (!sorted) { qsort(addresses, count, sizeof addresses[0], compare_address); sorted = 1; }",
    "    uintptr_t target = word;",
    "    size_t lo = 0, hi = count;",
    "    while (lo < hi) {",
    "        size_t mid = lo + (hi - lo) / 2;",
    "        if (addresses[mid].host <= target) lo = mid + 1; else hi = mid;",
    "    }",
    "    if (!lo) return word;",
    "    size_t index = lo - 1;",
    "    uintptr_t delta = target - addresses[index].host;",
    "    if (delta > 0x200000u) return word;",
    "    return addresses[index].n64 + (uint32_t)delta;",
    "}",
    "static PortAddressEntry by_n64[] = {",
] + [f"    {{ (uintptr_t){name}, {address}u }}," for name, address in reverse] + [
    "};",
    f"#define BSS_END 0x{bss_end:08X}u",
    "static int compare_n64(const void *a, const void *b) {",
    "    uint32_t x = ((const PortAddressEntry *)a)->n64, y = ((const PortAddressEntry *)b)->n64;",
    "    return (x > y) - (x < y);",
    "}",
    "uintptr_t Port_N64ToNative(uint32_t n64) {",
    "    static int sorted;",
    "    const size_t count = sizeof by_n64 / sizeof by_n64[0];",
    "    if (!sorted) {",
    "        qsort(by_n64, count, sizeof by_n64[0], compare_n64);",
    "        sorted = 1;",
    "    }",
    "    size_t lo = 0, hi = count;",
    "    while (lo < hi) {",
    "        size_t mid = lo + (hi - lo) / 2;",
    "        if (by_n64[mid].n64 <= n64) lo = mid + 1; else hi = mid;",
    "    }",
    "    if (!lo) return 0;",
    "    const PortAddressEntry *e = &by_n64[lo - 1];",
    "    uint32_t end = lo < count ? by_n64[lo].n64 : BSS_END;",
    "    if (e->host == e->n64 || n64 - e->n64 >= end - e->n64) return 0;",
    "    return e->host + (n64 - e->n64);",
    "}",
]
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(lines) + "\n")
print(f"{out}: {len(entries)} data symbols")
