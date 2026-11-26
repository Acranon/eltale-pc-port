#!/usr/bin/env bash
# Build the macOS (Apple Silicon, native arm64) game: out/p1-macos/eltale-p1 (top-level Makefile, clang, its
# image linked at 0x8_10000000: docs/MACOS.md) and the window/renderer library libeltale_host.dylib (port/host,
# CMake + Ninja, in out/host-macos), copied beside the exe with libSDL3.0.dylib and libMoltenVK.dylib, so the
# folder runs as it is.
#
#     tools/build_macos.sh --rom /path/to/eltalemonsters.jp.z64 [--no-host] [--clean]
#
# Needs: the Xcode command line tools, and from Homebrew: cmake ninja sdl3 molten-vk (or ELTALE_SDL3_DIR, an
# SDL3 install prefix, and ELTALE_MOLTENVK, a libMoltenVK.dylib), and a Python 3.10+ with Pillow, PyYAML,
# NumPy and fontTools (PYTHON, default python3; a venv does: python3 -m venv out/venv &&
# out/venv/bin/pip install pillow pyyaml numpy fonttools, then PYTHON=out/venv/bin/python).
# Run `$PYTHON tools/patches.py apply` first. The ROM is read in place and never copied.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="out/p1-macos"
HOST_OUT="out/host-macos"
ROM="${ELTALE_ROM:-}"
PYTHON="${PYTHON:-python3}"
HOST=1
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM="$2"; shift 2 ;;
        --no-host) HOST=0; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
cd "$ROOT"
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || { echo "this builds the arm64 macOS game: run it on an Apple Silicon Mac" >&2; exit 2; }
[ -n "$ROM" ] || { echo "supply --rom (or set ELTALE_ROM)" >&2; exit 2; }
[ -f work/src/game/core/main.c ] || { echo "run \`$PYTHON tools/patches.py apply\` first" >&2; exit 2; }
"$PYTHON" -c 'import sys; assert sys.version_info >= (3, 10); import yaml, PIL, numpy, fontTools' 2>/dev/null || {
    echo "$PYTHON needs Python 3.10+ with Pillow, PyYAML, NumPy and fontTools (see the top of $0)" >&2; exit 2; }
if [ "$CLEAN" = 1 ]; then rm -rf "$OUT" "$HOST_OUT"; fi

make -j"$(sysctl -n hw.ncpu)" P1_OS=1 OUT="$OUT" ROM="$ROM" CC=clang PYTHON="$PYTHON" p0

if [ "$HOST" = 1 ]; then
    configure=(cmake -S port/host -B "$HOST_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release)
    if [ -n "${ELTALE_SDL3_DIR:-}" ]; then configure+=("-DELTALE_SDL3_DIR=$ELTALE_SDL3_DIR"); fi
    "${configure[@]}"
    cmake --build "$HOST_OUT"
    cp -f "$HOST_OUT/libeltale_host.dylib" "$OUT/"
    chmod u+w "$OUT/libeltale_host.dylib"

    # SDL3 beside it, named through @rpath (the library's rpath is @loader_path), so the folder can move
    sdl=$(otool -L "$OUT/libeltale_host.dylib" | awk '/libSDL3[.0-9]*dylib/ { print $1; exit }')
    case "$sdl" in
        "") echo "libeltale_host.dylib does not name SDL3" >&2; exit 1 ;;
        @rpath/*) sdl_file="$HOST_OUT/$(basename "$sdl")" ;; # (SDL3 built here, beside the library)
        *) sdl_file="$sdl" ;;
    esac
    sdl_name=$(basename "$sdl")
    cp -f "$sdl_file" "$OUT/$sdl_name"
    chmod u+w "$OUT/$sdl_name"
    install_name_tool -id "@rpath/$sdl_name" "$OUT/$sdl_name"
    if [ "$sdl" != "@rpath/$sdl_name" ]; then
        install_name_tool -change "$sdl" "@rpath/$sdl_name" "$OUT/libeltale_host.dylib"
    fi

    # MoltenVK (Vulkan on Metal) beside it: the host picks it before Homebrew's or the SDK's (port/host/host.cpp)
    mvk="${ELTALE_MOLTENVK:-}"
    if [ -z "$mvk" ]; then
        for p in "$(brew --prefix molten-vk 2>/dev/null)/lib/libMoltenVK.dylib" /usr/local/lib/libMoltenVK.dylib; do
            if [ -f "$p" ]; then mvk="$p"; break; fi
        done
    fi
    [ -n "$mvk" ] && [ -f "$mvk" ] || { echo "no libMoltenVK.dylib: brew install molten-vk, or set ELTALE_MOLTENVK" >&2; exit 1; }
    cp -f "$mvk" "$OUT/libMoltenVK.dylib"
    chmod u+w "$OUT/libMoltenVK.dylib"

    # install_name_tool leaves the signatures stale, and arm64 macOS runs nothing unsigned: sign them again (ad hoc)
    codesign -f -s - "$OUT/$sdl_name" "$OUT/libeltale_host.dylib" "$OUT/libMoltenVK.dylib" 2>/dev/null
fi
if [ "$HOST" = 1 ]; then echo "macOS build OK: $OUT/eltale-p1 + libeltale_host.dylib"; else echo "macOS build OK: $OUT/eltale-p1"; fi
