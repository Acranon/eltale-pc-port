"""Make the English pictures for the port, from the user's ROM and the English art.

    python tools/images/build_en_images.py rom.z64 out/.../en_images.inc [--png out/images/en-final]

Each picture is made at the original's size in the original's palette, as colour indices in its
format, so the port can swap it in place of the Japanese (port/text/en_images.c):
- the artist's art (art/en/<name>.png, any size, same proportions): shrunk, mapped to the palette;
- the pause-menu headings: the artist's word between the original grey bullets (from the ROM);
- pages of text (art/en/pages.json): drawn in the game's own font, coloured like the original;
- the title menu strip: a wider picture of its own, three pieces, drawn by the port's own code.
The output (`--png`) can be written as PNGs to look at.
"""
import json
import os
import sys
from collections import Counter

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(HERE, "..", "text"))
import enfont  # noqa: E402

# id: (texture ROM offset, format, width, height, row stride, shown width, palette ROM offset, native symbol)
TARGETS = {
    "menu_sprite_26": (0x63690, "CI8", 82, 13, 82, 82, 0x62A98, "menu_sprite_26_tex"),
    "menu_sprite_33": (0x64280, "CI8", 86, 14, 86, 86, 0x62A98, "menu_sprite_33_tex"),
    "menu_sprite_34": (0x64740, "CI8", 54, 12, 54, 54, 0x62A98, "menu_sprite_34_tex"),
    "menu_sprite_35": (0x649D0, "CI8", 42, 12, 42, 42, 0x62A98, "menu_sprite_35_tex"),
    "menu_sprite_41": (0x65040, "CI8", 26, 9, 26, 26, 0x62A98, "menu_sprite_41_tex"),
    "menu_sprite_42": (0x65138, "CI8", 26, 9, 26, 26, 0x62A98, "menu_sprite_42_tex"),
    "menu_sprite_43": (0x65230, "CI8", 58, 16, 58, 58, 0x62A98, "menu_sprite_43_tex"),
    "menu_sprite_45": (0x656A0, "CI8", 86, 13, 86, 86, 0x62A98, "menu_sprite_45_tex"),
    "menu_sprite_86": (0x675F0, "CI8", 26, 9, 26, 26, 0x62A98, "menu_sprite_86_tex"),
    "menu_sprite_87": (0x676E8, "CI8", 26, 9, 26, 26, 0x62A98, "menu_sprite_87_tex"),
    "pak_D_801030A8": (0xDA7618, "CI4", 160, 14, 160, 160, 0xDA75F8, "D_801030A8_ovl_DA4570"),
    "pak_D_80103528": (0xDA7A98, "CI4", 64, 12, 64, 64, 0xDA7A78, "D_80103528_ovl_DA4570"),
    "pak_D_801036C8": (0xDA7C38, "CI4", 32, 12, 32, 32, 0xDA7C18, "D_801036C8_ovl_DA4570"),
    "pak_D_80103878": (0xDA7DE8, "CI4", 32, 12, 32, 32, 0xDA7DC8, "D_80103878_ovl_DA4570"),
    "pak_D_80103958": (0xDA7EC8, "CI4", 32, 12, 32, 32, 0xDA7EA8, "D_80103958_ovl_DA4570"),
    "pak_D_80103A38": (0xDA7FA8, "CI4", 80, 12, 80, 80, 0xDA7F88, "D_80103A38_ovl_DA4570"),
    "pak_D_80103DD8": (0xDA8348, "CI4", 32, 12, 32, 32, 0xDA8328, "D_80103DD8_ovl_DA4570"),
    "pak_D_80103C38": (0xDA81A8, "CI4", 64, 12, 64, 64, 0xDA8188, "D_80103C38_ovl_DA4570"),
    "map_region_0": (0xF2FD70, "CI8", 244, 185, 244, 244, 0xF2FB70, None),
    "map_region_1": (0xF3AFD0, "CI8", 244, 185, 244, 244, 0xF3ADD0, None),
    "map_region_2": (0xF46230, "CI8", 244, 185, 244, 244, 0xF46030, None),
    "map_region_3": (0xF51490, "CI8", 244, 185, 244, 244, 0xF51290, None),
    "map_region_4": (0xF5C6F0, "CI8", 244, 185, 244, 244, 0xF5C4F0, None),
    "map_region_5": (0xF67950, "CI8", 244, 185, 244, 244, 0xF67750, None),
    "map_region_6_small": (0xF72BB0, "CI8", 65, 18, 65, 65, 0xF729B0, None),
    "map_region_7_small": (0xF73250, "CI8", 65, 19, 65, 65, 0xF73050, None),
    "map_region_8_small": (0xF73930, "CI8", 96, 17, 96, 96, 0xF73730, None),
    "map_region_9_small": (0xF74190, "CI8", 99, 18, 99, 99, 0xF73F90, None),
    "prologue_anim_8": (0xE08438, "CI4", 208, 114, 208, 194, 0xE0B290, "intro_sprite_anim_8_tex"),
    "prologue_anim_12": (0xE0B498, "CI4", 160, 99, 160, 156, 0xE0D390, "intro_sprite_anim_12_tex"),
    "prologue_anim_17": (0xE0D598, "CI4", 176, 78, 176, 172, 0xE0F070, "intro_sprite_anim_17_tex"),
    "prologue_anim_22": (0xE0F278, "CI4", 176, 65, 176, 171, 0xE108D8, "intro_sprite_anim_22_tex"),
    "prologue_anim_29": (0xE10AE0, "CI4", 160, 39, 160, 155, 0xE11718, "intro_sprite_anim_29_tex"),
    "prologue_anim_34": (0xE11920, "CI4", 160, 39, 160, 145, 0xE12558, "intro_sprite_anim_34_tex"),
    "prologue_anim_32": (0xE12760, "CI4", 128, 43, 128, 121, 0xE13228, "intro_sprite_anim_32_tex"),
    "ending_sprite_anim_1": (0xE37BD8, "CI4", 272, 310, 272, 267, 0xE42090, "ending_sprite_anim_1_tex"),
    "intro_nopak_screen": (0xE13430, "CI4", 208, 112, 208, 208, 0xE161B8, "D_80259250_ovl_intro"),
    "intro_pak_help": (0xE163C0, "CI4", 208, 42, 208, 208, 0xE174D8, "D_8025C1E0_ovl_intro"),
    "intro_pak_full_screen": (0xE176E0, "CI4", 208, 112, 208, 208, 0xE1A468, "D_8025D500_ovl_intro"),
    "intro_no_controller": (0xE1A670, "CI4", 208, 42, 208, 208, 0xE1B788, "D_80260490_ovl_intro"),
    "intro_pak_error_screen": (0xE1B990, "CI4", 208, 112, 208, 208, 0xE1E718, "D_802617B0_ovl_intro"),
}
HEADINGS = ("menu_sprite_26", "menu_sprite_33", "menu_sprite_43", "menu_sprite_45")
# the title menu strip: CI4 176x12 at ROM 0xDB0240, palette 0xDB0220
TITLE_TLUT = 0xDB0220
TITLE_W, TITLE_H = 208, 12  # the English strip (even width for 4-bit rows)


