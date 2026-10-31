"""Try pixel fonts (TTF) as the English font, drawn in the game's style, in the game's text window.

    python tools/text/fontlab.py rom.z64 font.ttf [...] -> out/text/fontlab/<name>.png, summary.png

Each font is rasterised at its own pixel grid (no smoothing), given the game's drop shadow (one
pixel down and right) and palette, word-wrapped into the dialogue window with as many lines as fit,
and laid out for the whole script to count the pages it needs.
"""
import math
import os
import sys
from functools import reduce

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import enfont
import preview

TEXT_W, TEXT_H = 232, 48  # the dialogue window's text area (252x68 window, name row above)
SAMPLE = ["npc.4760", "npc.2258", "npc.0A48"]
CHARS = [chr(c) for c in range(33, 127)]


def pixel_size(path):
    """The font's own pixel size: the smallest size where smoothed drawing has no in-between greys
    (every edge lands on a pixel boundary)."""
    best = None
    for size in range(5, 40):
        font = ImageFont.truetype(path, size)
        img = Image.new("L", (400, 60), 0)
        ImageDraw.Draw(img).text((2, 2), "AaBbgjkMWQ?.,", font=font, fill=255)
        hist = img.histogram()
        ink = sum(hist[1:])
        grey = sum(hist[1:250])
        if ink and grey / ink < 0.02:
            return size
        if ink and (best is None or grey / ink < best[1]):
            best = (size, grey / ink)
    return best[0]


class TtfFont:
    """Glyphs as rows of game pixel values (3 ink, 1 shadow), with advances."""

    def __init__(self, path):
        self.size = pixel_size(path)
        font = ImageFont.truetype(path, self.size)
        ascent, descent = font.getmetrics()
        self.height = ascent + descent + 1
        self.glyphs = {}
        for ch in CHARS + ["“", "”"]:
            try:
                mask = font.getmask(ch, mode="1")
            except Exception:
                continue
            bbox = font.getbbox(ch)
            adv = round(font.getlength(ch))
            img = Image.new("1", (adv + 4, self.height + 2), 0)
            d = ImageDraw.Draw(img)
            d.fontmode = "1"
            d.text((0, 0), ch, font=font, fill=1)
            px = [[img.getpixel((x, y)) and 3 or 0 for x in range(img.width)] for y in range(img.height)]
            for y in range(img.height - 1, 0, -1):
                for x in range(img.width - 1, 0, -1):
                    if px[y][x] == 0 and px[y - 1][x - 1] == 3:
                        px[y][x] = 1
            if not any(any(r) for r in px) and ch not in (" ",):
                continue
            self.glyphs[ch] = (px, adv)
        # crop every glyph to the rows any glyph inks (plus the shadow row); a line is that plus 1
        rows = [y for px, _ in self.glyphs.values() for y, r in enumerate(px) if any(v == 3 for v in r)]
        top, bottom = min(rows), max(rows) + 1
        for ch, (px, adv) in self.glyphs.items():
            self.glyphs[ch] = (px[top:bottom + 1], adv)
        self.height = bottom - top + 1
        self.space = round(font.getlength(" ")) or 3
        self.line = self.height + 1
        self.rows = max(1, (TEXT_H + 2) // self.line)

    def width(self, ch):
        if ch == " ":
            return self.space
        g = self.glyphs.get(ch)
        return g[1] if g else 0


def layout(font, text):
    text = enfont.smart_quotes(text).replace("\x01", ".").replace("{F2}", "")
    pages = []
    for page in text.split("⏎▼⏎"):
        lines = []
        for para in page.split("⏎"):
            line, w = "", 0
            for word in para.split(" "):
                ww = sum(font.width(c) for c in word)
                if line and w + font.space + ww > TEXT_W:
                    lines.append(line)
                    line, w = word, ww
                else:
                    line, w = (line + " " + word, w + font.space + ww) if line else (word, ww)
            lines.append(line)
        while lines:
            pages.append(lines[:font.rows])
            lines = lines[font.rows:]
    return pages


def draw_page(rom, gfont, font, name, lines, background):
    img = preview.window(rom, gfont, 0, enfont.encode(gfont, [[name]]), bytes([0xFF]), background)
    colors = [preview.rgba5551(rom, preview.TLUT_FONT + v * 2) for v in range(4)]
    wx, wy = preview.RECTS[0][:2]
    y = wy + 20
    for line in lines:
        x = wx + 10
        for ch in line:
            if ch == " ":
                x += font.space
                continue
            g = font.glyphs.get(ch)
            if not g:
                continue
            px, adv = g
            for yy, row in enumerate(px):
                for xx, v in enumerate(row):
                    if v and 0 <= x + xx < 320 and 0 <= y + yy < 240:
                        img.putpixel((x + xx, y + yy), colors[v])
            x += adv
        y += font.line
    return img


def main():
    rom = open(sys.argv[1], "rb").read()
    gfont = enfont.Font(rom)
    en = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(HERE, "..", "..", "translations", "en.v2.tsv"), encoding="utf-8"))
    out = os.path.join(HERE, "..", "..", "out", "text", "fontlab")
    os.makedirs(out, exist_ok=True)
    bg = Image.new("RGBA", (320, 240), (70, 110, 80, 255))
    crops = []
    rows = []
    # the current font, for comparison
    total = sum(len(enfont.layout(gfont, t, TEXT_W, 3)) for t in en.values())
    page = enfont.layout(gfont, en[SAMPLE[0]], TEXT_W, 3)[2]
    img = preview.window(rom, gfont, 0, enfont.encode(gfont, [["Mais"]]), enfont.encode(gfont, [page]), bg)
    crops.append(("Current (the game's own)", img.crop((30, 144, 290, 220)), total, 3))
    for path in sys.argv[2:]:
        name = os.path.splitext(os.path.basename(path))[0]
        try:
            font = TtfFont(path)
        except Exception as e:
            print(name, "failed:", e)
            continue
        missing = [c for c in CHARS if c not in font.glyphs]
        total = sum(len(layout(font, t)) for t in en.values())
        pages = layout(font, en[SAMPLE[0]])
        img = draw_page(rom, gfont, font, "Mais", pages[min(2, len(pages) - 1)], bg)
        img.resize((960, 720), Image.NEAREST).save(os.path.join(out, name + ".png"))
        crops.append(("%s (%dpx, %d lines)" % (name, font.size, font.rows), img.crop((30, 144, 290, 220)), total, font.rows))
        rows.append((name, font.size, font.height, font.rows, total, "".join(missing)))
        print("%-14s size %2d line %2d rows %d pages %4d missing %s" % rows[-1])
    scale = 3
    sheet = Image.new("RGB", (260 * scale * 2 + 30, (len(crops) + 1) // 2 * (76 * scale + 40)), (255, 255, 255))
    d = ImageDraw.Draw(sheet)
    for i, (label, crop, total, nrows) in enumerate(crops):
        x = (i % 2) * (260 * scale + 30)
        y = (i // 2) * (76 * scale + 40)
        d.text((x + 4, y + 4), "%s: %d pages for the whole script" % (label, total), fill=(0, 0, 0))
        sheet.paste(crop.resize((260 * scale, 76 * scale), Image.NEAREST), (x, y + 20))
    sheet.save(os.path.join(out, "summary.png"))


if __name__ == "__main__":
    main()
