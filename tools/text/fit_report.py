"""Lay every English message out as the port does and report the encoded sizes and page counts.

    python tools/text/fit_report.py rom.z64
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import enfont


def main():
    rom = open(sys.argv[1], "rb").read()
    font = enfont.Font(rom)
    en = [l.rstrip("\n").split("\t", 1) for l in open(os.path.join(HERE, "..", "..", "translations", "en.v2.tsv"), encoding="utf-8")]
    sizes = []
    for rid, text in en:
        pages = enfont.layout(font, text, 232, 3)
        sizes.append((len(enfont.encode(font, pages)), len(pages), rid))
    sizes.sort(reverse=True)
    for s in sizes[:8]:
        print("%5d bytes %3d pages  %s" % s)
    print("over 0x4B0:", sum(s[0] > 0x4B0 for s in sizes))


if __name__ == "__main__":
    main()
