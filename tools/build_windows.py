#!/usr/bin/env python3
"""Build and run the native x64 Windows P0 checkpoint with MinGW Clang.

    python tools/build_windows.py --rom C:\\path\\to\\eltalemonsters.jp.z64
    python tools/build_windows.py --build-only

The ROM is read in place and never copied into this repository.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out/p0-windows"
SOURCES = [
    "work/src/game/core/boot.c",
    "work/src/game/core/dma.c",
    "work/src/game/core/main.c",
    "work/src/game/core/sched.c",
    "work/src/game/input/input.c",
    "work/src/game/text/text.c",
    "port/p0/boot_probe.c",
    "port/p0/host.c",
    "port/p0/stubs.c",
]


def run(args: list[str]) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, help="user-supplied Eltale Monsters (J) z64 ROM")
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    if not (ROOT / "work/src/game/core/main.c").is_file():
        raise SystemExit("run `python tools/patches.py apply` first")
    cc = shutil.which("clang")
    if not cc:
        raise SystemExit("MinGW-targeting clang was not found on PATH")
    target = subprocess.check_output([cc, "-dumpmachine"], text=True).strip()
    if target != "x86_64-w64-windows-gnu":
        raise SystemExit(f"expected x86_64-w64-windows-gnu clang, got {target}")

    OUT.mkdir(parents=True, exist_ok=True)
    generated = OUT / "include/PR/gbi.h"
    run([sys.executable, "tools/gen_host_gbi.py", str(generated)])
    includes = [
        f"-I{OUT / 'include'}", "-Iport/include", "-I.", "-Iwork/src",
        "-Idecomp/include", "-Idecomp/src",
        "-idirafter", "decomp/include/ultra64",
        "-idirafter", "decomp/include/ultra64/PR",
        "-include", "game/actor/actor.h",
    ]
    flags = [
        "-std=gnu11", "-O2", "-g", "-mcmodel=large",
        "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
        "-Wno-int-to-pointer-cast",
        "-Wno-pointer-to-int-cast", "-D_LANGUAGE_C", "-D_MIPS_SZLONG=32",
        "-DF3DEX_GBI", "-DNDEBUG", "-D_FINALROM",
        "-DBUILD_VERSION=VERSION_J", "-DNON_MATCHING",
        "-DELTALE_P0_BOOT_PROBE",
    ]
    objects: list[Path] = []
    for index, source in enumerate(SOURCES):
        obj = OUT / f"source_{index}.o"
        print(f"compile {source}", flush=True)
        run([cc, *flags, *includes, "-c", source, "-o", str(obj)])
        objects.append(obj)

    asm = OUT / "abs.s"
    run([sys.executable, "tools/gen_abs_symbols.py", "--asm", str(asm),
         *(str(obj) for obj in objects)])
    abs_obj = OUT / "abs.o"
    run([cc, "-c", str(asm), "-o", str(abs_obj)])
    exe = OUT / "eltale-p0.exe"
    run([cc, *(str(obj) for obj in objects), str(abs_obj), "-Wl,/OPT:REF", "-o", str(exe)])

    abi_exe = OUT / "abi-test.exe"
    endian_exe = OUT / "endian-test.exe"
    run([cc, *flags, *includes, "tests/abi_test.c", "-o", str(abi_exe)])
    run([cc, "-std=gnu11", "-O2", "-Iport/include", "tests/endian_test.c",
         "-o", str(endian_exe)])
    run([str(abi_exe)])
    run([str(endian_exe)])
    if args.build_only:
        print(f"Windows P0 build OK: {exe}")
        return
    if not args.rom:
        raise SystemExit("supply --rom path or use --build-only")
    run([sys.executable, "tools/check_rom.py", str(args.rom)])
    run([str(exe), "--rom", str(args.rom), "--headless", "--fast"])


if __name__ == "__main__":
    main()
