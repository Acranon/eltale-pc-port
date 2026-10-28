# Eltale Monsters PC port

This is the native PC port of Eltale Monsters. The matching source is pinned
as the `decomp/` submodule; PC-only edits to that source live in `patches/`.
No ROM, extracted assets or script text is stored here. Supply your own
Eltale Monsters (J) ROM in big-endian `.z64` order.

## P0 boot probe

The current x86-64 Linux/WSL build boots the decomp's `Boot_Main` and
`Core_MainThread`, creates their threads and scheduler queues, runs the game's
blocking ROM DMA and font loads, initializes graphics frames, and reaches an
18-command display list made by the game's display-list builder. It checks
the ROM SHA1, four DMA destination spans and the display-list end command.

```sh
git submodule update --init
python3 tools/patches.py apply
make p0-test ROM=/path/to/baserom.jp.z64
make endian-test
make abi-test
make census                 # writes out/p0/census.json
```

Expected result: `P0_BOOT_OK threads_created=5 ... dl_words=18`.

The probe is headless. It stops after the clear/display-list prefix; audio
synthesis and the PakMenu entry are temporary P0 stand-ins, and the three
scheduler workers are registered but not run. The intro display list,
renderer, controller input, saves and full 64-bit ABI/endian conversion are
next. See [P0 status](docs/P0_STATUS.md).

To change game source, edit `work/src/` and run `python3 tools/patches.py save`.
The resulting `patches/p0.patch` is committed; `decomp/` stays clean.