def rgba5551(v):
    return ((v >> 11 & 31) * 255 // 31, (v >> 6 & 31) * 255 // 31, (v >> 1 & 31) * 255 // 31, 255 if v & 1 else 0)


def palette(rom, at, n):
    return [rgba5551(int.from_bytes(rom[at + i * 2:at + i * 2 + 2], "big")) for i in range(n)]


def decode(rom, t):
    at, fmt, w, h, stride, shown, tlut, _ = t
    pal = palette(rom, tlut, 16 if fmt == "CI4" else 256)
    img = Image.new("RGBA", (shown, h))
    p = img.load()
    for y in range(h):
        for x in range(shown):
            if fmt == "CI4":
                b = rom[at + (y * stride + x) // 2]
                i = b >> 4 if x % 2 == 0 else b & 15
            else:
                i = rom[at + y * stride + x]
            p[x, y] = pal[i]
    return img, pal


def nearest(pal, c):
    """The palette index for an RGBA colour: a clear entry for clear, else the nearest opaque one."""
    if c[3] < 128:
        for i, q in enumerate(pal):
            if q[3] == 0:
                return i
        return None
    best, bi = None, 0
    for i, q in enumerate(pal):
        if q[3] == 0:
            continue
        d = (q[0] - c[0]) ** 2 + (q[1] - c[1]) ** 2 + (q[2] - c[2]) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi


def quantise(img, pal):
    p = img.load()
    for y in range(img.height):
        for x in range(img.width):
            i = nearest(pal, p[x, y])
            p[x, y] = pal[i] if i is not None else (0, 0, 0, 0)
    return img


def fit_art(path, size, pal):
    a = Image.open(path).convert("RGBA")
    return quantise(a.resize(size, Image.BOX), pal)


def heading(path, orig, pal):
    """The artist's word between the original's grey bullets, a tenth larger than drawn."""
    W, H = orig.size
    lb, rb = orig.crop((0, 0, 11, H)), orig.crop((W - 11, 0, W, H))
    a = Image.open(path).convert("RGBA")
    k = a.width / W * 0.9
    word = a.crop(a.getchannel("A").point(lambda v: 255 if v > 128 else 0).getbbox())
    word = quantise(word.resize((max(1, round(word.width / k)), max(1, round(word.height / k))), Image.BOX), pal)
    word = word.crop(word.getbbox())
    x0 = max(0, (W - (11 + 1 + word.width + 1 + 11)) // 2)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.alpha_composite(lb, (x0, 0))
    out.alpha_composite(word, (x0 + 12, (H - 10) // 2))  # one top for all: baselines line up
    out.alpha_composite(rb, (min(W - 11, x0 + 12 + word.width + 1), 0))
    return out


def bands(img, bg):
    p = img.load()
    rows = [y for y in range(img.height) if any(p[x, y] != bg for x in range(img.width))]
    out = []
    for y in rows:
        if out and y - out[-1][1] <= 3:
            out[-1][1] = y
        else:
            out.append([y, y])
    return out


class GameFont:
    """The game's own Latin glyphs (tools/text/enfont.py): values 3 ink, 2 edge, 1 shadow."""

    def __init__(self, rom):
        self.f = enfont.Font(rom)
        self.space, self.height = enfont.SPACE, 13

    def width(self, ch):
        return self.f.width(ch)

    def glyph(self, ch):
        if ch not in enfont.GLYPHS:
            return None, 0
        page, g = enfont.GLYPHS[ch]
        left, w = self.f.metrics[ch]
        return [row[g * 10 + left:g * 10 + left + w] for row in self.f.pages[page]], w


class PixelFont:
    """An optional pixel font (tools/text/fontlab.py): values 3 ink, 1 shadow."""

    def __init__(self, file):
        import fontlab
        self.f = fontlab.TtfFont(os.path.join(ROOT, "third_party", "fonts", "sg-fonts", file))
        self.space, self.height = self.f.space, self.f.height

    def width(self, ch):
        return self.f.width(ch)

    def glyph(self, ch):
        g = self.f.glyphs.get("." if ch == "\x01" else ch)
        return (g[0], g[1]) if g else (None, 0)


def wrap(font, text, width):
    lines = []
    for line in text.split("\n"):
        cur, w = "", 0
        for word in line.split(" "):
            ww = sum(font.width(c) for c in enfont.smart_quotes(word))
            if cur and w + font.space + ww > width:
                lines.append(cur)
                cur, w = word, ww
            else:
                cur, w = (cur + " " + word, w + font.space + ww) if cur else (word, ww)
        lines.append(cur)
    return lines


def draw_line(img, font, x, y, text, colours):
    """Pixel values 3 ink, 2 edge, 1 shadow, coloured by `colours` (a value it lacks is left clear)."""
    p = img.load()
    for ch in enfont.smart_quotes(text):
        if ch == " ":
            x += font.space
            continue
        rows, adv = font.glyph(ch)
        if rows is None:
            continue
        for yy, row in enumerate(rows):
            for xx, v in enumerate(row):
                if v and colours.get(v) and 0 <= x + xx < img.width and 0 <= y + yy < img.height:
                    p[x + xx, y + yy] = colours[v]
        x += adv


def line_width(font, text):
    return sum(font.space if c == " " else font.width(c) for c in enfont.smart_quotes(text))


def page(font, orig, pal, spec):
    W, H = orig.size
    counts = Counter(orig.get_flattened_data())
    bg = counts.most_common(1)[0][0]
    opaque = [q for q in pal if q[3]]
    pick = lambda c: min(opaque, key=lambda q: sum((q[i] - c[i]) ** 2 for i in range(3)))
    if spec["style"] == "prologue":
        colours = {3: pick((255, 255, 255)), 2: pick((170, 170, 170)), 1: pick((40, 40, 40))}
        pitch, align = font.height, "centre"
    else:
        colours = {3: pick((189, 189, 189)), 2: pick((120, 120, 120))}
        pitch, align = font.height - 1, "left"
    out = Image.new("RGBA", (W, H), bg)
    orig_bands = bands(orig, bg)
    choices = spec.get("yes_no")
    text_bands = orig_bands
    if choices and len(orig_bands) > 1 and orig_bands[-1][0] > H * 0.6:
        text_bands = orig_bands[:-1]
    paras = [wrap(font, p, W - 4) for p in spec["text"].split("\n\n")]
    if spec["style"] != "prologue":
        # the game's line spacing when it fits above the choices, else a pixel tighter
        room = (text_bands[-1][1] if choices else H) + 2
        if sum(len(l) for l in paras) * font.height <= room:
            pitch = font.height
    y = 0
    for n, lines in enumerate(paras):
        # each paragraph where the original's matching block starts (or after the one before)
        start = text_bands[n][0] - 1 if n < len(text_bands) else y + pitch
        y = max(y + (pitch if n else 0), start) if n else max(0, start)
        if y + len(lines) * pitch > H:
            y = max(0, H - len(lines) * pitch)
        for line in lines:
            x = (W - line_width(font, line)) // 2 if align == "centre" else 2
            draw_line(out, font, x, y, line, colours)
            y += pitch
    if choices and len(orig_bands) > 1 and orig_bands[-1][0] > H * 0.6:
        top, bottom = orig_bands[-1]
        p = orig.load()
        cols = [x for x in range(W) if any(p[x, yy] != bg for yy in range(top, bottom + 1))]
        groups = []
        for x in cols:
            if groups and x - groups[-1][1] <= 6:
                groups[-1][1] = x
            else:
                groups.append([x, x])
        for (a, b), word in zip(groups, choices):
            draw_line(out, font, (a + b) // 2 - line_width(font, word) // 2, top - 1, word, colours)
    overflow = y > H + 1
    return out, overflow


def inpaint(img, box, avoid):
    """Paint over `box` with the patch of the same picture whose surroundings match best, from
    somewhere free of text (`avoid`: every label's box)."""
    import numpy as np
    a = np.asarray(img.convert("RGB")).astype(np.int32)
    H, W, _ = a.shape
    x0, y0, x1, y1 = box
    w, h = x1 - x0 + 1, y1 - y0 + 1
    ring = [(x, y0 - 1) for x in range(x0 - 1, x1 + 2)] + [(x, y1 + 1) for x in range(x0 - 1, x1 + 2)] + \
           [(x0 - 1, y) for y in range(y0, y1 + 1)] + [(x1 + 1, y) for y in range(y0, y1 + 1)]
    ring = [(x, y) for x, y in ring if 0 <= x < W and 0 <= y < H]
    rx = np.array([x - x0 for x, _ in ring])
    ry = np.array([y - y0 for _, y in ring])
    target = a[[y for _, y in ring], [x for x, _ in ring]]
    lum = a[..., 0] * 3 + a[..., 1] * 6 + a[..., 2]
    texty = ((lum < 1000) | (lum > 2300)).astype(np.int32)  # the labels' dark and white
    best = None
    for dy in range(1, H - h - 1, 2):
        for dx in range(1, W - w - 1, 2):
            if any(not (dx + w + 1 < b[0] or dx - 1 > b[2] or dy + h + 1 < b[1] or dy - 1 > b[3]) for b in avoid):
                continue
            cost = int(((a[dy + ry, dx + rx] - target) ** 2).sum()) + 20000 * int(texty[dy:dy + h, dx:dx + w].sum())
            if best is None or cost < best[0]:
                best = (cost, dx, dy)
    _, dx, dy = best
    img.paste(img.crop((dx, dy, dx + w, dy + h)), (x0, y0))


def map_picture(orig, pal, labels, label_font, title_font):
    out = orig.copy()
    avoid = [tuple(l["box"]) for l in labels]
    for l in labels:
        inpaint(out, tuple(l["box"]), avoid)
    opaque = [q for q in pal if q[3]]
    pick = lambda c: min(opaque, key=lambda q: sum((q[i] - c[i]) ** 2 for i in range(3)))
    for l in labels:
        font = title_font if l.get("title") else label_font
        colours = {3: pick((247, 247, 247)), 2: pick((206, 206, 206)), 1: pick((57, 41, 24))} if l.get("title") else \
                  {3: pick((74, 57, 57)), 2: pick((115, 99, 82)), 1: pick((206, 197, 173))}
        w = line_width(font, l["text"])
        x = l["left"] if "left" in l else l["right"] - w if "right" in l else l["centre"] - w // 2
        draw_line(out, font, x, l["top"], l["text"], colours)
    return out


HUD_SHEET = (0xD7CF40, "CI4", 144, 45, 144, 144, 0xD7DBE8, None)
HUD_RAM = 0x8039ADB0 + (0xD7CF40 - 0xD74AC0)  # in the common block, loaded at boot
HUD_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'-.:!?"


def hud_sheet(rom):
    """The battle HUD's spell-name font with Latin letters in place of the kana: glyph n at
    (n % 16 * 8, n // 16 * 9), 8x9, the kana's fill and outline colours (Royalty Mono shapes)."""
    import fontlab
    orig, pal = decode(rom, HUD_SHEET)
    counts = Counter(c for c in orig.get_flattened_data() if c[3])
    opaque = sorted(counts, key=lambda c: sum(c[:3]))
    outline, fill = opaque[0], counts.most_common(1)[0][0]
    if fill == outline:
        fill = opaque[-1]
    f = fontlab.TtfFont(os.path.join(ROOT, "third_party", "fonts", "sg-fonts", "RoyaltyMono.ttf"))
    out = Image.new("RGBA", orig.size, (0, 0, 0, 0))
    p = out.load()
    for n, ch in enumerate(HUD_CHARS):
        px, adv = f.glyphs[ch]
        gx, gy = n % 16 * 8, n // 16 * 9
        ink = {(x, y) for y, row in enumerate(px) for x, v in enumerate(row) if v == 3}
        left = min(x for x, _ in ink) if ink else 0
        ink = {(x - left + 1, y) for x, y in ink}  # one column for the outline on the left
        for x, y in ink:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q not in ink and 0 <= q[0] < 8 and 0 <= q[1] < 9:
                        p[gx + q[0], gy + q[1]] = outline
        for x, y in ink:
            if 0 <= x < 8 and 0 <= y < 9:
                p[gx + x, gy + y] = fill
    return out, pal


def logo_picture(path, w, h):
    """The artist's logo at the game's size (194x76, like the Japanese one): shrunk with averaging,
    hard-edged (the N64's one-bit clear), and 255 colours of its own; returns a palette-index
    picture ("P") and the palette as RGBA5551 halfwords in host (little-endian) order."""
    a = Image.open(path).convert("RGBA")
    a = a.crop(a.getchannel("A").point(lambda v: 255 if v > 128 else 0).getbbox())
    # two pixels smaller than the slot: room for a dark outline and a drop shadow (as the Quest 64
    # logo has), so the gold reads on the title screen's gold background
    s = min((w - 3) / a.width, (h - 3) / a.height)
    small = a.resize((max(1, round(a.width * s)), max(1, round(a.height * s))), Image.LANCZOS)
    letters = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    letters.alpha_composite(small, ((w - 3 - small.width) // 2 + 1, (h - 3 - small.height) // 2 + 1))
    lm = letters.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cp, lp = canvas.load(), lm.load()
    for y in range(h):
        for x in range(w):
            if lp[x, y]:
                continue
            near = any(0 <= x + dx < w and 0 <= y + dy < h and lp[x + dx, y + dy]
                       for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            shadow = x >= 2 and y >= 2 and lp[x - 2, y - 2]
            if near:
                cp[x, y] = (48, 26, 8, 255)
            elif shadow:
                cp[x, y] = (24, 14, 6, 255)
    canvas.alpha_composite(letters)
    opaque = canvas.getchannel("A").point(lambda v: 255 if v >= 128 else 0)
    q = canvas.convert("RGB").quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:255 * 3]
    out = Image.new("P", (w, h), 0)
    op, qp, mp = out.load(), q.load(), opaque.load()
    for y in range(h):
        for x in range(w):
            if mp[x, y]:
                op[x, y] = qp[x, y] + 1
    rgba = [(0, 0, 0, 0)] + [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], 255) for i in range(len(pal) // 3)]
    rgba += [(0, 0, 0, 0)] * (256 - len(rgba))
    out.putpalette([c for col in rgba for c in col[:3]])
    tlut = bytearray()
    for r, g, b, al in rgba:
        v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (1 if al else 0)
        tlut += bytes((v & 0xFF, v >> 8))
    preview = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pp = preview.load()
    for y in range(h):
        for x in range(w):
            if op[x, y]:
                pp[x, y] = rgba[op[x, y]]
    return out, tlut, preview


def title_strip(path, pal):
    """The artist's strip at 12 px tall in the original palette; its three pieces by the gaps."""
    a = Image.open(path).convert("RGBA")
    a = a.crop(a.getchannel("A").point(lambda v: 255 if v > 128 else 0).getbbox())
    w = round(a.width * TITLE_H / a.height)
    strip = quantise(a.resize((w, TITLE_H), Image.BOX), pal)
    p = strip.load()
    cols = [x for x in range(w) if any(p[x, y][3] for y in range(TITLE_H))]
    pieces = []
    for x in cols:
        if pieces and x - pieces[-1][1] <= 5:  # a word gap inside an item, not between items
            pieces[-1][1] = x
        else:
            pieces.append([x, x])
    assert len(pieces) == 3, pieces
    out = Image.new("RGBA", (TITLE_W, TITLE_H), (0, 0, 0, 0))
    out.alpha_composite(strip.crop((0, 0, min(w, TITLE_W), TITLE_H)), (0, 0))
    return out, [(a, b - a + 1) for a, b in pieces]


def pack(img, pal, fmt, stride, h, bg_index):
    data = bytearray()
    p = img.load()
    for y in range(h):
        row = []
        for x in range(stride):
            c = p[x, y] if x < img.width and y < img.height else None
            i = nearest(pal, c) if c is not None else bg_index
            row.append(bg_index if i is None else i)
        if fmt == "CI4":
            data += bytes((row[x] << 4) | row[x + 1] for x in range(0, stride, 2))
        else:
            data += bytes(row)
    return data


def c_array(name, data):
    lines = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for k in range(0, len(data), 24):
        lines.append("    " + ", ".join("%d" % b for b in data[k:k + 24]) + ",")
    lines.append("};")
    return lines


def main():
    rom = open(sys.argv[1], "rb").read()
    out_path = sys.argv[2]
    png_dir = sys.argv[sys.argv.index("--png") + 1] if "--png" in sys.argv else None
    art = os.path.join(ROOT, "art", "en")
    pages = json.load(open(os.path.join(art, "pages.json"), encoding="utf-8"))
    maps = json.load(open(os.path.join(art, "maps.json"), encoding="utf-8"))
    game = GameFont(rom)
    # the prologue's pages are small: in the game's font if every one fits, else all in a smaller one
    small = PixelFont("QuestSquare.ttf")
    prologue_font = game
    for name, t in TARGETS.items():
        if pages.get(name, {}).get("style") == "prologue" and name.startswith("prologue"):
            if page(game, decode(rom, t)[0], decode(rom, t)[1], pages[name])[1]:
                prologue_font = small
                break
    print("prologue pages in", "the game's font" if prologue_font is game else "Quest Square")
    lines = ["/* Generated by tools/images/build_en_images.py from the ROM and art/en: the English pictures. */"]
    table = []
    for name, t in TARGETS.items():
        at, fmt, w, h, stride, shown, tlut, sym = t
        orig, pal = decode(rom, t)
        if name in maps:
            img = map_picture(orig, pal, maps[name], small, PixelFont("RetroSans.ttf"))
        elif name in pages:
            font = prologue_font if name.startswith("prologue") else game
            img, overflow = page(font, orig, pal, pages[name])
            if overflow:
                print("%s: the text runs past the bottom" % name)
        elif name in HEADINGS:
            img = heading(os.path.join(art, name + ".png"), orig, pal)
        else:
            img = fit_art(os.path.join(art, name + ".png"), (shown, h), pal)
        bg = Counter(orig.get_flattened_data()).most_common(1)[0][0]
        data = pack(img, pal, fmt, stride, h, pal.index(bg))
        if png_dir:
            os.makedirs(png_dir, exist_ok=True)
            img.save(os.path.join(png_dir, name + ".png"))
        lines += c_array("en_img_" + name, data)
        table.append((name, sym, at, len(data)))
    hud, hud_pal = hud_sheet(rom)
    if png_dir:
        hud.save(os.path.join(png_dir, "hud_glyph_font.png"))
    hud_data = pack(hud, hud_pal, "CI4", 144, 45, nearest(hud_pal, (0, 0, 0, 0)) or 0)
    lines += c_array("en_img_hud_glyph_font", hud_data)
    table.append(("hud_glyph_font", None, HUD_SHEET[0], len(hud_data)))
    codes = [255] * 128
    for n, ch in enumerate(HUD_CHARS):
        codes[ord(ch)] = n
    lines.append("/* ASCII -> the HUD font's glyph code (255: none) */")
    lines.append("static const unsigned char en_hud_codes[128] = { %s };" % ", ".join(map(str, codes)))
    # the logo (title screen and the prologue's copy): its own 255 colours, so its palette is
    # replaced too (a native u16 array: host-order halfwords); entry 0 is clear
    logo_path = os.path.join(art, "title_center_image.png")
    if os.path.exists(logo_path):
        logo, logo_tlut, logo_preview = logo_picture(logo_path, 194, 76)
        if png_dir:
            logo_preview.save(os.path.join(png_dir, "title_center_image.png"))
        logo_data = bytearray()
        lp = logo.load()
        for y in range(76):
            for x in range(194):
                logo_data.append(lp[x, y])
        lines += c_array("en_img_logo", logo_data)
        lines += c_array("en_img_logo_tlut", logo_tlut)
        for tex, tlut, rom_tex, rom_tlut in (("title_center_image_tex", "title_center_image_tlut", 0xDB0C70, 0xDB0A70),
                                             ("D_80218800_ovl_intro", "D_80218600_ovl_intro", 0xDD29E0, 0xDD27E0)):
            lines.append("#define en_img_%s en_img_logo" % tex)
            lines.append("#define en_img_%s en_img_logo_tlut" % tlut)
            table.append((tex, tex, rom_tex, len(logo_data)))
            table.append((tlut, tlut, rom_tlut, len(logo_tlut)))
    title_pal = palette(rom, TITLE_TLUT, 16)
    strip, pieces = title_strip(os.path.join(art, "title_menu_atlas.png"), title_pal)
    if png_dir:
        strip.save(os.path.join(png_dir, "title_menu_strip.png"))
    lines += c_array("en_title_strip", pack(strip, title_pal, "CI4", TITLE_W, TITLE_H, 0))
    lines.append("enum { EN_TITLE_W = %d, EN_TITLE_H = %d };" % (TITLE_W, TITLE_H))
    lines.append("static const int en_title_pieces[3][2] = { %s };" % ", ".join("{ %d, %d }" % p for p in pieces))
    for name, sym, at, n in table:
        if sym:
            lines.append("extern unsigned char %s[];" % sym)
    lines.append("static EnImage en_images[%d] = {" % len(table))
    for name, sym, at, n in table:
        ram = "0x%08X" % HUD_RAM if name == "hud_glyph_font" else "0"
        lines.append("    { \"%s\", %s, 0x%06X, %s, %d, en_img_%s, 0 }," % (name, sym or "0", at, ram, n, name))
    lines.append("};")
    with open(out_path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    print("%d pictures + the title strip -> %s" % (len(table), out_path))


if __name__ == "__main__":
    main()
