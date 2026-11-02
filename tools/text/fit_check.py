#!/usr/bin/env python3
"""Check that every English string fits the window it is drawn in, in every English font.

    python tools/text/fit_check.py ROM.z64 [--font N] [-v]

Each English row of translations/en.v2.tsv is measured with the port's own metrics (port/text/en_text.c:
set 0 is the game's Latin glyphs, measured from the ROM's font pages plus tools/text/enfont.py's extra
page; sets 1-5 are the optional fonts' tables in port/text/en_fonts.h) against the box the game draws it
in (BOXES below: where the port draws each kind of string and how much room the window leaves). Text the
game wraps (the item descriptions, every message window) is wrapped as en_text.c wraps it; the rest is
drawn as it is, its lines broken only where the row has a line break.

Prints FIT_OVER <font> <row> <what> for each string that does not fit, and FIT_CHECK_OK or
FIT_CHECK_FAIL (exit status 1). -v also prints the room each string leaves.

Keep boxes() in step with the game: where a window grows for English (Port_TextFit / Port_TextGrowBox in
port/text/en_text.c, used by item.c, save_menu.c and title_draw.c), the room here is the most the grown
window can give.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, HERE)
import enfont

SDOT, OPEN, CLOSE = 1, 3, 4
FONT_NAMES = ["Original", "Royalty Mono", "Quest Square", "Pixel Sans", "Retro Sans", "Rune Mono"]


class FontSet:
    """One English font set: advance per logical character, space, glyph height, line height."""

    def __init__(self, name, height, line, space, adv):
        self.name, self.height, self.line, self.space, self.adv = name, height, line, space, adv

    def char_width(self, code):
        return self.adv.get(code, 0)


def original_set(rom):
    f = enfont.Font(rom)
    adv = {}
    for ch, (_, w) in f.metrics.items():
        code = {"“": OPEN, "”": CLOSE, "\x01": SDOT}.get(ch, ord(ch) if len(ch) == 1 else None)
        if code is not None and code < 128:
            adv[code] = w
    return FontSet(FONT_NAMES[0], 13, 14, enfont.SPACE, adv)


def ttf_sets():
    text = open(os.path.join(ROOT, "port", "text", "en_fonts.h"), encoding="ascii").read()
    sets = []
    for m in re.finditer(r'\{ "([^"]+)", (\d+), (\d+), (\d+), (\d+), en_font\d+_texels, \{ (.*?) \} \},', text):
        adv = {}
        for g in re.finditer(r"\[(\d+)\] = \{ (\d+), (\d+), (\d+), (\d+), (\d+) \}", m.group(6)):
            c = int(g.group(1))
            adv[{1: OPEN, 2: CLOSE}.get(c, c)] = int(g.group(6))
        adv.setdefault(OPEN, adv.get(ord('"'), 0))
        adv.setdefault(CLOSE, adv.get(ord('"'), 0))
        adv[SDOT] = adv.get(ord("."), 0)
        sets.append(FontSet(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), adv))
    return sets


def runtime(text):
    """The row as runtime_text.py writes it: \\n line, \\f page, \\x02 choice."""
    return text.replace("⏎▼⏎", "\f").replace("⏎", "\n").replace("{F2}", "\x02")


def codes(text):
    """Logical characters as en_text.c code_at() sees them (spaced dots in "...", curly quotes)."""
    out = []
    for i, c in enumerate(text):
        if c == "." and text[i + 1:i + 2] == ".":
            out.append(SDOT)
        elif c == '"':
            prev = text[i - 1] if i else " "
            out.append(OPEN if i == 0 or prev in " \n\f(*-" else CLOSE)
        else:
            out.append(ord(c))
    return out


def lines_of(f, text, width=None):
    """Pages of lines (each line its pixel width) as en_text.c layout() makes them (rows unlimited)."""
    cs = codes(text)
    pages, lines, x, has = [], [], 0, False
    i = 0
    while i < len(cs):
        c = cs[i]
        if c == 12:  # \f
            lines.append(x)
            pages.append(lines)
            lines, x, has = [], 0, False
        elif c == 10:
            lines.append(x)
            x, has = 0, False
        elif c == 32:
            ww, j = 0, i + 1
            while j < len(cs) and cs[j] not in (32, 10, 12):
                ww += f.char_width(cs[j]) if cs[j] != 2 else 0
                j += 1
            if width is not None and has and x + f.space + ww > width:
                lines.append(x)
                x, has = 0, False
            else:
                x += f.space
        elif c != 2:
            x += f.char_width(c)
            has = True
        i += 1
    lines.append(x)
    pages.append(lines)
    return pages


def block_height(f, n):
    return (n - 1) * f.line + f.height if n else 0


# ---- the boxes ----
# Each check: (row id regex, label, kind, params). Kinds:
#   "box":     drawn as it is at a fixed place: (width, height) of the room for the text
#   "wrap":    lines wider than `width` word-wrapped to it (the window grows to hold them): (width, height),
#              height the most room the grown window can give
#   "window":  a message window (wrapped and paged by the port): the longest word must fit `width`
#   "hud":     the battle HUD's 6-pixel Latin letters in a panel `width` wide
#   "pak":     the Controller Pak manager's message rows (below)
# Room = the window's inside less the text's inset; heights: (lines - 1) * line height + glyph height
# (one-line rows: 15, the tallest font's glyphs; the rows they share a window with are 13 apart).
ONE = 15

# save_menu.c (English): a message wraps at SAVE_EN_TEXT_W and its panel grows about its centre, 6 pixels
# around the text (5 below), inside the screen less 6: at most 308 x 228
SAVE_EN_TEXT_W = 296
SAVE_MESSAGES = ["D_8006C190", "D_8006C1BC", "D_8006C1E0", "D_8006C258", "D_8006C280", "D_8006C2CC", "D_8006C370",
                 "D_8006C390", "D_8006C3B8", "D_8006C408", "D_8006C458", "D_8006C4C8", "D_8006C50C", "D_8006C564",
                 "D_8006C5F8", "D_8006C63C", "D_8006C658", "D_8006C678", "D_8006C6CC", "D_8006C858", "D_8006C880"]
# item.c (English): the description frame ITEM_EN_X = 24, ITEM_EN_W = 272; the name at frame x + 47, the
# description at frame x + 6 wrapped to ITEM_EN_W - 9; the frame grows down and the item row with it (at
# most 7 lines of the original font keep the item row above the HUD's elements)
ITEM_EN_W = 272


def boxes():
    out = []
    for name in SAVE_MESSAGES:
        out.append((r"ui\.%s\.\d+$" % name, "save menu message", "wrap", (SAVE_EN_TEXT_W, 228 - 11)))
    out.append((r"ui\.save_menu_operation_panel_message_\d\.00$", "save menu operation panel", "wrap",
                (SAVE_EN_TEXT_W, 228 - 11)))
    # the choice panel (42x38 at (220, 160), rows at x + 6 and 13 apart, the cursor at x 258) grows to the left
    for name in ("D_8006C16C", "D_8006C174", "D_8006C180", "D_8006C188"):
        out.append((r"ui\.%s\.00$" % name, "save menu choice panel", "box", (150, ONE)))
    # the slot list: the inn name at x 0x51 and "New Data" at x 0x82 in the slot panel (inside edge at x 262)
    out.append((r"ui\.save_menu_slot_map_label_\d+\.00$", "save slot place name", "box", (262 - 3 - 0x51, ONE)))
    out.append((r"ui\.D_80061650\.00$", "save slot New Data", "box", (262 - 3 - 0x82, ONE)))
    # the title screen's message box (title_draw.c, English): wrapped at 296 and the box widened about its
    # centre; the text from y 0x2E down to the Yes / No rows (y 0x73) for states 7 and 8, else the box's bottom
    out.append((r"ui\.title_menu_state[78]_message\.00$", "title message box (with Yes / No)", "wrap",
                (296, 0x73 - 2 - 0x2E)))
    out.append((r"ui\.title_menu_.*_message\.00$", "title message box", "wrap", (296, 141 - 3 - 0x2E)))
    out.append((r"ui\.title_confirmation_row\d_text\.00$", "title Yes/No row", "box", (256 - 3 - 0x94, ONE)))
    # the Controller Pak manager's message box (inside x 47..277, the text at x 0x34; its rows 14 apart, the
    # last one the lower window's buttons): 7 rows, the message in rows 0-3 (row 4 would cross the box's
# bottom edge at y 140), rows 4 and 5 empty, the buttons in row 6
    out.append((r"ui\.pak_menu_.*_message\.00$", "Controller Pak manager message", "pak", (274 - 0x34,)))
    # the item menu
    out.append((r"item\.0(0[0-9A-F]{2}|1[0-9A][0-9A-F])$", "item name (item menu)", "box", (ITEM_EN_W - 3 - 47, ONE)))
    out.append((r"item\.0(1[B-F][0-9A-F]|[2-9A-F][0-9A-F]{2})$", "item description (item menu)", "wrap",
                (ITEM_EN_W - 9, 6 * 14 + 13)))
    # message windows: the narrowest the game opens is 240 wide (text at x + 10, wrapped to width - 20)
    out.append((r"(npc|spot|ending|boss|ui\.(npc_area_entry_message|item_message_|npc_rest_choice|D_8004F2))",
                "message window", "window", (220,)))
    out.append((r"name\.", "speaker name tab", "box", (252 - 73 - 10, ONE)))
    # the battle HUD: spell names centred in the 110-pixel spell panel, monster names in the 110-pixel enemy panel
    out.append((r"(spell|monster)\.", "battle HUD name (110-pixel panel)", "hud", (110 - 4,)))
    # the pause menu's region names (port/p0/pause_menu.c): the HUD font, between the region arrows over the map
    # (150 wide at 4:3, less the two 20-pixel arrows)
    out.append((r"region\.", "pause menu region name (between the arrows)", "hud", (150 - 2 * 20,)))
    return out


def check(sets, rows, verbose=False):
    over = []
    for rid, _ in rows:
        if not any(re.match(b[0], rid) for b in boxes()):
            over.append((-1, rid, "no box: fit_check.py does not know where this row is drawn"))
    for k, f in sets:
        for rid, text in rows:
            for pat, label, kind, params in boxes():
                if not re.match(pat, rid):
                    continue
                t = runtime(text)
                if kind == "hud":
                    need = 6 * len(t)
                    if need > params[0]:
                        over.append((k, rid, "%s: %d px > %d" % (label, need, params[0])))
                    break
                if kind == "pak":
                    lines = t.split("\n")
                    wide = max(lines_of(f, t)[0])
                    if wide > params[0]:
                        over.append((k, rid, "%s: %d px wide > %d" % (label, wide, params[0])))
                    if len(lines) != 7 or lines[4] or lines[5]:
                        over.append((k, rid, "%s: %d rows, not 4 + 2 empty + the buttons' row" % (label, len(lines))))
                    break
                if kind == "window":
                    for word in re.split(r"[ \n\f]", t):
                        ww = sum(f.char_width(c) for c in codes(word) if c != 2)
                        if ww > params[0]:
                            over.append((k, rid, "%s: the word %r is %d px > %d" % (label, word, ww, params[0])))
                    break
                width, height = params
                pages = lines_of(f, t, width if kind == "wrap" else None)
                lines = [w for page in pages for w in page]
                wide = max(lines)
                tall = max(block_height(f, len(page)) for page in pages)
                problems = []
                if wide > width:
                    problems.append("%d px wide > %d" % (wide, width))
                if tall > height:
                    problems.append("%d lines, %d px tall > %d" % (max(len(p) for p in pages), tall, height))
                if problems:
                    over.append((k, rid, "%s: %s" % (label, ", ".join(problems))))
                elif verbose:
                    print("FIT %d %s %s: %d/%d x %d/%d" % (k, rid, label, wide, width, tall, height))
                break
    return over


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rom")
    ap.add_argument("--font", type=int, help="only this font set (0 original, 1-5)")
    ap.add_argument("--tsv", default=os.path.join(ROOT, "translations", "en.v2.tsv"))
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()
    rom = open(args.rom, "rb").read()
    sets = list(enumerate([original_set(rom)] + ttf_sets()))
    assert len(sets) == 6, "expected 6 English font sets"
    if args.font is not None:
        sets = [sets[args.font]]
    rows = [l.rstrip("\n").split("\t", 1) for l in open(args.tsv, encoding="utf-8") if "\t" in l]
    over = check(sets, rows, args.v)
    for k, rid, what in over:
        print("FIT_OVER %d %s %s" % (k, rid, what))
    print("FIT_CHECK_OK" if not over else "FIT_CHECK_FAIL %d" % len(over))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
