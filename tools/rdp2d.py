#!/usr/bin/env python3
"""Draw the 2D part of a frame image (ELTALE_DUMP_IMAGE=<frame> writes out/frame_<frame>.bin): the
texture rectangles only (text, windows, menus), with TMEM, palettes and tiles handled the way the
RDP does closely enough for checking text. Triangles (the 3D scene) are skipped.

    python tools/rdp2d.py out/frame_700.bin out/frame_700.png [--scale 3]
"""
import argparse
import struct

from PIL import Image

RDRAM = 0x80000000


def rgba5551(v):
    return ((v >> 11 & 31) * 255 // 31, (v >> 6 & 31) * 255 // 31, (v >> 1 & 31) * 255 // 31, 255 if v & 1 else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("png")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--ci-only", action="store_true", help="only colour-indexed rectangles (text and windows)")
    args = ap.parse_args()
    data = open(args.image, "rb").read()
    img, root = data[:8 << 20], struct.unpack(">I", data[8 << 20:])[0]
    seg = [0] * 16
    tmem = bytearray(4096)
    tiles = [dict(fmt=0, siz=0, line=0, tmem=0, pal=0, sl=0, tl=0) for _ in range(8)]
    st = dict(timg=0, tsiz=0, twidth=0, env=(255, 255, 255, 255), half1=0, half2=0)
    out = Image.new("RGBA", (320, 240), (0, 0, 0, 255))
    px = out.load()

    def addr(w):
        if w >= RDRAM:
            return w - RDRAM
        return (seg[(w >> 24) & 0xF] + (w & 0xFFFFFF)) & 0x7FFFFF

    def texel(t, s, tt):
        base = t["tmem"] * 8 + tt * t["line"] * 8
        if t["siz"] == 0:  # 4-bit
            b = tmem[(base + s // 2) & 0xFFF]
            v = b >> 4 if s % 2 == 0 else b & 15
            if t["fmt"] == 2:  # CI
                e = 0x800 + (t["pal"] * 16 + v) * 2
                return rgba5551(tmem[e & 0xFFF] << 8 | tmem[(e + 1) & 0xFFF])
            return (v * 17, v * 17, v * 17, 255 if v else 0)
        if t["siz"] == 1:  # 8-bit
            v = tmem[(base + s) & 0xFFF]
            if t["fmt"] == 2:
                e = 0x800 + v * 2
                return rgba5551(tmem[e & 0xFFF] << 8 | tmem[(e + 1) & 0xFFF])
            return (v, v, v, 255)
        e = base + s * 2
        return rgba5551(tmem[e & 0xFFF] << 8 | tmem[(e + 1) & 0xFFF])

    def texrect(w0, w1, s0, t0, dsdx, dtdy):
        xl, yl = (w0 >> 12 & 0xFFF) / 4, (w0 & 0xFFF) / 4
        tile = tiles[w1 >> 24 & 7]
        if args.ci_only and tile["fmt"] != 2:
            return
        xh, yh = (w1 >> 12 & 0xFFF) / 4, (w1 & 0xFFF) / 4
        s0 = (s0 - (1 << 16) if s0 & 0x8000 else s0) / 32
        t0 = (t0 - (1 << 16) if t0 & 0x8000 else t0) / 32
        dsdx = (dsdx - (1 << 16) if dsdx & 0x8000 else dsdx) / 1024
        dtdy = (dtdy - (1 << 16) if dtdy & 0x8000 else dtdy) / 1024
        a_env = st["env"][3] / 255
        for y in range(int(yh), int(yl)):
            for x in range(int(xh), int(xl)):
                if not (0 <= x < 320 and 0 <= y < 240):
                    continue
                s = int(s0 + (x - xh) * dsdx) - tile["sl"]
                t = int(t0 + (y - yh) * dtdy) - tile["tl"]
                if tile.get("masks"):
                    s &= (1 << tile["masks"]) - 1
                if tile.get("maskt"):
                    t &= (1 << tile["maskt"]) - 1
                c = texel(tile, max(s, 0), max(t, 0))
                if c[3] == 0:
                    continue
                a = a_env
                old = px[x, y]
                px[x, y] = tuple(int(c[i] * a + old[i] * (1 - a)) for i in range(3)) + (255,)

    def walk(a, depth):
        for i in range(1 << 16):
            w0, w1 = struct.unpack(">II", img[a + i * 8:a + i * 8 + 8])
            op = w0 >> 24
            if op == 0x06:
                walk(addr(w1), depth + 1)
                if (w0 >> 16) & 0xFF:
                    return
            elif op == 0xB8:
                return
            elif op == 0xBC and (w0 & 0xFF) == 0x06:
                seg[((w0 >> 8) & 0xFFFF) // 4 & 0xF] = w1 & 0x7FFFFF
            elif op == 0xFD:
                st["timg"], st["tsiz"], st["twidth"] = addr(w1), w0 >> 19 & 3, (w0 & 0xFFF) + 1
            elif op == 0xF5:
                t = tiles[w1 >> 24 & 7]
                t.update(fmt=w0 >> 21 & 7, siz=w0 >> 19 & 3, line=w0 >> 9 & 0x1FF, tmem=w0 & 0x1FF, pal=w1 >> 20 & 15,
                         maskt=w1 >> 14 & 15, masks=w1 >> 4 & 15)
            elif op == 0xF2:
                t = tiles[w1 >> 24 & 7]
                t.update(sl=(w0 >> 12 & 0xFFF) >> 2, tl=(w0 & 0xFFF) >> 2)
            elif op == 0xF4:  # LOADTILE
                t = tiles[w1 >> 24 & 7]
                uls, ult, lrs, lrt = (w0 >> 12 & 0xFFF) >> 2, (w0 & 0xFFF) >> 2, (w1 >> 12 & 0xFFF) >> 2, (w1 & 0xFFF) >> 2
                bpp = [0.5, 1, 2, 4][st["tsiz"]]
                rowb = int(st["twidth"] * bpp)
                for r in range(ult, lrt + 1):
                    src = st["timg"] + r * rowb + int(uls * bpp)
                    n = int((lrs - uls + 1) * bpp)
                    dst = t["tmem"] * 8 + (r - ult) * t["line"] * 8
                    for k in range(n):
                        tmem[(dst + k) & 0xFFF] = img[src + k]
            elif op == 0xF3:  # LOADBLOCK
                t = tiles[w1 >> 24 & 7]
                n = ((w1 >> 12 & 0xFFF) + 1) * [0.5, 1, 2, 4][st["tsiz"]]
                for k in range(int(n)):
                    tmem[(t["tmem"] * 8 + k) & 0xFFF] = img[st["timg"] + k]
            elif op == 0xF0:  # LOADTLUT
                t = tiles[w1 >> 24 & 7]
                n = ((w1 >> 14) & 0x3FF) + 1
                for k in range(n * 2):
                    tmem[(t["tmem"] * 8 + k) & 0xFFF] = img[st["timg"] + k]
            elif op == 0xFB:
                st["env"] = (w1 >> 24, w1 >> 16 & 255, w1 >> 8 & 255, w1 & 255)
            elif op == 0xE4:
                h1 = struct.unpack(">II", img[a + (i + 1) * 8:a + (i + 2) * 8])[1]
                h2 = struct.unpack(">II", img[a + (i + 2) * 8:a + (i + 3) * 8])[1]
                texrect(w0, w1, h1 >> 16, h1 & 0xFFFF, h2 >> 16, h2 & 0xFFFF)

    walk(addr(root), 0)
    out.resize((320 * args.scale, 240 * args.scale), Image.NEAREST).save(args.png)


if __name__ == "__main__":
    main()
