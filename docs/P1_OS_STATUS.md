# P1a OS status

Brief: `~/eltale-notes/P1_OS.md` (2026-10-28). The P0 build remains the
regression target while the P1 OS replaces its synchronous host stand-in.

## OS1 foundation

- `port/os/fiber.c` adapts Holyland's fixed-slot x64 fiber switch for Windows
  and Linux. It saves nonvolatile registers and FP control state, and the
  scheduler requires its stacks at the fixed address before running the game.
- `port/os/kernel.c` schedules the highest-priority runnable N64 thread.
  StartThread, SetThreadPri and message send wake-ups preempt immediately for
  a higher priority; equal priorities use FIFO order. Receive on an empty
  queue and send on a full queue block, while NOBLOCK returns -1. JamMesg
  inserts at the front. The interrupt mask API returns its old value.
- `python tools/build_windows.py --p1-os --rom ...` boots through all five
  game/scheduler threads on fibers. The P0 path remains a separate regression
  build.
- Windows and Linux fiber/scheduler tests cover switching, priority
  preemption, receiver and sender blocking, NOBLOCK, JamMesg and equal-priority
  FIFO. P0 Windows/WSL boot checks remain green.

## OS2: virtual events and completed frames

- The OS clock advances at 46.875 MHz (781,250 counts per NTSC VI), matching
  libultra's initialized `osClockRate`. `osGetCount`, `osGetTime`, one-shot and
  repeating timers use that virtual clock. `--fast` skips wall pacing; normal
  mode paces virtual time without feeding host time into game logic.
- PI DMA copies the ROM bytes and posts completion at the next scheduler
  switch. VI retraces, SP and DP completion use registered OS event queues.
  The game scheduler thread handles those messages and graphics worker
  finishes tasks through `osViSwapBuffer`.
- AI accepts up to two virtual PCM transfers, derives their completion times
  from the programmed sample rate, reports remaining bytes/status and posts
  AI events. PCM output itself is reserved for P1c. SP yield reports that the
  headless task has already completed.
- `osSpTaskStartGo` hashes each game's display list and posts SP/DP events.
  The hash normalizes address-bearing words through a generated host-symbol
  to pinned-N64-address map; the actual 32-bit Gfx words stay unchanged.
  P1 `osVirtualToPhysical` passes 32-bit addresses through, including KSEG0
  and the host frame data pointer. Hashing maps that frame pointer to a stable
  trace address on each host.
- `--headless --fast --frames N` stops after N completed graphics frames.
  Windows and Linux matched **all 60 frame hashes and command counts**, and
  repeated Windows runs matched exactly. After the P1 address-model correction,
  frame 60 has 120 commands and hash `3ec25a41519e00b0`.
- `tests/os_events_test.c` checks a timer at half a VI, retrace timing,
  deferred PI completion, SP/DP event delivery, and the two-entry AI FIFO.
  Fiber and scheduler tests
  continue to pass on Windows and Linux.

## OS3: pure-C libultra math

- The P1 executable compiles and links all ten `decomp/src/libultra/gu/*.c`
  sources natively. The matching `gu*` and `sins` fail-fast stand-ins are gone.
- `tests/gu_test.c` checks `guMtxF2L`/`guMtxIdent` packet words, including
  truncation of ±0.75 fixed-point units toward zero, and checks sine-table
  quadrants. It passes with native Windows Clang and Linux GCC. No other
  libultra/libc pure-C object is needed by the current intro task path.

## Busy-wait audit

`Core_RunGameplayLoop`, `Core_RunMessageCutsceneLoop` and
`Core_RunCameraCutsceneLoop` each poll `nnsc_retrace_count < 2` without a
blocking OS call. They need a cooperative yield point before those modes
run. Their audio-stop waits call `Audio_GetSeqState` inside a loop; the native
audio implementation must advance virtual time there. `audio.c` polls
`osAiGetLength` in its task loop; that host call must yield/advance a VI when
the queue is empty. `nnScWaitTaskReady` polls the framebuffer with a blocking
message receive, so it already yields. The idle thread's `for(;;)` is handled
by the kernel: after it starts main, the scheduler advances an event instead
of running that spin again.

## OS4: scripted input and Controller Pak

Windows title smoke run:

```powershell
python tools/build_windows.py --p1-os --frames 100 --input tests/intro-to-title.input --pak out/p1-windows/journey.mpk --rom C:\path\to\eltalemonsters.jp.z64
```

Linux/WSL equivalent:

