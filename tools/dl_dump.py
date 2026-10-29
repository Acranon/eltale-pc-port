#!/usr/bin/env python3
"""Walk a frame image (ELTALE_DUMP_IMAGE=<frame> writes out/frame_<frame>.bin) as the RSP would:
print the display-list tree with matrices decoded and vertex ranges summarised.

    python tools/dl_dump.py out/frame_700.bin [--max 400]
"""
import argparse
import struct

RDRAM = 0x80000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--max", type=int, default=400, help="commands to print")
    args = ap.parse_args()
    data = open(args.image, "rb").read()
    img, root = data[:8 << 20], struct.unpack(">I", data[8 << 20:])[0]
    seg = [0] * 16
    printed = [0]

    def addr(w):
        if w >= RDRAM:
            return w - RDRAM
        return (seg[(w >> 24) & 0xF] + (w & 0xFFFFFF)) & 0x7FFFFF

    def mtx(a):
        w = struct.unpack(">16I", img[a:a + 64])
        out = []
        for i in range(16):
            hi = (w[i // 2] >> (16 if i % 2 == 0 else 0)) & 0xFFFF
            lo = (w[8 + i // 2] >> (16 if i % 2 == 0 else 0)) & 0xFFFF
            v = (hi << 16 | lo)
            if v & 0x80000000:
                v -= 1 << 32
            out.append(v / 65536.0)
        return [out[0:4], out[4:8], out[8:12], out[12:16]]

    def vtx(a, n):
        pts = [struct.unpack(">hhh", img[a + i * 16:a + i * 16 + 6]) for i in range(n)]
        return min(pts), max(pts)

    def walk(a, depth):
        for i in range(1 << 15):
            w0, w1 = struct.unpack(">II", img[a + i * 8:a + i * 8 + 8])
            op = w0 >> 24
            note = ""
            if op == 0x01:
                m = mtx(addr(w1))
                kind = ("proj" if w0 & 0x10000 else "mv") + (" load" if w0 & 0x20000 else " mul") + (" push" if w0 & 0x40000 else "")
                note = f"MTX {kind} @{w1:08x} t=({m[3][0]:.1f},{m[3][1]:.1f},{m[3][2]:.1f}) row0=({m[0][0]:.3f},{m[0][1]:.3f},{m[0][2]:.3f})"
            elif op == 0x04:
                n = (w0 >> 10) & 0x3F
                lo, hi = vtx(addr(w1), n)
                note = f"VTX n={n} @{w1:08x} min={lo} max={hi}"
            elif op == 0x06:
                note = f"DL @{w1:08x}" + (" (branch)" if (w0 >> 16) & 0xFF else "")
            elif op == 0xBC and (w0 & 0xFF) == 0x06:
                seg[((w0 >> 8) & 0xFFFF) // 4 & 0xF] = w1 & 0x7FFFFF
                note = f"SEGMENT {((w0 >> 8) & 0xFFFF) // 4} = {w1:08x}"
            elif op == 0x03:
                note = f"MOVEMEM idx={(w0 >> 16) & 0xFF:02x} @{w1:08x}"
                if (w0 >> 16) & 0xFF == 0x80:
                    note += " viewport=" + str(struct.unpack(">8h", img[addr(w1):addr(w1) + 16]))
            elif op in (0xBF, 0xB1):
                note = "TRI"
            elif op == 0xB8:
                note = "END"
            if printed[0] < args.max and (op in (0x01, 0x03, 0x04, 0x06, 0xBC, 0xB8) or depth == 0):
                print("  " * depth + f"{w0:08x}:{w1:08x} {note}")
                printed[0] += 1
            if op == 0x06:
                walk(addr(w1), depth + 1)
                if (w0 >> 16) & 0xFF:
                    return
            if op == 0xB8:
                return

    walk(addr(root), 0)


if __name__ == "__main__":
    main()
