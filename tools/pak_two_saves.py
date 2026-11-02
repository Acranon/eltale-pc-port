#!/usr/bin/env python3
"""A Controller Pak image with two saves: note 0 of IN copied into note 1 (new pages) -> OUT.

    python3 tools/pak_two_saves.py IN.mpk OUT.mpk [TAILBYTE]

TAILBYTE fills bytes 0x68..0x80 of the copy's file (what the retail header write leaves after the 0x68-byte header).
The memory-safety sweep loads the first save from such a Pak (docs/MEMORY_SAFETY.md M3: with two saves the save
menu's file list overwrote the first save's stats on Windows). The image is a plain 32 KiB N64 Controller Pak."""
import sys

pak = bytearray(open(sys.argv[1], "rb").read())
assert len(pak) == 32768
PAGE = 256


def inode(p):
    return pak[0x100 + p * 2] << 8 | pak[0x101 + p * 2]


def inode_set(p, v):
    for base in (0x100, 0x200):
        pak[base + p * 2] = v >> 8
        pak[base + p * 2 + 1] = v & 0xFF


e0 = pak[0x300:0x320]
assert any(e0[:4]), "note 0 empty"
assert not any(pak[0x320:0x324]), "note 1 used"
pages = []
p = e0[7]
while True:
    pages.append(p)
    n = inode(p)
    if n == 1:
        break
    p = n
free = [i for i in range(5, 128) if inode(i) == 3]
new = free[:len(pages)]
for src, dst in zip(pages, new):
    pak[dst * PAGE:(dst + 1) * PAGE] = pak[src * PAGE:(src + 1) * PAGE]
for i, dst in enumerate(new):
    inode_set(dst, new[i + 1] if i + 1 < len(new) else 1)
s = sum(pak[0x100 + 10:0x200]) & 0xFF
pak[0x101] = pak[0x201] = s
e1 = bytearray(e0)
e1[6], e1[7] = 0, new[0]
e1[12] = (e1[12] + 1) & 0xFF
pak[0x320:0x340] = e1
if len(sys.argv) > 3:
    off = new[0] * PAGE
    for i in range(0x68, 0x80):
        pak[off + i] = int(sys.argv[3], 0)
open(sys.argv[2], "wb").write(pak)
print("note 0 pages", pages, "-> note 1 pages", new, "tail of note 0:", pak[pages[0] * PAGE + 0x68:pages[0] * PAGE + 0x80].hex())
