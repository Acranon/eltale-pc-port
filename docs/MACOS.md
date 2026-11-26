# macOS (Apple Silicon, native arm64)

Brief (2026-11-25): run the port natively on arm64 macOS, without Rosetta, and leave the Windows and
Linux builds exactly as they are. Rosetta was ruled out: it is being wound down after macOS 27.

## Where it is (2026-11-26)

- **The address model** (below): the game's 32-bit addresses live in a 4 GiB window higher up.
- **The game builds and links natively** (`tools/build_macos.sh`, `make macos`): the AArch64 fiber
  switch, the two-pass link that puts the image at `0x8_10000000`, the raw labels as absolute Mach-O
  symbols, `libeltale_host.dylib` with SDL3 and MoltenVK beside it, the data folder.
- **It plays.** On an M1 (macOS 27.2) with the ROM, `tests/p1_checks.sh --tour` prints the same 25 lines
  as an x86-64 build of the same code (the Windows/Linux address model) does: New Game
  (`dma_chunks=416 last_hash=74b8baf63f9c1889`, audio `be0b67b7b718648e`, as HOST_LAYER.md records for
  Windows and Linux), the ending, the camera tour, the 30,374-frame tour of every map with its audio hash,
  save and load, the Pak manager and quick saves. The game runs in a window (Retina swapchain, FIFO).
  The OS, ABI, endian and `addr-test` tests pass, and the window and renderer open on MoltenVK
  (`tests/host_smoke.c`). `tests/host_checks.sh` (a hidden window) passes: New Game drawn natively
  (`last_hash=6472f32d00cadc74`, as HOST_LAYER.md records, and sound played), the F1 menu's Reset into the
  Pak manager with its screens read back from the GPU matching the CPU frames, quick save and load with the
  notice, Alt+Enter, reset and full buffers. Reset was Windows-only; macOS now snapshots the image's
  writable segments (`__DATA`) and RDRAM as Windows does its sections (Linux still has none). Not yet
  tried: `tests/audio_checks.sh`, a long play.
- Windows and Linux build the same machine code as before throughout.

What running it found (each fixed): `en_images.c` turned a 32-bit address into a pointer without
`PORT_PTR` (the AST check could not compile it without the ROM's data; it does now); `PORT_PTR(0)` must stay
NULL, since the game tests converted pointers against NULL; the native 2D draw read the HUD's
`&D_2000000.ortho` (a segment-2 label, in the window) as a whole host pointer, as `PORT_ADDR_NARROW` only
took in the window from 0x10000000 (a limit left from the first window, which malloc shared): it takes the
whole window now, and so does `osVirtualToPhysical` (in play only: headless runs draw nothing natively);
`Port_HostNormalizeFrameAddress` compared a 32-bit
word with a host pointer (the frame hashes differed); and ld64's chained fixups do not survive the segments
being moved (below).

The ROM: a `.n64` dump is byte-swapped (`37 80 40 12`); swap each pair of bytes to `.z64` order first.
## Building and playing (macOS)

On an Apple Silicon Mac, with the Xcode command line tools and Homebrew:

```sh
brew install cmake ninja python sdl3 molten-vk
python3 -m venv out/venv && out/venv/bin/pip install pillow pyyaml numpy fonttools
git submodule update --init decomp third_party/granite
git -C third_party/granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
out/venv/bin/python tools/patches.py apply
PYTHON=out/venv/bin/python tools/build_macos.sh --rom /path/to/eltalemonsters.jp.z64
out/p1-macos/eltale-p1 --window --rom /path/to/eltalemonsters.jp.z64
```

This builds `out/p1-macos/eltale-p1` with `libeltale_host.dylib`, `libSDL3.0.dylib` (named through
`@rpath`) and `libMoltenVK.dylib` beside it, signed ad hoc, so the folder runs as it is. Settings, the
Controller Pak and quick saves live in `~/Library/Application Support/eltale` (`ELTALE_DATA_DIR` moves
it). The OS tests: `make p1-os-test abi-test endian-test addr-test P1_OS=1 OUT=out/p1-macos CC=clang
PYTHON=out/venv/bin/python`.

### What macOS needs besides the address model

- **Fibers** (`port/os/fiber.c`): an AArch64 switch, saving AAPCS64's callee-saved registers (x19..x28,
  the frame pointer and link register, d8..d15) and FPCR; x18 is Darwin's and is left alone. A new fiber
  starts with FPCR's defaults (round to nearest, no flush to zero). The stacks' fixed region
  (`0x1001_00000000`) is taken there as on Linux.
