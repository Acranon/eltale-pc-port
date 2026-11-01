#!/usr/bin/env bash
# Build the Linux game: out/p1-linux/eltale-p1 (top-level Makefile, gcc) and the window/renderer
# library libeltale_host.so (port/host, CMake + Ninja, in out/host-linux), copied beside the exe
# with libSDL3.so.0 when SDL3 was built from source (no system SDL3).
#
#     tools/build_linux.sh --rom /path/to/eltalemonsters.jp.z64 [--no-host] [--clean]
#     (or: make linux ROM=/path/to/eltalemonsters.jp.z64)
#
# Needs: gcc, make, cmake, ninja, python3 with Pillow, PyYAML and fontTools, and for the host the
# Vulkan/X11/Wayland/ALSA/PulseAudio/udev/dbus/xkbcommon development headers SDL3 builds against.
# Run `python3 tools/patches.py apply` first. The ROM is read in place and never copied.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="out/p1-linux"
HOST_OUT="out/host-linux"
ROM="${ELTALE_ROM:-}"
HOST=1
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM="$2"; shift 2 ;;
        --no-host) HOST=0; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
cd "$ROOT"
[ -n "$ROM" ] || { echo "supply --rom (or set ELTALE_ROM)" >&2; exit 2; }
[ -f work/src/game/core/main.c ] || { echo "run \`python3 tools/patches.py apply\` first" >&2; exit 2; }
if [ "$CLEAN" = 1 ]; then rm -rf "$OUT" "$HOST_OUT"; fi

make -j"$(nproc)" P1_OS=1 OUT="$OUT" ROM="$ROM" p0

if [ "$HOST" = 1 ]; then
    configure=(cmake -S port/host -B "$HOST_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release)
    if [ -n "${ELTALE_SDL3_DIR:-}" ]; then configure+=("-DELTALE_SDL3_DIR=$ELTALE_SDL3_DIR"); fi
    "${configure[@]}"
    cmake --build "$HOST_OUT"
    cp -f "$HOST_OUT/libeltale_host.so" "$OUT/"
    # the SDL3 built here (libSDL3.so -> libSDL3.so.0 -> libSDL3.so.0.x.y), links kept
    for lib in "$HOST_OUT"/libSDL3.so*; do
        if [ -e "$lib" ]; then cp -fP "$lib" "$OUT/"; fi
    done
fi
if [ "$HOST" = 1 ]; then echo "linux build OK: $OUT/eltale-p1 + libeltale_host.so"; else echo "linux build OK: $OUT/eltale-p1"; fi
