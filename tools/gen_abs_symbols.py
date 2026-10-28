#!/usr/bin/env python3
"""Give unresolved raw D_ labels their N64 numeric address in the P0 host link.

These are linker labels, not copied ROM content. Named runtime globals must be
provided by C code or an explicit P0 stand-in instead of this generator.
"""

import re
import subprocess
import sys
from pathlib import Path

raw_address = re.compile(r"D_([0-9A-F]{5,8})$")
dst = Path(sys.argv[1])
objects = sys.argv[2:]
undefined = set()
defined = set()
for obj in objects:
    output = subprocess.check_output(["nm", "-g", obj], text=True)
    for line in output.splitlines():
        words = line.split()
        if len(words) == 2 and words[0] == "U":
            undefined.add(words[1])
        elif len(words) == 3 and words[1] != "U":
            defined.add(words[2])
symbols = sorted(s for s in undefined - defined if raw_address.fullmatch(s))
dst.write_text("".join(f"-Wl,--defsym,{s}=0x{raw_address.fullmatch(s).group(1)}\n"
                       for s in symbols))
print(f"{dst}: {len(symbols)} fixed N64 linker labels")