- **The link** (`Makefile`, `tools/macho_segaddr.py`): ld64 ignores `-image_base` for a
  position-independent executable, so the game is linked once, and again with each segment moved by the
  same amount so `__TEXT` starts at `0x8_10000000` (the first link is kept as `eltale-p1.pass1`). The
  raw `D_` labels come from `gen_abs_symbols.py --macho`; `-mcmodel=large`, `-fno-pie` and the section
  flags are dropped; `-dead_strip` stands for `--gc-sections`. `-no_fixup_chains`: with the segments moved,
  ld64's chained fixups still counted the game's own pointers from the usual base (`0x1_00000000`), each
  `0x7_10000000` too high (a static table's pointer to the image's own data faulted); the older rebase
  opcodes count from the segments as they are.
- **`tools/gen_host_address_map.py`** reads Mach-O names (their leading underscore off) and looks a
  32-bit word up with the window put back.
- **The host library** (`port/host`): `libeltale_host.dylib`, rpath `@loader_path`. SDL3 from Homebrew
  (its `SDL3::SDL3` is an alias, now resolved). Granite's pinned timer uses `clock_nanosleep`, which
  macOS lacks: `cmake/granite_macos.cmake` makes it sleep with `nanosleep` on the same clock (Apple
  only, at configure, as the other Granite patches are made).
- **Vulkan** is MoltenVK. SDL (the surface) and Granite (the device) must load the same library, from
  where neither looks by itself: `port/host/host.cpp` picks `libvulkan.1.dylib` or `libMoltenVK.dylib`
  beside the host library, then Homebrew's and `/usr/local`'s, and gives both the one path
  (`GRANITE_VULKAN_LIBRARY`, `SDL_HINT_VULKAN_LIBRARY`; either, when already set, wins). On the M1 MoltenVK
  reports Vulkan 1.4; Granite notes it emulates events and push descriptors on Metal, and that it has no
  calibrated timestamps (a profiling feature only).
- **Paths**: the exe's folder from `_NSGetExecutablePath` (the host library, `text/en.tsv`), the data
  folder in `~/Library/Application Support`.

## The address model

Display-list words, the game's records and its raw `D_` labels hold 32-bit addresses: RDRAM at
`0x80000000..0x80800000` (N64 bytes) and the game's own image from `0x10000000` (native data;
lower addresses read as segmented ones). Windows and Linux put both exactly there, below 4 GiB, so a
32-bit address *is* the pointer (docs/P0_STATUS.md, "Address model").

arm64 macOS cannot do that. Its kernel kills an arm64 program whose `__PAGEZERO` is smaller than
4 GiB, every arm64 program is position independent, and the low 4 GiB is never mapped. So on arm64
macOS the image and RDRAM sit in a 4 GiB window higher up, at `PORT_ADDR_HI` = `0x8_00000000`:

| | Windows / Linux | arm64 macOS |
|---|---|---|
| image | `0x0_10000000` (fixed) | `0x8_10000000` (+ a small ASLR slide) |
| RDRAM | `0x0_80000000` | `0x8_80000000` |
| raw `D_` labels | `0x0_8xxxxxxx` | `0x8_8xxxxxxx` (absolute Mach-O symbols) |

Their low 32 bits are the same, so a pointer cut to 32 bits is still its address: display lists,
`osVirtualToPhysical` and every `(u32)(uptr)p` work unchanged. Only a 32-bit address turned back into a
pointer needs the window put back, and `port/include/port/addr.h` does that:

- `PORT_PTR(a)`: a 32-bit address (an integer) as a pointer. A constant expression for a constant, so
  static tables use it (`(ChestGroup *)PORT_PTR(0x80199140)`). Passing a pointer is a compile error.
- `PORT_ADDR_OF(p)`: a pointer as its 32-bit address, kept as a `uintptr_t`, for range checks such as
  `Port_InRdram`.
- `PORT_ADDR_NARROW(w)` / `PORT_PTR_ANY(w)`: for words that may hold either a 32-bit address or a whole
  host pointer (the native 2D draw takes stack buffers as well as list addresses).

`PORT_ADDR_HI` is `0` everywhere but arm64 macOS, and each macro then compiles to the cast it replaced
(the same sign or zero extension, nothing added). A 32-bit address made into a pointer *without*
`PORT_PTR` on macOS lands in the unmapped low 4 GiB and faults at the first access, so a missed site
crashes at once instead of corrupting memory.

### Why `0x8_00000000`

