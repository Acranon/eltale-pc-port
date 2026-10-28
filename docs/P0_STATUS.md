# P0 native build status

## First intro display list reached

On native x64 Windows with MinGW Clang, `python tools/build_windows.py --rom ...`
uses the user-supplied ROM and runs the same checkpoint as the Linux/WSL GCC
`make p0-test ROM=...` path. Both use the retail ROM (SHA1
`50ea6c3d4de4b68b3c9b77524d8177066b9f36e7`) and run
the decomp's boot, scheduler setup, controller initialization, DMA, font load,
startup initialization, intro overlay load, `Intro_Init`, the first
`Intro_Update`, and the display-list builder. The observed checkpoint is five
threads created/started, two run synchronously, 56 DMA chunks (816,336
bytes), and 18 graphics commands ending in `G_ENDDL`. Six DMA spans are
compared byte for byte against the input ROM. Windows and Linux produce the
same command hash, `0cf266fd815bb6a0`.

The port-only patch takes the checkpoint immediately after the first intro
display list. On that first update, the host's successful Pak scan causes
`Intro_Update` to return before sprite drawing, so the list contains the
game's clear/setup and full-sync/end commands. It is not a visible intro
picture or a playable game frame. [The boot audit](P0_BOOT_AUDIT.md) records
the exact path and data read before the checkpoint.

## Host design already in place

- `decomp/` is a pinned submodule; `tools/patches.py` applies/saves PC-only
  game changes into `patches/p0.patch` without modifying it.
- Native Windows Clang now compiles all 47 game and overlay C modules, including
  `NON_MATCHING` drafts, into `out/p0-windows/eltale-game.a` with ROM assets
  generated into the ignored build directory. The boot executable links the
  game and intro modules it needs; the archive is a full-source compile gate.
- The host uses fixed-width N64 scalar types. `tools/gen_host_gbi.py` generates
  an ignored SDK header with 32-bit `Gfx` and `Mtx` words (`sizeof` is checked
  at build time). The Windows build and `make abi-test` on Linux also check the
  display-list packet offset.
  Host runtime structs may grow with 64-bit pointers.
- A mapped N64 address range receives raw ROM DMA bytes. Fixed linker labels
  are generated from unresolved `D_` names by `tools/gen_abs_symbols.py`
  (linker flags on Linux, an absolute COFF symbol object on Windows).
  The enlarged graphics frames use host storage and a trace-only physical
  address; a real RDRAM mapping is still needed for rendering.
- `port/include/port/endian.h` has big-endian scalar read/write helpers and a small test.
  DMA copies raw bytes; no multi-byte ROM record is interpreted before this
  checkpoint. `tools/port_census.py` lists candidate endian and pointer-width
  sites for the full port. Its output is a starting inventory, not an ABI
  safety certificate.
- The OS stand-in runs boot thread IDs 1 and 6 synchronously, records the
  three scheduler workers, and services blocking PI DMA through message
  queues. Windows reserves RDRAM with `VirtualAlloc`, Linux with `mmap`.
  P1 will replace the stand-in with priority-scheduled fibers and events.

## Boundary after the first-list milestone

The first-list milestone is reproducible on native Windows and Linux. The
later code paths still have pointer-width warnings and ROM/save records that
need byte-order conversion before they can run correctly; the boot executable
does not certify them. The build keeps later audio, Pak, math and graphics
functions as fail-fast stand-ins. The next phase replaces those, runs the
scheduler workers, and reaches visible intro frames with the reference
renderer. The wider ABI and endian census, CPU edge-case checks and emulator
comparison are tracked as prerequisites for those paths.

The original byte-matching decomp build is unaffected by the port patch.
