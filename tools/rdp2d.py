#!/usr/bin/env python3
"""Draw the 2D part of a frame image (ELTALE_DUMP_IMAGE=<frame> writes out/frame_<frame>.bin): the
texture rectangles (text, windows, menus), with TMEM, palettes and tiles handled the way the
RDP does closely enough for checking text, and the textured triangles drawn under an orthographic
projection (the HUD's compass disc; texel colour and alpha only, no combiner). The 3D scene's
triangles are skipped.

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
    ap.add_argument("--no-hud-tris", action="store_true",
                    help="skip textured triangles under an orthographic projection (the HUD's compass disc, fades)")
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

    ident = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    geo = dict(proj=ident, mv=ident, vp=(640, 480, 640, 480), scale=(1.0, 1.0), vtx=[None] * 32)

    def mtx(a):
        w = struct.unpack(">16I", img[a:a + 64])
        m = []
        for i in range(16):
            hi = (w[i // 2] >> (16 if i % 2 == 0 else 0)) & 0xFFFF
            lo = (w[8 + i // 2] >> (16 if i % 2 == 0 else 0)) & 0xFFFF
            v = hi << 16 | lo
            m.append((v - (1 << 32) if v & 0x80000000 else v) / 65536.0)
        return [m[0:4], m[4:8], m[8:12], m[12:16]]

    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]

    def ortho():
        p = geo["proj"]
        return abs(p[3][3] - 1) < 1e-3 and all(abs(p[k][3]) < 1e-6 for k in range(3))

    def vertices(a, v0, n):
        m = mul(geo["mv"], geo["proj"])
        sx, sy, tx, ty = geo["vp"]
        for k in range(n):
            x, y, z, _, s, t = struct.unpack(">hhhHhh", img[a + k * 16:a + k * 16 + 12])
            c = [x * m[0][j] + y * m[1][j] + z * m[2][j] + m[3][j] for j in range(4)]
            w = c[3] or 1.0
            geo["vtx"][(v0 + k) & 31] = (tx / 4 + c[0] / w * sx / 4, ty / 4 - c[1] / w * sy / 4,
                                         s * geo["scale"][0] / 32, t * geo["scale"][1] / 32)

    def triangle(ia, ib, ic):
        if args.no_hud_tris or not ortho() or not geo.get("tex_on"):
            return
        p = [geo["vtx"][i & 31] for i in (ia, ib, ic)]
        if None in p:
            return
        tile = tiles[0]
        (x0, y0, s0, t0), (x1, y1, s1, t1), (x2, y2, s2, t2) = p
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-6:
            return
        for y in range(max(0, int(min(y0, y1, y2))), min(240, int(max(y0, y1, y2)) + 1)):
            for x in range(max(0, int(min(x0, x1, x2))), min(320, int(max(x0, x1, x2)) + 1)):
                cx, cy = x + 0.5, y + 0.5
                b1 = ((cx - x0) * (y2 - y0) - (x2 - x0) * (cy - y0)) / area
                b2 = ((x1 - x0) * (cy - y0) - (cx - x0) * (y1 - y0)) / area
                b0 = 1 - b1 - b2
                if b0 < 0 or b1 < 0 or b2 < 0:
                    continue
                s = int(b0 * s0 + b1 * s1 + b2 * s2) - tile["sl"]
                t = int(b0 * t0 + b1 * t1 + b2 * t2) - tile["tl"]
                if tile.get("masks"):
                    s &= (1 << tile["masks"]) - 1
                if tile.get("maskt"):
                    t &= (1 << tile["maskt"]) - 1
                c = texel(tile, max(s, 0), max(t, 0))
                if c[3] == 0:
                    continue
                px[x, y] = c[:3] + (255,)

    def walk(a, depth):
        for i in range(1 << 16):
            w0, w1 = struct.unpack(">II", img[a + i * 8:a + i * 8 + 8])
            op = w0 >> 24
            if op == 0x01:  # MTX
                m = mtx(addr(w1))
                key = "proj" if w0 & 0x10000 else "mv"
                geo[key] = m if w0 & 0x20000 else mul(m, geo[key])
            elif op == 0x03 and (w0 >> 16 & 0xFF) == 0x80:  # viewport
                v = struct.unpack(">8h", img[addr(w1):addr(w1) + 16])
                geo["vp"] = (v[0], v[1], v[4], v[5])
            elif op == 0xBB:  # TEXTURE
                geo["scale"] = ((w1 >> 16) / 65536, (w1 & 0xFFFF) / 65536)
                geo["tex_on"] = w0 & 0xFF != 0
            elif op == 0x04:  # VTX
                vertices(addr(w1), (w0 >> 16 & 0xFF) // 2, w0 >> 10 & 0x3F)
            elif op == 0xBF:  # TRI1
                triangle((w1 >> 16 & 0xFF) // 2, (w1 >> 8 & 0xFF) // 2, (w1 & 0xFF) // 2)
            elif op == 0xB1:  # TRI2
                triangle((w0 >> 16 & 0xFF) // 2, (w0 >> 8 & 0xFF) // 2, (w0 & 0xFF) // 2)
                triangle((w1 >> 16 & 0xFF) // 2, (w1 >> 8 & 0xFF) // 2, (w1 & 0xFF) // 2)
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
