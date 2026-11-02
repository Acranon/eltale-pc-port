# Memory safety: the sanitizer build and the sweep

N64 games often read or write past an array and get away with it, because on the console the bytes after it are
always the same neighbour. In the port the linker puts the game's globals wherever it likes (and differently on
Windows and Linux), so the same overrun silently changes another variable or crashes. The first one found
(c65951e, `anim_node_world_positions[4]`) overwrote the native renderer's pointer and crashed battles. This page is
the tool that hunts for the rest: an AddressSanitizer + UndefinedBehaviorSanitizer build of the game, a headless
sweep through everything that runs without a window, and what it found (all fixed or explained below).

## Building it

WSL (Ubuntu 24.04) with clang 18:

```sh
sudo apt-get install clang-18 libclang-rt-18-dev        # once
python3 tools/patches.py apply                           # work/ from patches/p0.patch
make asan ROM=/mnt/c/code/quest64/rom/eltalemonsters.jp.z64
```

`make asan` builds `out/asan/eltale-p1` (P1, audio on) and `out/asan/libeltale_host.so` (the stub host, below).
Build from a copy on the WSL file system (`rsync -a --exclude=/out --exclude=.git ./ ~/el-asan/`): /mnt/c is slow
and its clocks confuse make. A normal build is not changed by any of this (`SANITIZE` is off unless `make asan`).

What is instrumented (`SAN_CFLAGS` in the Makefile): all game code (work/src, the overlays, libultra's gu, io and
audio), all port C code (port/, game/native_draw), with

- AddressSanitizer: every global gets redzones (so an overrun of one game array into the next is a report),
  stacks (fiber-aware: port/os/fiber.c tells ASan of every switch), heap, mem* calls;
- UBSan: `array-bounds` (indexing past a declared array size, even where no redzone is: inside structs, inside
  RDRAM), `pointer-overflow`, `object-size`, `vla-bound`, `shift-exponent`, `float-cast-overflow`, `builtin`,
  `return`, `unreachable`, `bool`, `enum`, `nonnull-attribute`;
- every check recovers (`-fsanitize-recover=all`, `halt_on_error=0`): a run goes on and lists every report (ASan
  reports each place once per process). Leak checking is off (the game keeps everything for the whole run).

## How RDRAM fits in

The N64's RDRAM is one 8 MiB block mapped at 0x80000000 (display lists hold 32-bit addresses, and the game's code
names RDRAM by its N64 addresses). x86-64 Linux ASan keeps its *low shadow* at 0x7FFF8000..0x8FFF6FFF, right where
RDRAM goes, and a protected gap above it where RDRAM's own shadow would be. Rebuilding the ASan runtime with
another shadow offset would avoid that; instead `port/p0/asan_rdram.c` works with the stock runtime:

- it maps RDRAM over those 8 MiB of low shadow (they would only shadow host addresses 0x40000..0x4040000, where
  nothing lives: the image starts at 0x10000000) and maps RDRAM's shadow (0x8FFF8000..0x900F8000, in the gap) as
  ordinary zeroed memory: every RDRAM byte addressable, so instrumented loads and stores of RDRAM just work;
- ASan's own memcpy/memmove/memset refuse anything outside its "application memory", RDRAM included, so the build
  renames the calls the instrumentation makes (`-mllvm -asan-memory-access-callback-prefix=eltale_asan_`, with
  the call threshold set so no other callbacks are made) to `eltale_asan_mem*` in asan_rdram.c: a copy that touches
  RDRAM checks its RDRAM side against RDRAM's end and its other side with ASan; any other copy goes to ASan's.

`ELTALE_ASAN_RDRAM=loaded` looks inside RDRAM too: RDRAM starts poisoned except the IPL copy of the main segment
(0x80000000..0x80100400), the two frame buffers and the port's own pages (0x807F0000..), and every ROM DMA unpoisons
what it fills (osPiStartDma calls `Port_AsanRdramLoaded`). Reading or writing RDRAM that nothing loaded (past the
end of a loaded block, or a stale pointer into an unloaded area) is then a report, mem* copies included.

