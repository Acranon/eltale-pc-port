"""Build the port's English script file from translations/en.v2.tsv.

    python tools/text/runtime_text.py translations/en.v2.tsv decomp/assets.yaml out/.../text/en.tsv

Each output row is "<ROM offset hex>\t<English>": the offset of the Japanese string the row replaces
(from the id, as tools/text/dump.py made it; spell names, which have no Japanese string to
match, get 0x2000000 + element * 16 + index, the "can't use" label 0x20000FF), the English in ASCII with \\n a line break, \\f a page
break and \\2 a Yes/No choice (port/text/en_text.c reads it).
"""
import os
import re
import sys

BASES = {"npc": 0xD2E2F0, "spot": 0xD37700, "item": 0xD848DC, "ending": 0xE420D0, "boss": 0xD85780}


def main():
    src, yaml_path, out = sys.argv[1:4]
    yaml = open(yaml_path, encoding="utf-8").read()
    assets = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"name: (\w+), rom: (0x[0-9A-Fa-f]+)", yaml)}
    rows = []
    for line in open(src, encoding="utf-8"):
        rid, text = line.rstrip("\n").split("\t", 1)
        kind, rest = rid.split(".", 1)
        if kind in BASES:
            at = BASES[kind] + int(rest, 16)
        elif kind == "name":
            at = int(rest, 16)
        elif kind == "spell":
            # spell names (the battle HUD's own font, not a ROM string): 0x2000000 + element * 16 + index
            at = 0x20000FF if rest == "locked" else 0x2000000 + int(rest.split(".")[0]) * 16 + int(rest.split(".")[1])
        elif kind == "ui":
            asset, off = rest.rsplit(".", 1)
            at = assets[asset] + int(off, 16)
        else:
            raise SystemExit("unknown id " + rid)
        text = (text.replace("\\", "\\\\").replace("\u23ce\u25bc\u23ce", "\\f").replace("\u23ce", "\\n")
                .replace("{F2}", "\\2"))
        if any(ord(c) > 126 for c in text):
            raise SystemExit("%s: non-ASCII text" % rid)
        rows.append("%06X\t%s\n" % (at, text))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.writelines(rows)
    print("%d strings -> %s" % (len(rows), out))


if __name__ == "__main__":
    main()
