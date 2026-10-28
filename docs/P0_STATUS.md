# P0 native build status

## Boot checkpoint reached

On native x64 Windows with MinGW Clang, `python tools/build_windows.py --rom ...`
uses the user-supplied ROM and runs the same checkpoint as the Linux/WSL GCC
`make p0-test ROM=...` path. Both use the retail ROM (SHA1
`50ea6c3d4de4b68b3c9b77524d8177066b9f36e7`) and run
the decomp's boot, scheduler setup, controller initialization, DMA, font load,
graphics frame initialization and first display-list builder. The observed
checkpoint is five threads created/started, two run synchronously, 24 DMA
chunks (353,296 bytes), and 18 graphics commands ending in `G_ENDDL`.
Four DMA spans are compared byte for byte against the input ROM.

This is an early P0 probe. A port-only patch takes the checkpoint after
`Gfx_InitFrames` and skips the mode-specific draws in
`Gfx_BuildFrameDisplayList`. The display list is the game's hardware clear
prefix followed by its normal full-sync/end commands. It is not an intro
picture or a playable game frame.

## Host design already in place

- `decomp/` is a pinned submodule; `tools/patches.py` applies/saves PC-only
  game changes into `patches/p0.patch` without modifying it.
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
- `port/endian.h` has big-endian scalar read/write helpers and a small test.
  DMA copies raw bytes; no multi-byte ROM record is interpreted before this
  checkpoint. `tools/port_census.py` lists candidate endian and pointer-width
  sites for the full port. Its output is a starting inventory, not an ABI
  safety certificate.
- The OS stand-in runs boot thread IDs 1 and 6 synchronously, records the
  three scheduler workers, and services blocking PI DMA through message
  queues. Windows reserves RDRAM with `VirtualAlloc`, Linux with `mmap`.
  P1 will replace the stand-in with priority-scheduled fibers and events.

## Next work to complete P0

1. Compile and port the remaining game C, including the 22 `NON_MATCHING`
   drafts, without temporary audio or PakMenu stand-ins.
2. Audit every pointer-to-integer conversion and every ROM/save struct on a
   64-bit host. Add strict compiler gates after the known conversions are
   fixed. The P0 build currently suppresses these warnings for the copied
   decomp code; it is not ABI clean yet.
3. Review `out/p0/census.json` and convert multi-byte ROM/save fields at
   their read sites. Validate against a reference emulator trace.
4. Run the real intro overlay and scheduler to its full first display list,
   then add the P1 renderer and input/save implementations.

The original byte-matching decomp build is unaffected by the port patch.