The window must hold nothing else. `0x1_` holds malloc's small blocks and the stack, and the dyld shared
cache reserves `0x1_80000000..0xF_C0000000` for a program linked in the usual place. A program whose image
is linked outside `0x1_` (ours, at `0x8_10000000`) gets the shared cache at `0x260_00000000` instead, which
leaves `0x2_`..`0xF_` empty; `mmap` hands out memory from `0x70_00000000` up. The heap staying in `0x1_`
means a heap pointer can never pass for an image or RDRAM address, and `osVirtualToPhysical` refuses one.

RDRAM is taken with `mach_vm_allocate(..., VM_FLAGS_FIXED)`, which fails rather than replace a mapping
(macOS has no `MAP_FIXED_NOREPLACE`, and it ignores a plain `mmap` hint that low). `Port_HostInit` also
checks that the image is in the window at or above `0x10000000`. Measured on an M1, macOS 27.2: 40 runs
put the image between `0x8_100xxxxx` and `0x8_14dxxxxx`.

### What changed

- `port/include/port/addr.h` (new), included by `PR/ultratypes.h`.
- About 60 runtime casts of 32-bit addresses (port/, game/native_draw, and the decomp patch: anim, enemy,
  cutscene, scene, effect, keyframe, hud, audio, cseq) and 649 constant RDRAM addresses in static tables
  (menu.c, spirit.c, chest.c, effect.c, map.c, pakmenu.c, overlay_load.c, npc.c, player.c) go through
  `PORT_PTR`. ROM offsets kept in pointer fields stay as they are (they are never dereferenced; they are
  marked `/* port: a ROM address */`), as do values passed through `void *` parameters.
- `Port_InRdram`, the native 2D draw's `resolve`, frame_image's native addresses, and `port/p0/host.c`:
  RDRAM's N64 address (`RDRAM_N64`) and where it is on the host (`RDRAM_BASE`) are told apart; macOS gets
  the fixed Mach allocation, the image bounds from its Mach-O header, and a stricter
  `osVirtualToPhysical`.
- `tools/gen_abs_symbols.py --macho`: the raw `D_` labels as Mach-O absolute symbols in the window (it
  reads `PORT_ADDR_HI` from addr.h). Every label gets the window, segmented ones included: cut to 32 bits
  they are the same words, and they stay within the ±4 GiB that arm64's `ADRP` reaches from the code.

### Checks

- `tools/find_addr_casts.py COMMANDS --check` reads clang's AST for every file and lists each
  integer-to-pointer cast and each pointer compared with a 32-bit address; `--check` fails on any that
  is not through `PORT_PTR` and not marked as "not an address" or "a ROM address". `COMMANDS` is the
  compile lines from `make -n` (`clang ... -c file.c -o file.o`, one per line). It reports 0 now.
- `tests/addr_test.c`: the macros, with RDRAM mapped where the platform keeps it (on macOS, link it at
  `0x8_10000000`, as its header says). Passes on arm64 macOS and on x86_64.
- **Windows and Linux unchanged**, checked by compiling all 145 objects of the P1 build (`P1_OS=1`, audio
  on, asset arrays stubbed, `-g0`) for x86-64 from `main` and from this change, and comparing every
  object's code, data, relocations and symbols: all identical. `port/p0/host.c` was compiled with its
  macOS branches switched off for that, since the comparison ran on a Mac. `en_images.c` (needs ROM data)
  and `fiber.c` (x64 assembly) do not build there either way; their changes were read instead.
- The second round (fibers, link, host library, paths) checked the same way: `fiber.c`, `host_bridge.c`,
  `en_text.c` and the generated address map compile for x86-64 as at the first round's commit (the
  macOS branches switched off, as the check ran on a Mac; `fiber.c` differs only in the inline asm's
  source position).
- Not done: a run with the real ROM.

## Risks

- A 32-bit address reaching a pointer some way the AST check cannot see (through a `uintptr_t`
  variable, or memcpy into a pointer field). On macOS it faults at the first access, at the site.
- The shared cache moving when the image is linked high is the kernel's own behaviour, seen on macOS 27;
  if a later macOS keeps it in `0x2_`..`0xF_`, RDRAM's fixed allocation fails at startup with a message
  instead of breaking anything, and the window moves.
- arm64 converts out-of-range floats to integers differently from x86 (it saturates). If the game does
  that anywhere, headless results on macOS could differ from the Windows and Linux ones. The UBSan build
  (`-fsanitize=float-cast-overflow`, docs/MEMORY_SAFETY.md) finds such spots.
