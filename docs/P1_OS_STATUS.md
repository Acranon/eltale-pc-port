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

Update 3: the common block is closed.
- EnemyDef keeps its N64 layout (`attacks` is a u32 address) and is swapped in
  place with its attack lists (port/data/enemy.c): the boss defs in the block
  (D_803AB820[7], D_803AB9A8) at boot, and each area's defs after
  Enemy_LoadAreaSet's DMA. An area set's defs run up to its anim-set table; the
  port stops if that span is not a whole number of defs. Enemy anim-set tables
  are read with Port_N64Ptr.
- Table counts were checked with `ELTALE_DUMP_COMMON=1` (prints every swapped
  record and the boss defs). Three were too long and were swapping the next data:
  ItemDef is 25 (then the item name strings), EnemyEffectDef at 0x803AA6C4 is 5
  (then vertices), CameraPreset is 8. The boss defs read back right (HP 200 to
  2300, attack lists back to back from 0x803AB670).
- Renderer: RT64 first (it already draws this game in the Quest 64 recomp), then
  our own native renderer later, as for KF4.

## P1 step 2: RT64 in a window (Claude, 2026-10-28)

`--window` draws with RT64 (third_party/rt64, pinned to the Quest 64 recomp's
commit aa047b8). RT64 is C++ built with clang-cl, so it lives in its own DLL with
a C interface (port/rt64: eltale_rt64.h/.cpp, built by `tools/build_rt64.cmd`
into out/rt64; copy its DLLs next to eltale-p1.exe). The game side
(port/gfx/rt64_bridge.c) loads it at run time, draws each graphics task from its
frame image, and presents at each retrace with VI registers computed from the
game's OSViMode (port/gfx/vi.c; libultra's vitbl.c is now linked). The title and
the opening dialogue draw correctly.

- The framebuffers (0x803B5000, 0x803DA800) and depth buffer (0x802DA800) are
  kept out of the per-frame RDRAM copy: copying the CPU's empty copy over them
  made RT64 show that instead, and the window blinked black.
- Not yet: Linux build of the DLL, controller input in the window (the scripted
  input still drives it), audio, frame pacing checks, fullscreen/config.

## P1 step 3: sound (plan, 2026-10-28)

The game's audio thread (game/audio/audio.c, now stubbed out in P1) drives
libultra's synthesizer, which builds aspMain command lists (Acmd, ABI 1) each
audio frame; the RSP runs them and the AI plays the output. Plan:

1. **Build the real audio code natively**: game/audio/audio.c and
   decomp/src/libultra/audio/*.c in the P1 build, dropping the Audio_* stubs
   (port/p0/stubs.c, unavailable.c).
2. **Bank and sequence files**: alBnkfNew and alSeqFileNew patch offsets into
   pointers in place, in structs with 32-bit pointer fields (ALBankFile ->
   ALBank -> ALInstrument -> ALSound -> ALWaveTable/ALEnvelope/ALKeyMap/ADPCM
   book and loop; ALSeqFile). On the host they decode the big-endian file into
   native structs instead (same approach as port/data/map_area.c). Sample data
   (ADPCM bytes) and sequence bytes stay as they are.
3. **aspMain HLE** (port/audio/aspmain.c): run each audio task's command list
   natively when osSpTaskStartGo gets an M_AUDTASK. The list is built by
   native code, so its words are host-order and its addresses host addresses
   (below 4 GiB): no frame image needed. Commands: ADPCM, CLEARBUFF, ENVMIXER,
   LOADBUFF, RESAMPLE, SAVEBUFF, SEGMENT, SETBUFF, SETVOL, DMEMMOVE, LOADADPCM,
   MIXER, INTERLEAVE, POLEF, SETLOOP. Later: check it sample for sample
   against an LLE run (the Quest 64 recomp's RSPRecomp output of the same
   microcode, generated from the user's ROM, never committed).
4. **Output**: osAiSetNextBuffer hands the interleaved 16-bit stereo buffer to
   SDL audio in eltale_rt64 (a queue at the AI frequency the game sets); the
   virtual AI timing stays as it is, so headless runs are unchanged.
5. **Checks**: the US envelope bug from PLAN.md (1 µs releases; PAL values are
   the fix), and a headless hash of each audio frame's output.

Sound, first probe: libultra's audio sources compile natively except for ~12
pointer/integer casts (bnkf.c, cseq.c, env.c, heapinit.c, load.c, resample.c,
reverb.c, save.c), all through `osVirtualToPhysical`-style 32-bit addresses or
the in-place offset patching that the bank/sequence decoder replaces. Build them
from work/src/libultra/audio (so fixes go through patches.py). The audio heap
(alHeapInit at 0x80332DB0, 0x68000 bytes) will need more room natively: its
structs carry 8-byte pointers.

Sound step 1 done: `--audio` (Windows) / `AUDIO=1` (Make) builds the real
game/audio/audio.c and libultra's audio library (from work/src, so fixes go
through the patch) instead of the stand-ins. Both compile and link; the ~16
pointer/integer casts in libultra audio now go through `uptr`, and load.c's
K0_TO_PHYS takes host addresses. It is off by default until it runs: at boot,
Audio_InitSoundPlayer reads the bank alBnkfNew left in its N64 form (step 2).

Sound step 2 done (with `--audio` / `AUDIO=1`): the audio thread runs and
builds a command list every audio frame; the 700-frame New Game run finishes
on Windows and Linux with the same final hash as without audio (6 threads, 416
DMA chunks: the audio DMAs). What it took:
- port/audio/bank.c decodes the bank files and the sequence file header into
  native libultra structs (Port_BankFileNew / Port_SeqFileNew, called from
  audio.c in place of alBnkfNew / alSeqFileNew); audio.c reads the sequence
  count big-endian.
- reverb.c indexed with negated u32 offsets (`&r->input[-d->output]`), which
  wrap on the N64 but run off the end of a 64-bit address space: now s32.
- cseq.c reads the compressed-MIDI header (track offsets, division) big-endian.
- the audio heap is a 1 MiB buffer in the image (structs are bigger natively).
- ALParam slots hold ALStartParamAlt and friends, which are bigger than ALParam
  with 8-byte pointers: ALParam has room added, with static asserts.
Audio tasks still only complete; nothing executes the command lists (step 3).
The VI count per frame changed a little with the audio thread running (frame
540 at VI 1099 instead of 1078); the frame hashes match.
