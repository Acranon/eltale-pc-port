# macOS (Apple Silicon, native arm64)

Brief (2026-11-25): run the port natively on arm64 macOS, without Rosetta, and leave the Windows and
Linux builds exactly as they are. Rosetta was ruled out: it is being wound down after macOS 27.

## Where it is (2026-11-26)

- **Done: the address model.** The game's code builds for arm64 macOS with its 32-bit addresses
  placed one window higher up (below). Windows and Linux compile to the same machine code as before.
- **Next:** the arm64 fiber switch (`port/os/fiber.c` has x64 only), the macOS build and link
  (`Makefile`/a `tools/build_macos.sh`: `-segaddr`, `gen_abs_symbols.py --macho`, no `-mcmodel=large`),
  `libeltale_host.dylib` and its loading (`port/gfx/host_bridge.c`), the data folder, then the renderer
  on MoltenVK. None of it has run on a Mac yet: there is no ROM on the machine this was written on.

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
- Not done: running the game on macOS (that needs the remaining steps above, and a ROM).

## Risks

- A 32-bit address reaching a pointer some way the AST check cannot see (through a `uintptr_t`
  variable, or memcpy into a pointer field). On macOS it faults at the first access, at the site.
- The shared cache moving when the image is linked high is the kernel's own behaviour, seen on macOS 27;
  if a later macOS keeps it in `0x2_`..`0xF_`, RDRAM's fixed allocation fails at startup with a message
  instead of breaking anything, and the window moves.
- arm64 converts out-of-range floats to integers differently from x86 (it saturates). If the game does
  that anywhere, headless results on macOS could differ from the Windows and Linux ones. The UBSan build
  (`-fsanitize=float-cast-overflow`, docs/MEMORY_SAFETY.md) finds such spots.