```sh
make P1_OS=1 OUT=out/p1 p1-os-run ROM=/path/to/baserom.jp.z64 FRAMES=100 INPUT=tests/intro-to-title.input PAK=out/p1/journey.mpk
```

- `--input path` reads ordered `VI held_buttons stick_x stick_y` rows. The
  controller state holds between rows; button edges are still computed by the
  game's `Input_ReadController`. `--pak path` uses a raw 32 KiB Controller Pak
  image, creating a formatted image when the file is absent. With no path,
  the Pak is volatile. `tests/pak_test.c` allocates a two-page game file,
  writes it, reloads the image, reads it back, checks the mirrored inode table
  and raw page bytes, and deletes it.
- `osPfs*` file operations use the Pak's big-endian ID, inode and directory
  layout. The game now calls these functions through its actual input module.
  Motor calls are harmless success; the GB Pak probe reports a device error.
- Frame pacing (Claude's review): `osViSwapBuffer` only queues the buffer, which
  becomes current at the next retrace as in libultra's VI, and a graphics
  task's SP/DP done events are due at the next retrace (the RSP/RDP take real
  time on the N64). Virtual time then advances only through the scheduler, and
  the intro runs at 2 VIs per frame (frame 60 at VI 118) with the same
  display-list hashes as before. An earlier version advanced one VI inside
  `osViSwapBuffer`, which ran the game at 1 VI per frame. The task duration sets
  the frame rate, so it must be calibrated against an emulator's frames-per-VI
  for the intro and gameplay before P1's picture comparisons.
- `tests/intro-to-title.input` reaches the title through the game's own
  intro/title loops. Windows and Linux both finish a 100-frame run with
  `mode=3`, 63 DMA chunks and hash `9945f9cc1c2b7a5e`. Both create a
  32 KiB `.mpk` image. Native title code is linked in P1.
- `tests/title-new-game.input` enters the title's New Game path. Map loading
  currently requests invalid ROM offset `0x7796e15f`. The map area data in
  RDRAM is still raw N64 bytes, while the native `EnvAreaSettings` type has
  64-bit pointers and a different stride. Decoding and relocating those
  records belongs to the next data port slice. Consequently the OS4 brief's
  full gameplay save/load milestone is not yet met; the Pak API write/reload
  path is verified independently. Game save record byte order is also still
  the separate endian pass named in the brief.

## Remaining P1 work

- Decode and relocate the map area data that gameplay reads after the title,
  then drive a save and load through the in-game menu.
- Native audio synthesis and rendering remain the later P1 slices. Their
  sequence and SFX calls are quiescent stand-ins for the headless OS path.

## P1 step 1: New Game reaches gameplay (Claude, 2026-10-28)

`tests/title-new-game.input` (Start every 30 VIs) now goes boot, intro, title,
New Game and runs gameplay: 700 frames headless reach `mode=1` with 914-command
display lists, on Windows and Linux, with no crash. What it took:

- **Map area records** (`port/data/map_area.c`): after Map_LoadArea's DMA the
  area block's record graph (sub-areas, grids, map objects, models with their
  collision triangles and vertices, exits, entry points, environment keyframes,
  camera presets, walls, encounter tables, NPC lists) is decoded into native
  copies, and `map_area_def` points at the copy. Display lists, textures and
  vertices stay N64 bytes in RDRAM at their N64 addresses. Array lengths come
  from the records' counts, the largest index used, or the next record start
  in the block. Chest groups are decoded the same way (`Port_ChestGroup`).
- **Animation records keep their N64 layout** (`port/data/anim.c`): AnimSet,
  AnimNode and KfAnim hold addresses as `u32`, because the game indexes them
  with N64 strides and reads fields at N64 offsets (PlayerAnimTable). They are
  byte-swapped in place in RDRAM. A bitmap over RDRAM marks swapped halfwords,
  so shared records swap once, and a DMA over a range forgets it. The anim
  functions swap their set on use (its nodes can be in a block loaded later).
  The game's own AnimNodes in menu.c point at their keyframes' RDRAM copies
  (`PORT_ADDR`), swapped once at startup.
- **Address labels inside native objects** (player.h, env.h, menu.h, map.c):
  the N64 labels the matching build uses for fields (`D_8007AA70` =
  `player_state.actor.animSetIndex`, ...) are macros for those fields on the
  host, and two biased-base labels in MapObj_DrawAnimTexBillboards read their
  tables directly. NpcDef's byte view of `boss_index` is ordered for the host.
