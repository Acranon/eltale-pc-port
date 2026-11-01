# Eltale Monsters PC port

This is the native PC port of Eltale Monsters. The matching source is pinned
as the `decomp/` submodule; PC-only edits to that source live in `patches/`.
No ROM, extracted assets or script text is stored here. Supply your own
Eltale Monsters (J) ROM in big-endian `.z64` order.

## Where it is

The game plays in a window on Windows, in English or Japanese, with sound, keyboard, mouse and
controller, an F1 menu and F2/F4 quick saves. It is drawn by our own renderer (Vulkan, through
Granite) from the game's own data, at any resolution, widescreen, with MSAA and up to 240 pictures a
second; three presets (Classic, Classic + visuals, Everything on) set the switches. Linux builds and
gives the same headless results; its window is not yet tried. See the [OS status](docs/P1_OS_STATUS.md)
and the [renderer plan](docs/renderer/PLAN.md).

## Building and playing (Windows)

Use a MinGW-targeting Clang (`clang -dumpmachine` should print `x86_64-w64-windows-gnu`), CMake and
Ninja, Python 3 with PyYAML, Pillow, NumPy and fontTools (for the English text and pictures), and Git.

```powershell
git submodule update --init decomp third_party/granite
git -C third_party/granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
python tools/patches.py apply
python tools/build_windows.py --p1-os --build-only --rom "C:\path\to\eltalemonsters.jp.z64"
python tools/build_host.py
tools\play.cmd "C:\path\to\eltalemonsters.jp.z64"
```

`build_windows.py` builds `out\p1-windows\eltale-p1.exe`; `build_host.py` builds `out\host`
(`eltale_host.dll`: the window, input, sound, menu and renderer, and `SDL3.dll`). `play.cmd` copies the
DLLs beside the exe and runs it with `--window`. Settings, the Controller Pak and quick saves live in
`%APPDATA%\eltale`. Build steps, settings and the window checks: [the host layer](docs/renderer/HOST_LAYER.md).

## Linux

Built in WSL (Ubuntu 24.04); the packages it needs are listed at the top of `tools/build_linux.sh`.

```sh
git submodule update --init decomp third_party/granite
git -C third_party/granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
python3 tools/patches.py apply
bash tools/build_linux.sh --rom /path/to/eltalemonsters.jp.z64    # or: make linux ROM=...
out/p1-linux/eltale-p1 --window --rom /path/to/eltalemonsters.jp.z64
```

This builds `out/p1-linux/eltale-p1` with `libeltale_host.so` (and `libSDL3.so.0`, when SDL3 is built
from source) beside it. The data folder is `$XDG_DATA_HOME/eltale` (or `~/.local/share/eltale`).

## Tests

- `tests/p1_checks.sh exe rom [--tour]` runs the game headless and deterministic (`--headless --fast
  --frames N --input script`): New Game, the ending and its camera tour, and with `--tour` every map,
  saving and loading, the Controller Pak manager and quick saves. Each line (`P1_OS_OK`, `P1_AUDIO`)
  must stay as [the OS status](docs/P1_OS_STATUS.md) records it.
- `tests/host_checks.sh exe rom` runs it in a hidden window: New Game, the menu and reset, quick saves,
  Alt+Enter, resets and full buffers.
- The renderer's pixel reference (paraLLEl-RDP, `python tools/build_rdpref.py`, needs the
  `third_party/parallel-rdp` submodule) and the compare tools: [docs/renderer/REFERENCE.md](docs/renderer/REFERENCE.md).
- The P0 boot probe (`python tools/build_windows.py --full-compile --rom ...`, or `make p0-test
  ROM=...` on Linux) still checks the first display list: `P0_BOOT_OK intro_updates=1 ... dl_words=18
  dl_hash=0cf266fd815bb6a0`. See [P0 status](docs/P0_STATUS.md) and the [boot audit](docs/P0_BOOT_AUDIT.md).

## Controls

Keyboard and mouse by default: WASD move, Space A, Left Shift B, Q Z, Enter Start, E L, R R, arrow keys the
C buttons, IJKL the D-pad, Left Ctrl walk; hold the right mouse button (or press Tab) and move the mouse to
turn the camera, the wheel zooms, the middle button resets it. A controller works as on the N64 (right
stick the C buttons). F1 opens the menu, where every binding can be changed; F2/F4 quick save and load; F11 or Alt+Enter
fullscreen.
See [Controls](docs/CONTROLS.md).

To change game source, edit `work/src/` and run `python3 tools/patches.py save`.
The resulting `patches/p0.patch` is committed; `decomp/` stays clean.
