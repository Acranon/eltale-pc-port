"""Fit the artist's English images (out/images/sol-en/, any size, transparent) to the game:
shrink to the original's size, map to the original's colours, and for the pause-menu headings put
the original grey bullets back around the word.

    python tools/images/fit_art.py   (from the repo root) -> out/images/sol-native/, sol_native_sheet.png
"""
import os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "images"))
import glob
from PIL import Image, ImageDraw
def quant(img, pal):
    p = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = p[x, y]
            if a < 128: p[x, y] = (0, 0, 0, 0); continue
            c = min(pal, key=lambda c: (c[0]-r)**2 + (c[1]-g)**2 + (c[2]-b)**2)
            p[x, y] = c + (255,)
    return img
for n in ('menu_sprite_26', 'menu_sprite_33', 'menu_sprite_43', 'menu_sprite_45'):
    o = Image.open('png/%s.png' % n).convert('RGBA')
    W, H = o.size
    pal = sorted(set(c[:3] for c in o.get_flattened_data() if c[3] > 128))
    lb = o.crop((0, 0, 11, H))
    rb = o.crop((W - 11, 0, W, H))
    a = Image.open('sol-en/%s.png' % n).convert('RGBA')
    k = a.width / W
    word = a.crop(a.getchannel('A').point(lambda v: 255 if v > 128 else 0).getbbox())
    k = k * 0.9  # a tenth bigger than drawn, to stand as tall as the Japanese between the bullets
    word = quant(word.resize((max(1, round(word.width / k)), max(1, round(word.height / k))), Image.BOX), pal)
    word = word.crop(word.getbbox())
    total = 11 + 1 + word.width + 1 + 11
    if total > W:
        print(n, 'word too wide', word.width, 'for', W)
    x0 = max(0, (W - total) // 2)
    out = Image.new('RGBA', (W, H))
    out.alpha_composite(lb, (x0, 0))
    # the same top for every word, so their baselines line up (descenders hang below)
    out.alpha_composite(word, (x0 + 12, (H - 10) // 2))
    out.alpha_composite(rb, (min(W - 11, x0 + 12 + word.width + 1), 0))
    out.save('sol-native/%s.png' % n)
    print(n, 'word', word.size, 'canvas', (W, H), 'sol', a.size)

HEADINGS = ('menu_sprite_26', 'menu_sprite_33', 'menu_sprite_43', 'menu_sprite_45')
def fit(a, o):
    W, H = o.size
    small = a.resize((W, H), Image.BOX)
    pal = sorted(set(c[:3] for c in o.get_flattened_data() if c[3] > 128))
    sp = small.load()
    for y in range(H):
        for x in range(W):
            r, g, b, al = sp[x, y]
            if al < 128: sp[x, y] = (0, 0, 0, 0); continue
            c = min(pal, key=lambda c: (c[0]-r)**2 + (c[1]-g)**2 + (c[2]-b)**2)
            sp[x, y] = c + (255,)
    return small
S = 4
rows = []
for f in sorted(glob.glob('sol-en/*.png')):
    n = os.path.basename(f)[:-4]
    o = Image.open('png/%s.png' % n).convert('RGBA')
    if n in HEADINGS:
        s = Image.open('sol-native/%s.png' % n).convert('RGBA')
    else:
        s = fit(Image.open(f).convert('RGBA'), o)
        s.save('sol-native/%s.png' % n)
    rows.append((n, o, s))
y = 0
sheet = Image.new('RGBA', (1400, 4000), (40, 40, 60, 255))
d = ImageDraw.Draw(sheet)
for n, o, s in rows:
    d.text((4, y + 2), n, fill=(255, 255, 255))
    sheet.alpha_composite(o.resize((o.width * S, o.height * S), Image.NEAREST), (150, y))
    sheet.alpha_composite(s.resize((s.width * S, s.height * S), Image.NEAREST), (170 + o.width * S, y))
    y += o.height * S + 8
sheet.crop((0, 0, 1400, y)).save('sol_native_sheet.png')
