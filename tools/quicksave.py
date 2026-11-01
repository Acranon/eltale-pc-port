#!/usr/bin/env python3
"""Read and rewrite quick saves (quicksaves/quick<N>.elq, port/p0/quicksave.c) for tests.

    python3 tools/quicksave.py info FILE
    python3 tools/quicksave.py edit IN OUT [--clock N] [--version N] [--pc-extra N] [--truncate N] [--damage]

`edit` writes a copy with fields changed and the hash made good again (but for --damage, which flips
a byte after hashing, and --truncate, which cuts the file to N bytes after hashing):
  --clock N      the time of day (env_clock)
  --version N    the format version
  --pc-extra N   N more bytes of PC block (zeros), as a later build that adds fields would write
The file holds no ROM content: the game's own save record and the PC block (see quicksave.c).
"""

import argparse
import struct
import sys

HEADER = 0x18
PC_FIELDS = 44


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse(b: bytes):
    if b[:8] != b"ELTQUICK":
        raise SystemExit("not a quick save")
    version, pc_bytes, record_bytes = struct.unpack_from("<HHI", b, 8)
    return version, pc_bytes, record_bytes


def info(path: str) -> None:
    b = open(path, "rb").read()
    version, pc_bytes, record_bytes = parse(b)
    pc = HEADER
    _, when, m, sub, x, y, z, rot, clock, seed = struct.unpack_from("<IQiiffffiI", b, pc)
    ok = struct.unpack_from("<Q", b, 16)[0] == fnv(b[HEADER:])
    print(f"version={version} pc={pc_bytes} record={record_bytes} bytes={len(b)} hash={'ok' if ok else 'BAD'} "
          f"when={when} map={m} sub={sub} x={x:.2f} y={y:.2f} z={z:.2f} rot={rot:.3f} clock={clock} seed={seed:#x}")


def edit(a) -> None:
    b = bytearray(open(a.input, "rb").read())
    version, pc_bytes, record_bytes = parse(b)
    if a.clock is not None:
        struct.pack_into("<i", b, HEADER + 36, a.clock)
    if a.pc_extra:
        at = HEADER + pc_bytes
        b[at:at] = bytes(a.pc_extra)
        struct.pack_into("<H", b, 10, pc_bytes + a.pc_extra)
    if a.version is not None:
        struct.pack_into("<H", b, 8, a.version)
    struct.pack_into("<Q", b, 16, fnv(bytes(b[HEADER:])))
    if a.damage:
        b[HEADER + PC_FIELDS + 8] ^= 0x01
    if a.truncate is not None:
        del b[a.truncate:]
    open(a.output, "wb").write(b)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    i = sub.add_parser("info")
    i.add_argument("file")
    e = sub.add_parser("edit")
    e.add_argument("input")
    e.add_argument("output")
    e.add_argument("--clock", type=lambda s: int(s, 0))
    e.add_argument("--version", type=int)
    e.add_argument("--pc-extra", type=int, default=0)
    e.add_argument("--truncate", type=int)
    e.add_argument("--damage", action="store_true")
    a = p.parse_args()
    if a.command == "info":
        info(a.file)
    else:
        edit(a)


if __name__ == "__main__":
    sys.exit(main())
