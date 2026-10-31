"""Pixel widths of English lines in the game's font, for rows matching a prefix.

    python tools/text/widths.py rom.z64 item. [ui.save_menu]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import enfont

rom = open(sys.argv[1], "rb").read()
font = enfont.Font(rom)
for l in open(os.path.join(HERE, "..", "..", "translations", "en.v2.tsv"), encoding="utf-8"):
    rid, text = l.rstrip("\n").split("\t", 1)
    if not any(rid.startswith(p) for p in sys.argv[2:]):
        continue
    text = enfont.smart_quotes(text).replace("{F2}", "")
    for line in text.replace("⏎▼⏎", "⏎").split("⏎"):
        print("%4d  %-28s %s" % (sum(font.width(c) for c in line), rid, line))
