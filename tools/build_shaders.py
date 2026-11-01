#!/usr/bin/env python3
"""Compile the native renderer's shaders (port/render/shaders/) to SPIR-V into port/render/nd_shaders.h,
which is committed: building the port needs no shader compiler, only changing a shader does (as King's
Field IV does).

    python tools/build_shaders.py [--glslang PATH]

glslang is Khronos' glslangValidator: from the Vulkan SDK, a distribution's package, or built from
source (here: C:/code/kf4-tools/glslang-build/StandAlone/glslangValidator.exe). Without --glslang it is
looked for as $GLSLANG, then that path, then on the PATH.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(ROOT, "port", "render", "shaders")
OUT = os.path.join(ROOT, "port", "render", "nd_shaders.h")
KNOWN = r"C:/code/kf4-tools/glslang-build/StandAlone/glslangValidator.exe"

# Shaders built a second time with defines, under a name of their own: (source, name, defines)
VARIANTS = [("nd.frag", "nd_ms.frag", ["-DND_MSAA"])]


def find_glslang(given):
    for candidate in (given, os.environ.get("GLSLANG"), KNOWN, "glslangValidator", "glslang"):
        if candidate and (os.path.isfile(candidate) or shutil.which(candidate)):
            return candidate
    sys.exit("no glslang: pass --glslang PATH or set GLSLANG")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslang")
    args = parser.parse_args()
    glslang = find_glslang(args.glslang)

    sources = sorted(f for f in os.listdir(SHADERS) if f.endswith((".vert", ".frag", ".comp")))
    lines = [
        "/* The native renderer's shaders as SPIR-V, made by tools/build_shaders.py from port/render/shaders/.",
        "   Generated: edit the shaders and run the script again. */",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
    ]
    builds = [(name, name, []) for name in sources] + [v for v in VARIANTS if v[0] in sources]
    with tempfile.TemporaryDirectory() as tmp:
        for source, name, defines in builds:
            out = os.path.join(tmp, name + ".spv")
            cmd = [glslang, "-V", "--target-env", "vulkan1.1", "-I" + SHADERS] + defines + ["-o", out,
                   os.path.join(SHADERS, source)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                sys.exit("%s failed:\n%s%s" % (name, result.stdout, result.stderr))
            data = open(out, "rb").read()
            words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
            ident = "nd_spv_" + name.replace(".", "_")
            lines.append("static const uint32_t %s[] = {" % ident)
            for i in range(0, len(words), 8):
                lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
            lines.append("};")
            lines.append("")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d shaders)" % (OUT, len(builds)))


if __name__ == "__main__":
    main()
