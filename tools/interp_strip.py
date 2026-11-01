#!/usr/bin/env python3
"""A strip of pictures from a --dump-frames folder with pictures between ticks (R6), for looking at motion: the
ticks from A to B and the pictures between them, in time order, as a PNG grid (a row per tick) or an animated GIF
(each picture shown for its time at --hz).

    python tools/interp_strip.py FOLDER A B OUT.png|OUT.gif [--scale S] [--hz 120] [--crop x0,y0,x1,y1]
"""
import argparse
import os
import re

from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder")
    ap.add_argument("first", type=int)
    ap.add_argument("last", type=int)
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--hz", type=int, default=120, help="a GIF's pictures a second (the pictures between's rate)")
    ap.add_argument("--crop", help="x0,y0,x1,y1 of each picture")
    ap.add_argument("--ticks-only", action="store_true", help="only the ticks' own pictures (the 30 fps look)")
    args = ap.parse_args()
    pics = []
    for n in os.listdir(args.folder):
        m = re.match(r"frame_(\d+)(?:_(\d+))?\.png$", n)
        if m and args.first <= int(m.group(1)) <= args.last:
            if args.ticks_only and m.group(2):
                continue
            pics.append((int(m.group(1)) + (int(m.group(2)) / 1000 if m.group(2) else 0), n))
    pics.sort()
    ims = []
    for _, n in pics:
        im = Image.open(os.path.join(args.folder, n)).convert("RGB")
        if args.crop:
            im = im.crop(tuple(int(v) for v in args.crop.split(",")))
        if args.scale != 1:
            im = im.resize((im.width * args.scale, im.height * args.scale), Image.NEAREST)
        ims.append((n, im))
    if args.out.lower().endswith(".gif"):
        ms = round(1000 / args.hz)
        if args.ticks_only:
            ms = round(1000 / 30)
        ims[0][1].save(args.out, save_all=True, append_images=[i for _, i in ims[1:]], duration=ms, loop=0)
    else:
        per = max(1, sum(1 for t, _ in pics if int(t) == int(pics[0][0])))
        w, h = ims[0][1].size
        rows = (len(ims) + per - 1) // per
        sheet = Image.new("RGB", (w * per, h * rows))
        for i, (_, im) in enumerate(ims):
            sheet.paste(im, ((i % per) * w, (i // per) * h))
        sheet.save(args.out)
    print("%s: %d pictures" % (args.out, len(ims)))


if __name__ == "__main__":
    main()
