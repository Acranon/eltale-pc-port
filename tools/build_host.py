#!/usr/bin/env python3
"""Build out/host/eltale_host.dll (port/host, port/render) with the MinGW clang the game uses.

    python tools/build_host.py [--clean]

CMake and Ninja configure port/host/CMakeLists.txt into out/host; the first configure downloads the
pinned SDL3 MinGW package (hash checked) unless ELTALE_SDL3_DIR names an unpacked one. Copy
out/host/*.dll next to the game (tools/play.cmd does) to play with it.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out/host"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clean", action="store_true", help="configure from scratch")
    args = parser.parse_args()
    for tool in ("cmake", "ninja", "clang", "clang++"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} was not found on PATH")
    target = subprocess.check_output(["clang", "-dumpmachine"], text=True).strip()
    if sys.platform == "win32" and target != "x86_64-w64-windows-gnu":
        raise SystemExit(f"expected x86_64-w64-windows-gnu clang, got {target}")
    if args.clean and OUT.exists():
        shutil.rmtree(OUT)
    configure = ["cmake", "-S", str(ROOT / "port/host"), "-B", str(OUT), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
    if os.environ.get("ELTALE_SDL3_DIR"):
        configure.append(f"-DELTALE_SDL3_DIR={os.environ['ELTALE_SDL3_DIR']}")
    subprocess.run(configure, cwd=ROOT, check=True)
    subprocess.run(["cmake", "--build", str(OUT)], cwd=ROOT, check=True)
    print(f"host build OK: {OUT / 'eltale_host.dll'}")


if __name__ == "__main__":
    main()
