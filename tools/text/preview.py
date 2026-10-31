"""Draw English messages the way the game will, to check the look and the fit without playing.

    python tools/text/preview.py rom.z64 [id ...]   (default: a sample) -> out/text/preview/<id>.png

Each page of a message becomes one picture of the dialogue window (native 320x240 pixels, scaled 3x).
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import enfont

TLUT_FONT = 0xD3A8B0
WINDOW_TLUT, WINDOW_FILL, NAME_BG = 0x62450, 0x62650, 0x62750
RECTS = [(34, 148, 252, 68), (34, 20, 252, 68), (40, 30, 240, 52), (40, 150, 240, 52)]
SCALE = 3


def rgba5551(rom, at):
    v = int.from_bytes(rom[at:at + 2], "big")
    return ((v >> 11 & 31) * 255 // 31, (v >> 6 & 31) * 255 // 31, (v >> 1 & 31) * 255 // 31, 255 if v & 1 else 0)


def ci8(rom, tex, tlut, w, h):
    img = Image.new("RGBA", (w, h))
    img.putdata([rgba5551(rom, tlut + rom[tex + i] * 2) for i in range(w * h)])
    return img


def draw_string(img, font, rom, x0, y0, data):
    x, y, page, pal, en = x0, y0, 0, 0, False
    for b in data:
        if b == 0xFF:
            break
        if b == 0x7F:
            x += enfont.SPACE if en else 10
        elif b < 0x80:
            pix = font.pages.get(page)
            if pix is None:
                x += 10
                continue
            left, w = (0, 10)
            if en:
                for ch, (p, g) in enfont.GLYPHS.items():
                    if (p, g) == (page, b):
                        left, w = font.metrics[ch]
                        break
            for yy in range(13):
                for xx in range(w):
                    v = pix[yy][b * 10 + left + xx]
                    if v:
                        img.putpixel((x + xx, y + yy), rgba5551(rom, TLUT_FONT + pal * 32 + v * 2))
            x += w + (enfont.GAP if en else 0)
        elif b < 0xA0:
            page = b & 0x1F
        elif b < 0xC0:
            pal = b & 0x1F
        elif b < 0xE0:
            en = (b & 0x1F) == 1
        elif b == 0xE0:
            x, y = x0, y + 14


def window(rom, font, kind, name, page_bytes, background):
    wx, wy, ww, wh = RECTS[kind]
    img = background.copy()
    fill = ci8(rom, WINDOW_FILL, WINDOW_TLUT, 16, 16)
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    for ty in range(wy, wy + wh, 16):
        for tx in range(wx, wx + ww, 16):
            layer.paste(fill.crop((0, 0, min(16, wx + ww - tx), min(16, wy + wh - ty))), (tx, ty))
    a = layer.getchannel("A").point(lambda v: v * 0xCC // 255)
    layer.putalpha(a)
    img.alpha_composite(layer)
    for x in range(wx, wx + ww):
        img.putpixel((x, wy), (255, 255, 255, 255))
        img.putpixel((x, wy + wh - 1), (255, 255, 255, 255))
    for y in range(wy, wy + wh):
        img.putpixel((wx, y), (255, 255, 255, 255))
        img.putpixel((wx + ww - 1, y), (255, 255, 255, 255))
    if name:
        img.alpha_composite(ci8(rom, NAME_BG, WINDOW_TLUT, 40, 13), (wx + 27, wy + 4))
        draw_string(img, font, rom, wx + 73, wy + 4, name)
        draw_string(img, font, rom, wx + 10, wy + 20, page_bytes)
    else:
        draw_string(img, font, rom, wx + 10, wy + 12, page_bytes)
    return img


def main():
    rom = open(sys.argv[1], "rb").read()
    ids = sys.argv[2:] or ["npc.4760", "npc.2258", "npc.0A48", "item.0320"]
    font = enfont.Font(rom)
    en = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(HERE, "..", "..", "translations", "en.v2.tsv"), encoding="utf-8"))
    speakers = {}
    sp = os.path.join(HERE, "..", "..", "out", "text", "speakers.tsv")
    if os.path.exists(sp):
        speakers = {l.split("\t")[0]: l.split("\t")[1] for l in open(sp, encoding="utf-8")}
    out = os.path.join(HERE, "..", "..", "out", "text", "preview")
    os.makedirs(out, exist_ok=True)
    background = Image.new("RGBA", (320, 240), (70, 110, 80, 255))
    for rid in ids:
        name = speakers.get(rid)
        name_bytes = enfont.encode(font, [[en[name]]]) if name else None
        kind = 0
        wx, wy, ww, wh = enfont_rect = RECTS[kind]
        pages = enfont.layout(font, en[rid], ww - 20, (wh - 20) // 13)
        for n, page in enumerate(pages):
            img = window(rom, font, kind, name_bytes, enfont.encode(font, [page]), background)
            img = img.resize((320 * SCALE, 240 * SCALE), Image.NEAREST)
            img.save(os.path.join(out, "%s_%d.png" % (rid, n + 1)))
        print(rid, len(pages), "pages")


if __name__ == "__main__":
    main()
