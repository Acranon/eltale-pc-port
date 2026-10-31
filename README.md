# Eltale Monsters PC port

This is the native PC port of Eltale Monsters. The matching source is pinned
as the `decomp/` submodule; PC-only edits to that source live in `patches/`.
No ROM, extracted assets or script text is stored here. Supply your own
Eltale Monsters (J) ROM in big-endian `.z64` order.

## P0 first-display-list milestone

The native x64 Windows and x86-64 Linux/WSL builds boot the decomp's
`Boot_Main` and `Core_MainThread`, create their threads and scheduler queues, run the game's
blocking ROM DMA and font loads, initialize graphics frames, and reach an
18-command display list made by the game's display-list builder after the
first intro update. They check the ROM SHA1, six DMA destination spans and
the display-list end command and hash.

Windows is the primary development path. Use a MinGW-targeting Clang
(`clang -dumpmachine` should print `x86_64-w64-windows-gnu`), Python 3 with
PyYAML (and, for the English pictures, Pillow, NumPy and fontTools), and Git:

```powershell
git submodule update --init
python tools/patches.py apply
python tools/build_windows.py --full-compile --rom "C:\path\to\eltalemonsters.jp.z64"
```

This compiles every game and overlay C module into a native archive, then
builds and runs `out\p0-windows\eltale-p0.exe`. Use `--build-only` to compile
without launching it, or omit `--full-compile` for a faster boot-only build.

Linux/WSL remains available for comparison:

```sh
git submodule update --init
python3 tools/patches.py apply
make p0-test ROM=/path/to/baserom.jp.z64
make endian-test
make abi-test
make census                 # writes out/p0/census.json
```

Expected on either platform: `P0_BOOT_OK intro_updates=1 ... dl_words=18
dl_hash=0cf266fd815bb6a0`.

The probe is headless. It runs the real startup initialization and first
`Intro_Update`, then stops after the clear/display-list commands. Audio
synthesis, the PakMenu entry and the Pak scan are temporary P0 stand-ins;
the three scheduler workers are registered but not run. Visible intro
frames, rendering, input, saves and wider ABI/endian conversion follow.
See [P0 status](docs/P0_STATUS.md) and the [boot audit](docs/P0_BOOT_AUDIT.md).

## P1 OS development

The fiber-scheduled OS can run deterministic intro frames on native Windows:

```powershell
python tools/build_windows.py --p1-os --frames 60 --rom "C:\path\to\eltalemonsters.jp.z64"
```

The run logs each completed display-list hash and ends with `P1_OS_OK`.
Linux/WSL uses `make P1_OS=1 OUT=out/p1 p1-os-run ROM=/path/to/rom.z64
FRAMES=60`. The [OS status](docs/P1_OS_STATUS.md) records the completed
scheduler/event slices and the remaining audio, controller and Pak work.

To change game source, edit `work/src/` and run `python3 tools/patches.py save`.
The resulting `patches/p0.patch` is committed; `decomp/` stays clean.
