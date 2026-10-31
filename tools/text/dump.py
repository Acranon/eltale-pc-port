"""Dump the game's Japanese text from the user's ROM into a working file (never committed).

    python tools/text/dump.py rom.z64 [out/text/script.tsv]

Each line: id, ROM offset, text (newlines as ⏎). Ids are stable: the block name and the string's
offset in the block. Sources so far:
- npc:  NPC messages (D_D2E2F0, read by Npc_LoadMessage at an NPC def's message offset);
- spot: signs and examined spots (D_D37700, TextSpot_LoadMessage: a name and a text per spot).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from charmap import decode

BLOCKS = [
    ("npc", 0xD2E2F0, 0xD37700),
    ("spot", 0xD37700, 0xD384B0),
]


def strings(rom, start, end):
    i = start
    while i < end:
        if rom[i] == 0xFF:
            i += 1
            continue
        text, j = decode(rom, i)
        yield i, text
        i = j


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "out/text/script.tsv"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    n = 0
    with open(out, "w", encoding="utf-8") as f:
        for name, start, end in BLOCKS:
            for at, text in strings(rom, start, end):
                if not text.strip("　\n"):
                    continue
                f.write("%s.%04X\t%06X\t%s\n" % (name, at - start, at, text.replace("\n", "⏎")))
                n += 1
    print("%d strings -> %s" % (n, out))


if __name__ == "__main__":
    main()