- **Decomp fix** (eltale-decomp 93b0ca0): the day-phase table was split in two,
  and Env_InitForMap walked past the first part.
- `-Werror=int-conversion` on Linux too (Clang already errors).

Open after step 1:

- ~~Windows and Linux diverged from frame 10.~~ Fixed: all 700 frames of the
  New Game run now hash the same on both. Two causes: GCC's strict aliasing
  (the title fade is read through a second struct type; both builds now use
  `-fno-strict-aliasing -fwrapv`, as holyland does), and the fade colour ramps
  being split at their labels, so Fade_Start walked into whatever the linker
  put next (eltale-decomp e92a411 merges them; the labels are aliases, macros
  in the port).
- Other N64-pointer tables in native data still read RDRAM through native types
  (enemy area sets and EnemyDefs, boss data, scenes, cutscene camera tracks,
  map-object animations): they crash or misbehave when reached. Same two tools
  apply: decode into native copies, or N64 layout plus in-place swap.
- AnimSet table ends are found by validity checks, which could swap data that
  only looks like an AnimSet; table lengths from the data would be safer.
- No in-game save yet; the gameplay busy-wait did not hang in this run but is
  still a poll.

## P1 step 2 (renderer), started: the frame image (Claude, 2026-10-28)

A renderer that reads RDRAM like the RSP/RDP (paraLLEl-RDP with an LLE RSP, or
RT64, which the Quest 64 recomp already uses for this game) needs one N64-format
RDRAM image per frame. `port/gfx/frame_image.c` builds it from a graphics task:
it starts from a copy of RDRAM, walks the display list (F3DEX 1.23 address
commands: MTX, MOVEMEM, VTX, DL, BRANCH_Z, MOVEWORD segments, SETTIMG with the
following loads, SETCIMG/SETZIMG), converts every native object it reaches by
how the list uses it (lists with their address words rewritten, matrices, vertices,
16/32-bit texels and TLUTs, viewports) and writes it big-endian at its N64
address (decomp symbol addresses via the generated host map; the frame data at
0x80301000 + i*0xDAD0 + 0x58). `ELTALE_FRAME_IMAGE=1` logs per frame: commands,
matrices, vertices, texture loads, native bytes converted, unresolved addresses.

- Linux now links at 0x10000000 like Windows (`-Ttext-segment`): native
  addresses below 0x10000000 looked like segmented ones.
- Gameplay frames: ~2650 commands, 63 matrices, 1050 vertices, 157 texture
  loads, no unresolved address except one: a SETTIMG of `0xB0AF3980`, which is
  `0x8039AFB0` byte-swapped. Native code read a texture pointer from an N64
  table in RDRAM without swapping it (a HUD table at 0x8039xxxx is the likely
  source, e.g. hud.c's `D_803A8778`): one more of the step-1 leftovers, which
  the walker now finds.

Next: fix that read; choose the backend (paraLLEl-RDP + an LLE RSP running the
ROM's F3DEX, the plan's reference, or RT64 first for a quick picture) and add
the SDL window; then frame tests against an emulator.

### Next data job: the common block at 0x803A0000 (found 2026-10-28)

The game reads ~20 tables at fixed RDRAM addresses in the common data block (HUD,
items, magic visuals, enemy effects, boss data) through native types: big-endian
bytes, and pointer tables read with 8-byte strides. The texture pointer above is
one (`D_803A8778[pal]`, Hud). Indexed uses (game/): D_803AB36C (8), D_803AAAA0
(5), D_803AAA40 (5), D_803AC418, D_803AB390, D_803AB358, D_803AB288 (2 each),
D_803AC438, D_803ABA58, D_803AB820, D_803AB4B0, D_803AB48C, D_803AB3D0,
D_803AB2F0, D_803AA6C4, D_803AA690, D_803AA628, D_803A8778 (1 each). Find the
DMA that loads the block, give each table an N64-layout type (u32 for
pointers), and swap the block in place after the DMA as port/data/anim.c does,
or decode the tables into native copies.

Update: the unswapped texture pointer was HudTextureDesc (D_803A86F8 and the
other descriptors in the common block). Hud_LoadTextureTile now reads a
descriptor in RDRAM as N64 fields (port/data.h has Port_N64U16/U32/Ptr for
this), and Hud's palette table D_803A8778 is read the same way. Gameplay
frames now have no unresolved address; the 700-frame run ends at hash
74b8baf63f9c1889 on Windows and Linux. The other common-block tables above
are still read natively.
