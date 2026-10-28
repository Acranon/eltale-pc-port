# Eltale Monsters PC port

This is the native PC port of Eltale Monsters. The matching source is pinned
as the `decomp/` submodule; PC-only edits to that source live in `patches/`.
No ROM, extracted assets or script text is stored here. Supply your own
Eltale Monsters (J) ROM in big-endian `.z64` order.

## P0 boot probe

The native x64 Windows and x86-64 Linux/WSL builds boot the decomp's `Boot_Main` and
`Core_MainThread`, create their threads and scheduler queues, run the game's
blocking ROM DMA and font loads, initialize graphics frames, and reach an
18-command display list made by the game's display-list builder. They check
the ROM SHA1, four DMA destination spans and the display-list end command.

Windows is the primary development path. Use a MinGW-targeting Clang
(`clang -dumpmachine` should print `x86_64-w64-windows-gnu`), Python 3 and Git:

```powershell
git submodule update --init
python tools/patches.py apply
python tools/build_windows.py --rom "C:\path\to\eltalemonsters.jp.z64"
```

This builds and runs `out\p0-windows\eltale-p0.exe` as a native Windows
program. For a build without a ROM, pass `--build-only`.

To compile every game and overlay C module into a native archive as well:

```powershell
python tools/build_windows.py --rom "C:\path\to\eltalemonsters.jp.z64" --full-compile
```

The archive is a compile gate. The boot executable still stops at the P0
checkpoint while the remaining native ABI and first intro frame are ported.

Linux/WSL remains available for comparison:

```sh
git submodule update --init
python3 tools/patches.py apply
make p0-test ROM=/path/to/baserom.jp.z64
make endian-test
make abi-test
make census                 # writes out/p0/census.json
```

Expected on either platform: `P0_BOOT_OK threads_created=5 ... dl_words=18`.

The probe is headless. It stops after the clear/display-list prefix; audio
synthesis and the PakMenu entry are temporary P0 stand-ins, and the three
scheduler workers are registered but not run. The intro display list,
renderer, controller input, saves and full 64-bit ABI/endian conversion are
next. See [P0 status](docs/P0_STATUS.md).

To change game source, edit `work/src/` and run `python3 tools/patches.py save`.
The resulting `patches/p0.patch` is committed; `decomp/` stays clean.
