#!/usr/bin/env python3
"""Build and run the native x64 Windows P0 checkpoint with MinGW Clang.

    python tools/build_windows.py --rom C:\\path\\to\\eltalemonsters.jp.z64
    python tools/build_windows.py --build-only --rom C:\\path\\to\\eltalemonsters.jp.z64

The ROM is read in place and never copied into this repository.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out/p0-windows"
HOST_SOURCES = ["port/p0/boot_probe.c", "port/p0/host.c", "port/p0/stubs.c",
                "port/p0/unavailable.c"]


def run(args: list[str]) -> None:
    subprocess.run(args, cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, help="user-supplied Eltale Monsters (J) z64 ROM")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--full-compile", action="store_true",
                        help="also compile every game and overlay C module into a native archive")
    parser.add_argument("--audio", action="store_true",
                        help="P1: build the real game audio and libultra audio instead of stand-ins")
    parser.add_argument("--p1-os", action="store_true",
                        help="use the fiber scheduler instead of P0 synchronous threads")
    parser.add_argument("--frames", type=int, default=1,
                        help="P1 OS: stop after this many completed graphics frames")
    parser.add_argument("--input", type=Path, help="P1 OS: scripted controller input")
    parser.add_argument("--pak", type=Path, help="P1 OS: 32 KiB Controller Pak image")
    args = parser.parse_args()
    global OUT
    if args.p1_os:
        OUT = ROOT / "out/p1-windows"
    if not (ROOT / "work/src/game/core/main.c").is_file():
        raise SystemExit("run `python tools/patches.py apply` first")
    if not args.rom:
        raise SystemExit("supply --rom to build the native game with local assets")
    cc = shutil.which("clang")
    if not cc:
        raise SystemExit("MinGW-targeting clang was not found on PATH")
    target = subprocess.check_output([cc, "-dumpmachine"], text=True).strip()
    if target != "x86_64-w64-windows-gnu":
        raise SystemExit(f"expected x86_64-w64-windows-gnu clang, got {target}")

    OUT.mkdir(parents=True, exist_ok=True)
    generated = OUT / "include/PR/gbi.h"
    run([sys.executable, "tools/gen_host_gbi.py", str(generated)])
    run([sys.executable, "tools/gen_addresses.py", str(OUT / "include/port/addresses.h")])
    run([sys.executable, "tools/check_rom.py", str(args.rom)])
    run([sys.executable, "decomp/tools/extract_assets.py", "decomp/assets.yaml",
         str(args.rom), str(OUT / "assets")])
    includes = [
        f"-I{OUT / 'include'}", f"-I{OUT}", "-Iport/include", "-I.", "-Iwork/src",
        "-Idecomp/include", "-Idecomp/src",
        "-idirafter", "decomp/include/ultra64",
        "-idirafter", "decomp/include/ultra64/PR",
        "-include", "port/addresses.h",
        "-include", "game/actor/actor.h",
    ]
    flags = [
        "-std=gnu11", "-O2", "-g", "-mcmodel=large", "-ffp-contract=off",
        # the decomp reads objects through other types and relies on wrapping signed math
        "-fno-strict-aliasing", "-fwrapv",
        "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
        "-Werror=int-to-pointer-cast", "-Werror=pointer-to-int-cast",
        "-Werror=int-to-void-pointer-cast", "-Werror=void-pointer-to-int-cast", "-D_LANGUAGE_C", "-D_MIPS_SZLONG=32",
        "-DF3DEX_GBI", "-DNDEBUG", "-D_FINALROM",
        "-DBUILD_VERSION=VERSION_J", "-DNON_MATCHING",
        "-DELTALE_P0_BOOT_PROBE",
    ]
    if args.p1_os:
        flags.append("-DELTALE_P1_OS")
        if args.audio:
            flags.append("-DELTALE_P1_AUDIO")
    boot_sources = sorted((ROOT / "work/src/game").rglob("*.c"))
    boot_sources = [p for p in boot_sources if p.as_posix() != (ROOT / "work/src/game/audio/audio.c").as_posix()]
    boot_sources += sorted((ROOT / "work/src/overlays/intro").rglob("*.c"))
    if args.p1_os:
        boot_sources += sorted((ROOT / "work/src/overlays/title").rglob("*.c"))
    sources = [str(p.relative_to(ROOT)) for p in boot_sources] + HOST_SOURCES
    if args.p1_os:
        sources += ["port/os/fiber.c", "port/os/kernel.c", "port/os/controller.c", "port/os/pak.c"]
        sources += ["port/os/math_glue.c", "port/data/map_area.c", "port/data/anim.c", "port/gfx/frame_image.c", "port/data/common.c", "port/data/enemy.c",
                    "port/gfx/vi.c", "port/gfx/rt64_bridge.c", "decomp/src/libultra/io/vitbl.c"]
        sources += [str(p.relative_to(ROOT)) for p in sorted((ROOT / "decomp/src/libultra/gu").glob("*.c"))]
        if args.audio:
            sources += ["work/src/game/audio/audio.c"]
            sources += [str(p.relative_to(ROOT)) for p in sorted((ROOT / "work/src/libultra/audio").glob("*.c"))]
    objects: list[Path] = []
    for index, source in enumerate(sources):
        obj = OUT / f"source_{index}.o"
        print(f"compile {source}", flush=True)
        if source == "port/p0/unavailable.c":
            extra = ["-DELTALE_P1_OS"] if args.p1_os else []
            run([cc, "-std=gnu11", "-O2", *extra, "-c", source, "-o", str(obj)])
        elif source.replace("\\", "/").startswith(("decomp/src/libultra/gu/", "work/src/libultra/audio/")):
            run([cc, *flags, f"-I{OUT / 'include'}", "-Iport/include", "-Idecomp/include", "-Iwork/src",
                 "-Idecomp/src", "-idirafter", "decomp/include/ultra64",
                 "-idirafter", "decomp/include/ultra64/PR", "-include",
                 "port/include/PR/ultratypes.h", "-c", source, "-o", str(obj)])
        else:
            run([cc, *flags, *includes, "-c", source, "-o", str(obj)])
        objects.append(obj)

    if args.p1_os:
        address_map = OUT / "host_address_map.c"
        run([sys.executable, "tools/gen_host_address_map.py", str(address_map),
             *(str(obj) for obj in objects[:len(boot_sources)])])
        address_map_obj = OUT / "host_address_map.o"
        run([cc, "-std=gnu11", "-O2", "-c", str(address_map), "-o", str(address_map_obj)])
        objects.append(address_map_obj)

    asm = OUT / "abs.s"
    run([sys.executable, "tools/gen_abs_symbols.py", "--asm", str(asm),
         *(str(obj) for obj in objects)])
    abs_obj = OUT / "abs.o"
    run([cc, "-c", str(asm), "-o", str(abs_obj)])
    exe = OUT / ("eltale-p1.exe" if args.p1_os else "eltale-p0.exe")
    run([cc, *(str(obj) for obj in objects), str(abs_obj), "-Wl,/OPT:REF",
         # display-list words are 32-bit: keep the image below 4 GiB and below RDRAM
         "-Wl,--image-base=0x10000000", "-Wl,--disable-dynamicbase", "-Wl,--disable-high-entropy-va",
         "-lm", "-o", str(exe)])

    abi_exe = OUT / "abi-test.exe"
    endian_exe = OUT / "endian-test.exe"
    run([cc, *flags, *includes, "tests/abi_test.c", "-o", str(abi_exe)])
    run([cc, "-std=gnu11", "-O2", "-Iport/include", "tests/endian_test.c",
         "-o", str(endian_exe)])
    run([str(abi_exe)])
    run([str(endian_exe)])
    if args.p1_os:
        fiber_test = OUT / "fiber-test.exe"
        scheduler_test = OUT / "os-scheduler-test.exe"
        events_test = OUT / "os-events-test.exe"
        run([cc, "-std=gnu11", "-O2", "-Iport/include", "-I.",
             "tests/fiber_test.c", "port/os/fiber.c", "-o", str(fiber_test)])
        run([cc, *flags, *includes, "tests/os_scheduler_test.c",
             "port/os/kernel.c", "port/os/fiber.c", "-o", str(scheduler_test)])
        run([cc, *flags, *includes, "tests/os_events_test.c",
             "port/os/kernel.c", "port/os/fiber.c", "-o", str(events_test)])
        run([str(fiber_test)])
        run([str(scheduler_test)])
        run([str(events_test)])
        gu_test = OUT / "gu-test.exe"
        run([cc, *flags, *includes, "tests/gu_test.c",
             "decomp/src/libultra/gu/mtxutil.c", "decomp/src/libultra/gu/sins.c",
             "-o", str(gu_test)])
        run([str(gu_test)])
        pak_test = OUT / "pak-test.exe"
        run([cc, *flags, *includes, "tests/pak_test.c", "port/os/pak.c", "-o", str(pak_test)])
        run([str(pak_test), str(OUT / "test.mpk")])
    if args.full_compile:
        all_sources = sorted((ROOT / "work/src/game").rglob("*.c")) + \
                      sorted((ROOT / "work/src/overlays").rglob("*.c"))
        full_flags = [flag for flag in flags if flag != "-DELTALE_P0_BOOT_PROBE"]
        full_flags += ["-Wno-deprecated-non-prototype", "-Wno-pointer-sign"]
        full_objects: list[Path] = []
        for index, source in enumerate(all_sources):
            obj = OUT / f"full_{index}.o"
            run([cc, *full_flags, *includes, f"-I{OUT}", "-c", str(source), "-o", str(obj)])
            full_objects.append(obj)
        ar = shutil.which("llvm-ar")
        if not ar:
            raise SystemExit("llvm-ar was not found on PATH")
        archive = OUT / "eltale-game.a"
        run([ar, "rcs", str(archive), *(str(obj) for obj in full_objects)])
        print(f"FULL_COMPILE_OK sources={len(all_sources)} archive={archive}")
    if args.build_only:
        print(f"Windows {'P1 OS' if args.p1_os else 'P0'} build OK: {exe}")
        return
    command = [str(exe), "--rom", str(args.rom), "--headless", "--fast"]
    if args.p1_os:
        command += ["--frames", str(args.frames)]
        if args.input:
            command += ["--input", str(args.input)]
        if args.pak:
            command += ["--pak", str(args.pak)]
    run(command)


if __name__ == "__main__":
    main()
