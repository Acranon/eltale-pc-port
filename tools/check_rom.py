#!/usr/bin/env python3
"""Verify the user-supplied Eltale Monsters (J) z64 ROM before a port run."""

import hashlib
import sys
from pathlib import Path

EXPECTED = "50ea6c3d4de4b68b3c9b77524d8177066b9f36e7"

if len(sys.argv) != 2:
    raise SystemExit("usage: check_rom.py path/to/baserom.jp.z64")
path = Path(sys.argv[1])
digest = hashlib.sha1()
with path.open("rb") as f:
    for block in iter(lambda: f.read(1024 * 1024), b""):
        digest.update(block)
actual = digest.hexdigest()
if actual != EXPECTED:
    raise SystemExit(f"wrong ROM SHA1: {actual}")
print(f"ROM SHA1 OK: {path}")
