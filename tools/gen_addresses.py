#!/usr/bin/env python3
"""Generate constant N64 address words for static display-list packets.

The address map comes from the pinned decomp's symbol table. The emitted
header has symbols and addresses only; it contains no ROM bytes.
"""

import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "out/p0-windows/include/port/addresses.h"
if not out.is_absolute():
    out = root / out
symbols = re.findall(r"^([A-Za-z_]\w*) = (0x[0-9A-Fa-f]+);", (root / "decomp/symbol_addrs.txt").read_text(), re.M)
lines = ["#ifndef ELTALE_PORT_N64_ADDRESSES_H", "#define ELTALE_PORT_N64_ADDRESSES_H", "#define PORT_ADDR(name) PORT_ADDR_##name"]
lines += [f"#define PORT_ADDR_{name} {address}u" for name, address in symbols]
lines.append("#endif")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("\n".join(lines) + "\n")
print(f"{out}: {len(symbols)} address constants")
