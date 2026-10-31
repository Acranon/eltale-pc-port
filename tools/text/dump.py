"""Dump the game's Japanese text from the user's ROM into a working file (never committed).

    python tools/text/dump.py rom.z64 [out/text/script.tsv]

Each line: id, ROM offset, kind, text (newlines as ⏎). Ids are stable: a source name and the
string's offset in that source. Sources:
- npc:     NPC messages (D_D2E2F0, Npc_LoadMessage at an NpcDef message offset);
- spot:    signs and examined spots (D_D37700, TextSpot_LoadMessage: a name, then a text);
- name:    speaker names in the NPC area-set blocks (NpcDef.name) and the shared NPC block;
- ending:  the ending's scenes (D_E420D0, SceneMessage text and speaker names);
- item:    item names and descriptions (common block, D_803AB288 and the descriptions after);
- boss:    boss names and their lines before a fight (common block, D_803AC418/438);
- ui:      strings in the code's data, from the decomp's assets.yaml ("message text").
Text drawn as images (logos, prologue and epilogue pages, menu labels) is not here.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from charmap import PAGES, decode

# (name, start, end): every string in the range, in order
SEQUENTIAL = [
    ("npc", 0xD2E2F0, 0xD37700),
    ("spot", 0xD37700, 0xD384B0),

]
# (name, start, end): ranges that mix strings with other data: strings found by their shape
SCANNED = [
    ("item", 0xD848DC, 0xD84F97),
    ("ending", 0xE420D0, 0xE43780),
    ("boss", 0xD85780, 0xD86170),
]
# speaker names: "A0 C0 ... FF" in these ranges
NAME_RANGES = [(0x871D10, 0xA70350), (0xA70350, 0xA77F10)]
OK_CONTROLS = {0xE0, 0xF0, 0xF2}


def valid(rom, i, end, limit=0x4B0):
    """The end (0xFF) of a well-formed string at i, and its glyph count, or None."""
    page, glyphs, j = 0, 0, i
    while j < end and j - i < limit:
        b = rom[j]
        if b == 0xFF:
            return j, glyphs
        if b < 0x7F:
            if page >= len(PAGES) or b >= len(PAGES[page]):
                return None
            glyphs += 1
        elif 0x80 <= b < 0xA0:
            page = b & 0x1F
        elif b >= 0xE0 and b not in OK_CONTROLS:
            return None
        j += 1
    return None


def sequential(rom, start, end):
    i = start
    while i < end:
        if rom[i] == 0xFF:
            i += 1
            continue
        text, j = decode(rom, i)
        yield i, text
        i = j


def scanned(rom, start, end):
    i = start
    while i < end:
        if 0x80 <= rom[i] < 0xE0:
            r = valid(rom, i, end)
            if r and r[1] >= 2 and r[1] >= 0.4 * (r[0] - i):
                yield i, decode(rom, i)[0]
                i = r[0] + 1
                continue
        i += 1


def names(rom, start, end):
    i = start
    while True:
        i = rom.find(b"\xa0\xc0", i, end)
        if i < 0:
            return
        r = valid(rom, i, end, 0x24)
        if r and r[1] >= 1 and all(b < 0xA0 for b in rom[i + 2:r[0]]):
            yield i, decode(rom, i)[0]
            i = r[0]
        i += 1


def ui_assets(rom):
    """assets.yaml entries marked "message text": each may hold several strings."""
    yaml = open(os.path.join(HERE, "..", "..", "decomp", "assets.yaml"), encoding="utf-8").read()
    for m in re.finditer(r"name: (\w+), rom: (0x[0-9A-Fa-f]+), size: (0x[0-9A-Fa-f]+).*# message text", yaml):
        name, at, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        i, end = at, at + size
        while i < end:
            if rom[i] in (0x00, 0xFF) or (i > at and rom[i] < 0x80):
                i += 1
                continue
            r = valid(rom, i, end + 1)
            if not r:
                i += 1
                continue
            yield name, at, i, decode(rom, i)[0]
            i = r[0] + 1


def main():
    rom = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "out/text/script.tsv"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    rows = []
    for name, start, end in SEQUENTIAL:
        rows += [("%s.%04X" % (name, i - start), i, name, t) for i, t in sequential(rom, start, end)]
    for name, start, end in SCANNED:
        rows += [("%s.%04X" % (name, i - start), i, name, t) for i, t in scanned(rom, start, end)]
    for start, end in NAME_RANGES:
        for i, t in names(rom, start, end):
            rows.append(("name.%06X" % i, i, "name", t))
    for asset, at, i, t in ui_assets(rom):
        rows.append(("ui.%s.%02X" % (asset, i - at), i, "ui", t))
    n = 0
    seen = set()
    junk = re.compile(r"\{(\d+\.\d+|E.|F[013-9A-F])\}")
    with open(out, "w", encoding="utf-8") as f:
        for rid, at, kind, text in rows:
            # one row per ROM string; skip padding and data read as text
            if at in seen or junk.search(text) or not re.search("[぀-ヿ一-鿿『]", text) and not re.fullmatch("[A-Za-z!.　]+", text):
                continue
            seen.add(at)
            f.write("%s\t%06X\t%s\t%s\n" % (rid, at, kind, text.replace("\n", "⏎")))
            n += 1
    print("%d strings -> %s" % (n, out))


if __name__ == "__main__":
    main()