## The stub host

The game's native draw side (game/native_draw: the model loader, the recordings, 2D, the area map's feed) only runs
when the renderer is there (`--dump-frames`). The real renderer (port/host, port/render: C++, Vulkan, SDL3) is not
part of this build; `tools/asan_host_stub.c`, built as `libeltale_host.so` beside the exe, stands in for it: no GPU,
no window. It reads through every buffer the game hands over (each read checked by ASan) and checks what the real
renderer would trust (a mesh's groups inside its vertices and materials, tile texels inside what was uploaded,
ids handed out, light counts, the area map's counts and strings) and prints `[asan] host stub: ...` for a broken
one. It keeps the real renderer's buffer sizes (and `ELTALE_ND_SMALL_BUFFERS`, so `resources_reset` runs), takes
the picture's shape from `ELTALE_WIDESCREEN` as the headless renderer does, and writes blank pictures for the
frames asked, so the test tools that read pictures run to their end (their picture checks fail, of course).

## Running the sweep

```sh
tools/asan_run.sh --rom /mnt/c/code/quest64/rom/eltalemonsters.jp.z64 --logs ~/asanlogs/run1 \
    checks saves frames native long rdram tools
```

One game process at a time; each run's reports go to `LOGS/<run>.<pid>` (`ASAN_OPTIONS=log_path`), its output to
`LOGS/<run>.out`/`.err`, a line per run to `LOGS/runs.txt`, and at the end `tools/asan_summary.py` lists the
distinct findings (keyed by kind and first game/port frame, with the logs they came from) in `LOGS/summary.txt`.
Reports already looked at and found harmless are marked `known` there (its `KNOWN` table; the list below).

| suite | what runs |
| --- | --- |
| checks | `tests/p1_checks.sh --tour`: New Game, ending, camera tour, every map's sub-area with battles, save at an inn and load, the Pak manager, quick saves (and the damaged ones); its hashes must be the normal build's |
| saves | a Controller Pak with **two** saves (`tools/pak_two_saves.py`, three variants of the copy's tail): load the first, the title's Continue (save menu), the Pak manager |
| frames | every run of `tests/render_frames*.txt` (battle, spells, spell input, element allocation, item menu, title, fonts, intro, save menu, compass, items, chests, ending, English) with `--dump-frames` (native draw side), and the 4:3/2D lists again with `ELTALE_FRAME_IMAGE` (the frame image) |
| native | New Game (3000 frames) and the ending in Japanese and English, the camera tour, the map tour three times (4:3; English at 16:9; small buffers, so the renderer's resources reset in play), all with `--dump-frames` |
| long | battles the player steps back from and swings at the air in (the enemies attack, battles end and start again), the map tour with `ELTALE_BATTLE_LOG=2`, each quick save's battle with spells cast, 20000 frames each |
| rdram | `ELTALE_ASAN_RDRAM=loaded`: New Game, ending, camera tour, map tour (plain and native), a battle with spells, the walk-away battles, the item menu |
| tools | `spell_ui_test`, `mouse_menu_test`, `pause_menu_test`, `interp_test` against this build (mouse rows, the spell panel and quick bar, the pause menu, 16:9) |

`ELTALE_ASAN_LOG` (set by the runner) also catches the port's own reports (`[asan] RDRAM ... runs past RDRAM's
end`, `[asan] host stub: ...`).

The whole sweep takes about 15 minutes on this machine (the ASan build is fast headless: the map tour's 30374 frames
in 5 to 50 seconds; the three Pak manager runs with two saves wait out their 60 s, since one save stays). `make asan
SAN_EXTRA=-fstrict-flex-arrays=2 ASAN_OUT=out/asan-sfa` also bounds-checks a struct's last array (by default clang
treats every trailing array as variable length); run it with `--exe out/asan-sfa/eltale-p1`.

The sweep of 2026-11-02: the first pass found M1-M5 (before the fixes); then, on the fixed build, every suite at least
once (checks, saves, frames, native, long, rdram, tools: about 100 game processes per pass, several passes), and the
whole set again with `-fstrict-flex-arrays=2`.

## Findings (2026-11-02, main 4f19049)

M1-M5 were fixed in `patches/p0.patch` (each with a comment naming it); no hash changed (below).

| | where | what | whose | effect | resolution |
| --- | --- | --- | --- | --- | --- |
| **M3** | save_menu.c `SaveMenu_RefreshFileList` (Pak_ReadFile) | reads 0x80 bytes of each save into the 0x68-byte `save_menu_header_read_scratch`; on the N64 the 0x18 left over land in `D_80090358` (unreferenced) | retail, layout-dependent | **Windows: a crash.** The 0x18 bytes land on `save_menu_operation_panel_index`, `save_menu_pak_error`, `save_menu_repair_count` and the first 12 bytes of `save_menu_save_headers[0]` (the first save's `unk0`, `anim_set_index`, `hp`, `max_hp`, `mp`, `max_mp`). With two saves on the Pak the second file's tail (normally zeros) overwrites the first save's stats: its HP and MP show 0 in the save menu and loading it divides by max HP 0 in `Hud_DrawPlayerMeters` (exception c0000094). Reproduced on the 2e8bbc0 Windows build with a two-save Pak; main's exe has the same layout. Linux happened to have alignment padding there | read into a 0x80-byte buffer, then the header and `D_80090358` each get their part |
| M4 | save_menu.c `SaveMenu_SaveSelectedFile` (Pak_WriteFile) | writes 0xE0 bytes from the 0xC8-byte `save_menu_game_state_block` (a read overrun into the next global) | retail | the save file's bytes 0x148..0x160 were whatever the linker put after the block; loading reads 0xE0 and keeps 0xC8, so never used | written from a zero-padded 0xE0 buffer |
| M5 | input.c `Pak_WriteFile` (port's header swap) | the header write takes 0x80 bytes from a 0x68-byte `SaveHeader` (the next slot's header; past the array for slot 15) | retail | never read back (M3 puts those bytes in `D_80090358`) | bytes 0x68..0x80 written as zeros |
| M1 | npc.c `Npc_SetMessage` from `Npc_InitArea` | copies 0x4B0 bytes from the 0x2F8-byte `npc_area_entry_message` (a read overrun, every area with an entry message) | retail | the text ends (0xFF) at byte 749, so the bytes after are never shown | the port's array is 0x4B0 bytes, zeros after the message |
| M2 | text_window.c `Text_OpenWindow` | copies 24 bytes from each of the inn's two 8-byte choice rows (`npc_rest_choice_row0/1`) | retail | each row ends (0xFF) within its 8 bytes: never shown | the port's rows are 24 bytes |
| A1 | libultra audio env.c:540 `_getRate` | float to `s16` conversion out of range (UB in C) | retail libultra | x86 converts through a 32-bit integer as the N64's `trunc.w.s` does, so the same value; not memory | left: port/audio is being replaced (native audio); for its owner |
| B1 | env.c:150 `Env_Update` | `map_backdrop_phase_tluts[0] + paletteIndex * 256` indexes past row 0 | decomp (matching) | inside the `[4][256]` array: harmless | known, left as is |
| B2 | camera.c:733 `Camera_UpdateHeightOscillation` | a `Vec2f` passed as `Vec3f`, `.y` written | decomp (matching) | `.y` is inside the `Vec2f`: harmless | known, left as is |
| R1 | en_images.c `Port_ImagesUpdate` (rdram mode only) | the port writes the English pictures into RDRAM's common block at boot, before it is loaded | port | none (not a load the DMA hook sees) | known |
| A2 | port/audio/bank.c, game audio.c, libultra csplayer.c, seqplayer.c (strict flex arrays only) | libultra's `[1]` arrays (`ALBank.instArray`, `ALInstrument.soundArray`, `ALSeqFile.seqArray`) indexed past 1 | the N64 SDK's variable-length idiom | none: the records are that long | known |

Nothing else came up: no heap or stack overrun, no host-stub rule broken (meshes, tiles, palettes, ids, the area
map's feed all consistent over every run), no RDRAM copy past RDRAM's end, in the rdram mode no read or write of
RDRAM nothing had loaded (New Game, the ending, both tours, battles, the menus), and with strict flex arrays nothing
outside libultra's audio idiom. Every run's hashes were the normal build's (the frame lists with `--dump-frames` are
the native renderer's constant 6472f32d00cadc74, as `tests/host_checks.sh` prints).

With the fixes, the Windows build's checks all pass with the same hashes: `tests/p1_checks.sh --tour` (New Game audio
be0b67b7b718648e, last_hash 74b8baf63f9c1889; ending 6b50454cc76480b3; camera tour 02b00a69b6c72d1a; map tour
a2d2e81245d538d4 / 46b61f5a7d79c61c; Pak manager 43842e6ff23bd261; save, load and quick saves as before),
`tests/host_checks.sh` (6472f32d00cadc74), `frame_test.py --native`, `interp_test.py`, `warm_test.py --check`,
`mouse_menu_test.py`, `mouse_host_test.py`, `automap_test.py`, `spell_ui_test.py`, `pause_menu_test.py`. A Pak with two
saves now loads the first one with its own stats (it crashed before).

The validation: the motivating retail array put back (`anim_node_world_positions[4]` with no bound, a separate
build) ran the frames suite, both tours, every save's battle and the idle battle without the overrun: no model drawn
in those headless runs records more than 4 node positions (an instrumented count printed none either), so the
sweep cannot say which model did it (the owner's crash was in a windowed battle walk and after a game over; no
scripted run here reaches a game over). The class itself, a global overrun into the next object, is what M1-M3
are, and ASan reported each of those on its first occurrence.

## Blind spots

- **Inside RDRAM** ASan sees one block. Overruns from one table into the next table of the same loaded block, or
  from one loaded block into another, are invisible. The rdram mode catches reads and writes of RDRAM that was
  never loaded. UBSan's `array-bounds` would check indexing, but the RDRAM labels are declared without sizes
  (`extern u8 D_8039ADB0[]`, the magic, common-block and scene tables), so it has nothing to check against.
- **The real host** (eltale_host.dll: SDL3, ImGui, the Vulkan renderer, the F1 menu, the area map's drawing,
  settings, sound output) is not instrumented; the stub only checks what crosses into it.
- **Window-only paths**: input from SDL (keyboard, mouse events, the F1 menu, the reset snapshot, Alt+Enter, the
  area map's hover and zones buttons) need a window and are not in the sweep (`automap_test`, `mouse_host_test`,
  `warm_test`, `pace_test`, `lifetime_test`'s reset are window tests). `_WIN32` code (the crash handler, the
  reset's section snapshot) is Windows-only.
- **Uninitialised reads** (MemorySanitizer) are not checked: MSan needs every library instrumented. The fiber
  stacks start zeroed and the port's globals are zero, which hides most of them anyway.
- **Use after return** is off (`detect_stack_use_after_return=0`): the fibers' fake stacks would need more care.
- Only what the scripted runs reach: a game over, a boss fight, the late game's areas in play (the map tour visits
  them without playing), many saves on the Pak, and every spell's every stage are not all covered. The sanitizer
  build is the tool for those later: play it under WSL with the ROM (headless runs only so far; a window needs the
  real host, which this build does not have).

## Files

- Makefile `asan` target, `SANITIZE=1` flags
- port/include/port/sanitize.h, port/p0/asan_rdram.c (RDRAM in ASan's layout, the mem* wrappers, the rdram mode,
  ASan's default options), host.c (RDRAM mapping and the DMA hook under `PORT_ASAN`), port/os/fiber.c (ASan
  fiber switches)
- tools/asan_host_stub.c, tools/asan_run.sh, tools/asan_summary.py, tools/pak_two_saves.py
