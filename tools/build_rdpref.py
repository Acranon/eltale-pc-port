#!/usr/bin/env python3
"""Build eltale-rdpref (the offline pixel reference) with the port's MinGW clang, CMake and Ninja.

    python tools/build_rdpref.py            -> out/rdpref/eltale-rdpref.exe

Needs the third_party/parallel-rdp submodule (git submodule update --init third_party/parallel-rdp).
The tool reads the ROM at run time; nothing from the ROM is built in.
"""

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "out/rdpref/build"


def main() -> None:
    if not (ROOT / "third_party/parallel-rdp/parallel-rdp/rdp_device.cpp").is_file():
        raise SystemExit("third_party/parallel-rdp is missing: git submodule update --init third_party/parallel-rdp")
    cc, cxx = shutil.which("clang"), shutil.which("clang++")
    if not cc or not cxx or not shutil.which("cmake") or not shutil.which("ninja"):
        raise SystemExit("needs clang, clang++, cmake and ninja on PATH (the port's MinGW clang toolchain)")
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmake", "-G", "Ninja", "-S", str(ROOT / "tools/rdpref"), "-B", str(BUILD),
                    f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD)], check=True)
    exe = "eltale-rdpref.exe" if sys.platform == "win32" else "eltale-rdpref"
    shutil.copy2(BUILD / exe, ROOT / "out/rdpref" / exe)
    print(f"eltale-rdpref OK: {ROOT / 'out/rdpref' / exe}")


if __name__ == "__main__":
    main()
