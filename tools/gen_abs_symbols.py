#!/usr/bin/env python3
"""Give unresolved raw D_ labels their N64 numeric address in the P0 host link.

These are linker labels, not copied ROM content. Named runtime globals must be
provided by C code or an explicit P0 stand-in instead of this generator.

    gen_abs_symbols.py OUT.rsp OBJ...           GNU ld: -Wl,--defsym lines
    gen_abs_symbols.py --asm OUT.s OBJ...       an assembly file of .set lines (MinGW)
    gen_abs_symbols.py --macho OUT.s OBJ...     the same for arm64 macOS: Mach-O names (a leading _) and
                                                RDRAM in its 4 GiB window (PORT_ADDR_HI, read from
                                                port/include/port/addr.h; docs/MACOS.md)
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

raw_address = re.compile(r"D_([0-9A-F]{5,8})(?:_end)?$")
macho = len(sys.argv) > 1 and sys.argv[1] == "--macho"
asm = len(sys.argv) > 1 and sys.argv[1] in ("--asm", "--macho")
args = sys.argv[2:] if asm else sys.argv[1:]
prefix = "_" if macho else ""  # Mach-O C names
addr_hi = 0
if macho:  # PORT_ADDR_HI, as the C code has it
    addr_h = Path(__file__).resolve().parent.parent / "port" / "include" / "port" / "addr.h"
    m = re.search(r"#define PORT_ADDR_HI (0x[0-9A-Fa-f]+)ull", addr_h.read_text())
    if not m:
        sys.exit("gen_abs_symbols: no PORT_ADDR_HI in port/include/port/addr.h")
    addr_hi = int(m.group(1), 16)
dst = Path(args[0])
objects = args[1:]
undefined = set()
defined = set()
nm = shutil.which("llvm-nm") or shutil.which("nm") or "nm"
for obj in objects:
    output = subprocess.check_output([nm, "-g", obj], text=True)
    for line in output.splitlines():
        words = line.split()
        if len(words) == 2 and words[0] == "U" and words[1].startswith(prefix):
            undefined.add(words[1][len(prefix):])
        elif len(words) == 3 and words[1] != "U" and words[2].startswith(prefix):
            defined.add(words[2][len(prefix):])
symbols = sorted(s for s in undefined - defined if raw_address.fullmatch(s))
# N64 objects the port keeps natively: a raw label into one would write or read RDRAM the renderer
# never sees (camera.c's guOrtho once filled RDRAM 0x80301000 instead of port_gfx_frames)
NATIVE_ONLY = [(0x80301000, 0x8031C5A0, "the graphics frames (port_gfx_frames, port/boot_probe.h)")]
bad = [(s, why) for s in symbols for lo, hi, why in NATIVE_ONLY
       if lo <= int(raw_address.fullmatch(s).group(1), 16) < hi]
# Labels the decomp defines as an offset into a named object (undefined_syms.txt `D_x = name + off`):
# that object is native on the host, so the raw address is RDRAM the game never wrote (Sfx_StopChannel4
# once read sfx_channel_sound[4] from RDRAM 0x800559E4 and indexed the sound ids with garbage)
alias = re.compile(r"(D_[0-9A-F]{5,8})\s*=\s*([A-Za-z_]\w*)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)\s*;")
syms_txt = Path(__file__).resolve().parent.parent / "decomp" / "undefined_syms.txt"
aliases = {}
if syms_txt.exists():
    for line in syms_txt.read_text().splitlines():
        m = alias.match(line.strip())
        if m:
            aliases[m.group(1)] = f"{m.group(2)} + {m.group(3)}"
bad += [(s, f"an alias of the native {aliases[s]} (undefined_syms.txt): map it to that object")
        for s in symbols if s in aliases]
if bad:
    sys.exit("".join(f"gen_abs_symbols: {s} is {why}: declare it through the port's native object\n" for s, why in bad))
if macho:
    dst.write_text("".join(f".globl {prefix}{s}\n.set {prefix}{s}, {addr_hi + int(raw_address.fullmatch(s).group(1), 16):#x}\n"
                           for s in symbols))
elif asm:
    dst.write_text("".join(f".globl {s}\n.set {s}, 0x{raw_address.fullmatch(s).group(1)}\n"
                           for s in symbols))
else:
    dst.write_text("".join(f"-Wl,--defsym,{s}=0x{raw_address.fullmatch(s).group(1)}\n"
                           for s in symbols))
print(f"{dst}: {len(symbols)} fixed N64 linker labels")
