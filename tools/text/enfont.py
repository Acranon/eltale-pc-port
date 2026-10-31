"""English text for the game's own font: glyph choice, proportional metrics, extra glyphs, layout.

The Japanese font (ROM 0xD3A930, 30 pages of 304x13 4bpp, glyph n of a page at x = 10n) already has
digits, A-Z, a-z and some punctuation. English uses those glyphs, spaced by their real width, plus a
small page of extra glyphs drawn here in the same style. The port does the same in C
(port/text/en_text.c, en_glyphs.h generated from EXTRAS by `python tools/text/enfont.py --header`).

Pixel values: 3 stroke, 2 edge, 1 shadow (one pixel down and right of the stroke), 0 clear.
"""
import os
import sys

FONT, PAGE_BYTES, ROW_BYTES = 0xD3A930, 0x7B8, 152
EXTRA_PAGE = 30  # page code 0x9E: the port's own page (not in the ROM)
SPACE = int(os.environ.get("EN_SPACE", 4))  # pixels for a space
GAP = int(os.environ.get("EN_GAP", 0))  # extra pixels between glyphs (the shadow column already separates them)

# char -> (page, glyph) in the game's font
GLYPHS = {}
for i, ch in enumerate("0123456789!?"):
    GLYPHS[ch] = (0, i)
GLYPHS.update({".": (0, 16), ",": (0, 17), ":": (0, 20), "~": (0, 24)})
for i in range(26):
    GLYPHS[chr(65 + i)] = (1, i)
    GLYPHS[chr(97 + i)] = (2, i)
GLYPHS.update({"&": (1, 26), "'": (2, 26), "“": (2, 28), "”": (2, 27)})

# Extra glyphs, 10x13: "3" stroke, "2" edge, "." clear. The shadow is added automatically.
EXTRAS = {
    "-": [
        "", "", "", "", "", "",
        ".3333",
    ],
    "*": [
        "", "",
        "...3",
        ".2.3.2",
        "..333",
        ".2.3.2",
        "...3",
    ],
    ";": [
        "", "", "",
        "....33",
        "....33",
        "", "",
        "....33",
        "....33",
        "....32",
        "...32",
    ],
    "%": [
        "", "",
        ".33...3",
        ".33..3",
        ".....3",
        "....3",
        "....3",
        "...3",
        "...3..33",
        "..3...33",
    ],
    "<": [
        "", "", "",
        ".....3",
        "...33",
        ".33",
        "...33",
        ".....3",
    ],
    ">": [
        "", "", "",
        ".3",
        "..33",
        "....33",
        "..33",
        ".3",
    ],
    "(": [
        "",
        "....3",
        "...3",
        "..3",
        "..3",
        "..3",
        "..3",
        "..3",
        "..3",
        "...3",
        "....3",
    ],
    ")": [
        "",
        ".3",
        "..3",
        "...3",
        "...3",
        "...3",
        "...3",
        "...3",
        "...3",
        "..3",
        ".3",
    ],
    "/": [
        "",
        "......3",
        "......3",
        ".....3",
        ".....3",
        "....3",
        "....3",
        "...3",
        "...3",
        "..3",
        "..3",
    ],
    # a capital I with serifs: the font's own I is a bare bar, too like l and |
    "I": [
        "",
        "..33333",
        "....3",
        "....3",
        "....3",
        "....3",
        "....3",
        "....3",
        "....3",
        "....3",
        "..33333",
    ],
}
EXTRA_ORDER = "-*;%<>()/I"
for i, ch in enumerate(EXTRA_ORDER):
    GLYPHS[ch] = (EXTRA_PAGE, i)


def extra_page():
    """The extra page as rows of pixel values (304x13)."""
    px = [[0] * 304 for _ in range(13)]
    for i, ch in enumerate(EXTRA_ORDER):
        for y, row in enumerate(EXTRAS[ch]):
            for x, c in enumerate(row):
                if c in "23":
                    px[y][i * 10 + x] = int(c)
        for y in range(12, 0, -1):
            for x in range(i * 10 + 9, i * 10, -1):
                if px[y][x] == 0 and px[y - 1][x - 1] >= 2:
                    px[y][x] = 1
    return px


class Font:
    def __init__(self, rom):
        self.pages = {}
        for p in (0, 1, 2):
            base = FONT + p * PAGE_BYTES
            self.pages[p] = [[(rom[base + y * ROW_BYTES + x // 2] >> (4 if x % 2 == 0 else 0)) & 15
                              for x in range(304)] for y in range(13)]
        self.pages[EXTRA_PAGE] = extra_page()
        self.metrics = {}
        for ch, (p, g) in GLYPHS.items():
            cols = [x for x in range(10) if any(self.pages[p][y][g * 10 + x] for y in range(13))]
            left, right = (cols[0], cols[-1]) if cols else (0, 2)
            self.metrics[ch] = (left, right - left + 1)

    def width(self, ch):
        if ch == " ":
            return SPACE
        return self.metrics[ch][1] + GAP if ch in self.metrics else 0


def smart_quotes(text):
    out = []
    for i, ch in enumerate(text):
        if ch == '"':
            prev = text[i - 1] if i else " "
            ch = "“" if prev in " ⏎▼(*-" or i == 0 else "”"
        out.append(ch)
    return "".join(out)


def layout(font, text, width, rows):
    """Wrap one message (⏎ line, ⏎▼⏎ page, {F2} choice) into pages of lines that fit."""
    text = smart_quotes(text).replace("{F2}", "\x02")
    pages = []
    for page in text.split("⏎▼⏎"):
        lines = []
        for para in page.split("⏎"):
            line, w = "", 0
            for word in para.split(" "):
                ww = sum(font.width(c) for c in word if c != "\x02")
                if line and w + SPACE + ww > width:
                    lines.append(line)
                    line, w = word, ww
                else:
                    line, w = (line + " " + word, w + SPACE + ww) if line else (word, ww)
            lines.append(line)
        while lines:
            pages.append(lines[:rows])
            lines = lines[rows:]
    return pages


def encode(font, pages):
    """Bytes for the game: A0 C1 (font 1 = proportional English), page selects, glyphs, E0, F0, F2, FF."""
    out = [0xA0, 0xC1]
    page = None
    for pi, lines in enumerate(pages):
        if pi:
            out.append(0xF0)
        for li, line in enumerate(lines):
            if li:
                out.append(0xE0)
            for ch in line:
                if ch == " ":
                    out.append(0x7F)
                elif ch == "\x02":
                    out.append(0xF2)
                elif ch in GLYPHS:
                    p, g = GLYPHS[ch]
                    if p != page:
                        out.append(0x80 | p)
                        page = p
                    out.append(g)
    out.append(0xFF)
    return bytes(out)


def header():
    px = extra_page()
    rows = []
    for y in range(13):
        row = bytes((px[y][x] << 4) | px[y][x + 1] for x in range(0, 304, 2))
        rows.append("    " + ", ".join("0x%02X" % b for b in row) + ",")
    return ("/* Generated by tools/text/enfont.py --header: the English extra glyphs \"%s\"\n"
            " * (page 0x9E), 304x13 4bpp like a page of the game's font. */\n"
            "static const char en_extra_order[] = \"%s\";\n"
            "static const unsigned char en_extra_page[13 * 152] = {\n%s\n};\n" % (EXTRA_ORDER, EXTRA_ORDER, "\n".join(rows)))


if __name__ == "__main__" and "--header" in sys.argv:
    sys.stdout.write(header())
