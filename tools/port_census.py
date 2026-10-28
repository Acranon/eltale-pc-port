#!/usr/bin/env python3
"""List source sites needing 64-bit address or big-endian review.

This is a candidate census, not an automatic safety proof. It deliberately
records file/line/category without copying game text or binary data.
"""

import json
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
source = root / "work/src" if (root / "work/src").exists() else root / "decomp/src"
patterns = {
    "rom_dma": re.compile(r"\b(?:Dma_ReadRom|osPiStartDma)\s*\("),
    "save_io": re.compile(r"\b(?:osPfsReadWriteFile|osEepromRead|osEepromWrite)\s*\("),
    "fixed_address_cast": re.compile(r"\(\s*[^)]*\*\s*\)\s*0x(?:8|A)[0-9A-Fa-f]{7}"),
    "pointer_to_word_candidate": re.compile(r"\(\s*(?:u32|s32)\s*\)\s*(?:&|[A-Za-z_])"),
    "raw_multibyte_view": re.compile(r"\(\s*(?:u16|s16|u32|s32|f32)\s*\*\s*\)\s*(?:D_|[A-Za-z_])"),
}
sites = []
for subtree in ("game", "overlays"):
    for path in sorted((source / subtree).rglob("*.c")):
        rel = path.relative_to(source).as_posix()
        for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for category, pattern in patterns.items():
                if pattern.search(line):
                    sites.append({"category": category,
                                  "file": rel,
                                  "line": lineno,
                                  "p0_linked": (rel.startswith("game/") and rel != "game/audio/audio.c")
                                  or rel.startswith("overlays/intro/")})
print(json.dumps({"source": source.relative_to(root).as_posix(), "sites": sites}, indent=2))
