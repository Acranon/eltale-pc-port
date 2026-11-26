#!/usr/bin/env python3
"""Print the ld64 flags that link a Mach-O executable again with its image at BASE.

    python3 tools/macho_segaddr.py PASS1_EXECUTABLE BASE

arm64 macOS (docs/MACOS.md): the game's image must start at 0x8_10000000 (PORT_ADDR_HI + 0x10000000),
but ld64 ignores -image_base for a position-independent executable, and -segaddr wants an address for
every segment, in order. So the executable is linked once as usual, and this reads its segments
(`otool -l`) and moves each by the same amount: `-Wl,-segaddr,__TEXT,0x810000000 -Wl,-segaddr,...`.
The second link gives the same segment sizes, so the layout holds. __PAGEZERO stays where it is.
"""
import re
import subprocess
import sys


def segments(path):
    out = subprocess.run(["otool", "-l", path], capture_output=True, text=True, check=True).stdout
    segs = []
    name = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("segname "):
            name = line.split()[1]
        elif line.startswith("vmaddr ") and name is not None:
            segs.append((name, int(line.split()[1], 16)))
            name = None
    return segs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, base = sys.argv[1], int(sys.argv[2], 0)
    segs = [(n, a) for n, a in segments(path) if n != "__PAGEZERO"]
    text = dict(segs).get("__TEXT")
    if text is None:
        sys.exit(f"macho_segaddr: no __TEXT segment in {path}")
    delta = base - text
    if base & 0x3FFF:
        sys.exit("macho_segaddr: BASE must be 16 KiB aligned")
    print(" ".join(f"-Wl,-segaddr,{n},{a + delta:#x}" for n, a in segs if re.fullmatch(r"__\w+", n)))


if __name__ == "__main__":
    main()
