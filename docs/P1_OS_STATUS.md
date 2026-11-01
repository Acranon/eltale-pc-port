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

(Since the native renderer's R0 the window, input, sound and menu are eltale_host.dll's, and RT64's
DLL only draws: docs/renderer/HOST_LAYER.md.)

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

Sound steps 3 and 4 done: port/audio/aspmain.c runs each audio task's aspMain
command list (ABI 1) on the CPU (ADPCM, resampler with the microcode's own
filter table read from its data, envelope mixer, mixer, interleave, pole
filter, buffer moves). With a window, eltale_rt64 plays each AI buffer through
SDL audio. Audio is now on by default (`--no-audio` / `AUDIO=0` for the
stand-ins). Headless: the 700-frame New Game run plays 1429 AI buffers (23.8 s at
22047 Hz, no clipping) with output hash 65d1f0a0a39d3ea5 on Windows and Linux;
`ELTALE_AUDIO_WAV=path` writes the output as a WAV. Not yet: a sample-for-sample
check against the LLE microcode, and the US envelope bug check.

## In-game rendering (2026-10-29)

Gameplay drew nothing but the clear colour on Windows: every matrix the game
built had NaN in it. libultra's `sinf`/`cosf` (gu/sinf.c, cosf.c) keep their
polynomial constants as `{hi, lo}` word pairs, which are wrong doubles on a
little-endian host, so guPerspectiveF and every rotation came out NaN. Linux had
been linking glibc's sinf/cosf instead and was fine. Both builds now leave those
two files out and use the host libm. The opening scene (throne room, Spirit King,
the hero) now draws in the RT64 window.

Also from this hunt:
- `ELTALE_DUMP_IMAGE=<frame>` (with ELTALE_FRAME_IMAGE=1) writes the frame image
  to out/frame_<frame>.bin, and `tools/dl_dump.py` walks it like the RSP and prints
  the display-list tree with matrices decoded and vertex ranges.
- `ELTALE_TRACE_COPIES=1` lists every native object the frame image converts.
- Scene records (SceneEntry, SceneMessage) keep their N64 layout and are swapped
  in place when a scene starts; a scene's NPC table is decoded into native NpcDefs.

## Open: title-screen music (2026-10-29)

On the N64 (ares) the title screen has music; the port's title is silent. Trace
(gdb, Audio_LoadSeq/Audio_StopSeq): the intro loads sequence 27 at about VI 787;
pressing Start ends Core_RunIntroLoop, whose tail (Bgm_Play(-1); Bgm_Update())
stops it, and Core_RunTitleLoop never calls Bgm_Update; the title overlay only
plays cursor SFX. ALSeqPlayer and ALCSPlayer share their layout up to evtq, and
both handle the stop the same way, so by the code the title is silent on hardware
too. To check in ares next: is the title tune the intro's, continuing, or another
song; does it play if Start is pressed before the intro music starts; and after
the intro ends on its own. Record the port with ELTALE_AUDIO_WAV to compare.

## Title screen: not a bug (2026-10-29)

The "title music" is the intro: an attract sequence that starts on a picture
identical to the title screen, with music (sequence 27), then moves on to other
scenes. Pressing Start skips it, which is why the earlier tests heard nothing.
Left alone, the port plays the intro and its music as the N64 does.

## Sound checked against the microcode (2026-10-29)

`port/audio/rsp_lle.c` is an RSP interpreter (scalar unit, COP0 DMA, and the
vector unit with ares' accumulator, clamp and flag rules) that runs the game's
own aspMain, text and data from the user's ROM in RDRAM.
- `ELTALE_AUDIO_CHECK=1` runs every command twice, once on the microcode and
  once on our HLE, starting from the same DMEM and DRAM. It compares the
  buffers (DMEM 0x5C0 onwards; the HLE keeps them at the command lists' own
  offsets) and every DRAM byte the microcode writes, then reports each command
  that differs and a count per command type. The microcode's DRAM writes are
  undone, so the game always hears the HLE.
- `ELTALE_AUDIO_LLE=1` plays the microcode instead of the HLE.

DMA type rules: the microcode sees big-endian memory and the native game keeps
host order. So the DMA converts by what the memory holds: bytes the PI copied
from the ROM (sample data, tracked through `Port_PiDmaHook`) as bytes, the
command list as 32-bit words, everything else as 16-bit samples.

Address rules:
- aspMain takes an address's whole top byte as the segment number. The
  reference therefore gets a copy of the command list with its addresses
  rebased to 24 bits (the audio heap and arenas sit in the image's first
  16 MiB).
- rspboot leaves the task's DMEM address in `$1`.

It found six differences, all fixed. aspmain.c now matches bit for bit over
about 5 million commands: the 4000-frame intro (all of its music) and 2500
frames of New Game.
- **POLEF** filtered in place (in == out for the reverb's filter) while still
  reading inputs it had overwritten. The reverb was fed wrong samples, a
  likely source of the clicks heard in the window.
- **POLEF INIT**:
  - it clears only half of the scratch it takes its history from (DMEM
    0xF90), so it starts from the history of the last state that passed
    through there;
  - it scales the coefficient table's second row in place.
- **MIXER** rounds (vmulf then vmacf: `out*0x7FFF*2 + 0x8000 + in*gain*2`) and
  works in 32-byte blocks.
- **RESAMPLE** rounds each of its four products, then adds them in saturating
  pairs. Its 0x20-byte state passes through the scratch.
- **ENVMIXER** is the microcode's own envelope:
  - 8 lanes of 16.16 volume per side;
  - INIT interpolates from the volume to volume × rate;
  - each block multiplies by the rate;
  - the integer part is held at the target (unsigned min rising, signed max
    falling);
  - gains are vmulf;
  - the state is 0x50 bytes in the microcode's layout.
- **DMEMMOVE** copies 16 bytes at a time.

The HLE also keeps the scratch at DMEM 0xF90 (`dmem[SCRATCH]`) the way the
microcode does, because POLEF INIT reads it stale.

New-Game run audio hash: **be0b67b7b718648e** (was 65d1f0a0a39d3ea5),
identical on Windows and Linux.

Envelope check (PLAN's "US envelope bug"):
- `ELTALE_DUMP_ENVELOPES=1` lists the bank's 29 envelopes. None has Quest 64
  US's 1 µs release on a sounding note.
- Releases are 500 µs on the melodic instruments, 100 µs on one sustained
  instrument, and 1 µs only on percussion that has already decayed to silence.
- That is the retail data, so it is left as is. A gentler release could be a
  modern option later.

Window sound output (eltale_rt64): the old code queued buffers to SDL with no
cushion and cleared the whole queue when it passed 1 s. Both are audible:
- the queue running dry on a frame hitch;
- the jump when it was cleared.

It is now a ring that SDL's callback drains:
- playback starts once about 67 ms is buffered;
- the read rate is steered by up to 0.5% towards that fill, to follow the
  drift between the host clock and the sound card;
- an underrun fades out over a few ms and waits to refill;
- a ring that is far too full drops its oldest samples down to the target.

Also: Port_HostFatal now flushes and `_Exit`s. On Windows, exit() from a game
fiber never returned, so a fatal error hung the process.

## The ending, its camera tour, and layout bugs (2026-10-29)

`ELTALE_START_MODE=n` starts the game in mode n right after boot, so a mode can
be tested without playing to it. Modes 5 and 7 also set `D_8007A172 = 4`, as
the last boss does, which hides the HUD. `tests/p1_checks.sh exe rom` runs the
three headless checks; Windows and Linux print the same lines:
- New Game: mode=1, last_hash 74b8baf63f9c1889, audio be0b67b7b718648e.
- Ending (mode 5, 10000 frames): reaches the tour, mode=7, last_hash
  6b50454cc76480b3.
- Tour (mode 7, 10000 frames): plays all 14 scenes, resets to the intro,
  mode=4, last_hash 02b00a69b6c72d1a.

The ending overlay (`overlays/ending`) is now in the native build: entries in
stubs.c, Ending_Init when mode 5 loads it, func_8020C15C = Ending_Update.
pakmenu (the Controller Pak manager at 0x80100000) is the one overlay left
that is not built natively.

Fixed on the way. Several of these were only visible on one platform, because
the two linkers lay out globals differently, so the cross-platform check is
the tool for them.
- **Cutscene sequence table** (ending tour): it keeps the N64 layout and is
  swapped in place after its DMA. That covers the entries, the camera-track
  KfAnims with their keyframes, and the image cues up to the cue with frame 0
  (`Port_SwapCutsceneTable`, `Port_SwapKfAnim`).
- **Encounter tables** list entries only up to the last sub-area with
  encounters, and other records follow in the block (map 11). The decoder now
  stops at the first entry whose addresses are not in the block.
- **`mapobj_opaque_list` / `mapobj_noanim_list`** hold MapObj pointers but are
  `u16[0x200]` in the matching build. That is 128 native pointers, so they are
  pointer arrays here.
- **`mapobj_spline_out`** is 4 floats, but Math_EvalTcbSpline writes 9. On the
  N64 the rest fell into unnamed bytes (D_80082030). Natively, the Windows
  linker put `mapobj_translucent_count` there, which crashed map 11.
- **Ending_Update** wrote `(&ending_wait_for_sprites)[1]` to reach
  ending_display_buffer_index (a matching trick). On Linux that word is
  ending_cue_group_index. The port writes the variable by name.
- **Backdrop** (sky and far ground):
  - Display lists in the ROM load the texture and palette from fixed N64
    addresses (0x8007C6B0, 0x800806B0) of native globals.
    `Port_N64ToNative` (gen_host_address_map.py) maps an address in the main
    segment's .bss back to its native object, so the frame image converts it
    like any native texture.
  - The DMA'd palettes the game blends as numbers (backdrop phases, clock
    palettes, D_80081AB0) are byte-swapped to host order after their DMA
    (`Port_SwapHalfwords`).
- **Busy waits**: `while (Audio_GetSeqState(0) != 0) {}` (three loops in
  main.c) now calls `Port_OsSpinWait`. The thread waits for the next virtual
  event and keeps the CPU from threads of its priority and below, as a
  spinning N64 thread does. It used to hang the scheduler. The retrace waits
  were already blocking receives.
- **Windows crash reports**: a vectored exception handler prints the
  exception, the faulting address and the unwound stack (fiber stacks defeat
  the unhandled-exception filter). Symbolize with
  `llvm-symbolizer --obj=out/p1-windows/eltale-p1.exe 0x...`.

## Map tour, saving, magic (2026-10-29)

`ELTALE_MAP_TOUR=1` (with no `--input`) starts a New Game and then visits every
sub-area of every map, 90 frames each. Arrival goes through the game's own exit
transition, with entry -1 so the player keeps its position. The run ends after
map 35. The tour drives its own pad:
- Start on the intro and title;
- A while a text window is open, so it never starts a conversation, and in
  battles;
- B in menus;
- in battles, a C button and A in turn, so it casts one-element spells
  (each element in turn) until the MP runs out, then attacks.

`tests/p1_checks.sh exe rom --tour` runs it. Windows and Linux both finish all
258 sub-areas in 56367 frames, with last_hash 50bf22ee177096dd and audio
d04b0bbcb16b02f5. (Since 2026-10-31 the bosses attack at once: 30374 frames, 46b61f5a7d79c61c,
audio a2d2e81245d538d4; see "Battles: enemies that never attacked".) On the way the tour:
- fights the two map 30 bosses, with the player casting spells;
- passes through the opening scene's dialogue.

(`tests/tour-save.input` instead mashes A and B, which talks to the innkeeper
on map 18 and saves; the save/load check uses it.)

Input scripts accept `repeat FROM PERIOD`: after the last row, the rows from
VI FROM onwards play again every PERIOD VIs.

Fixed on the way:
- **Magic block** (ROM 0xD490C0, DMA'd to 0x800C0000 at boot) was raw N64 data
  read through native types. Every spell, the player's included, would have
  misread it. `Port_SwapMagicBlock` (port/data/magic.c) swaps it in place:
  - the records: SpellDef, SpellFxDef and SpellParticleDef rows for 5 groups ×
    15 spells, the MP costs, the launch directions, the visual defs, the
    effect texture animations, and two light animations;
  - the pointer tables stay big-endian and are read through `MAGIC_SPELLS`,
    `MAGIC_FX` and `MAGIC_PARTICLES` (magic.h), and in hud.c through
    `Port_N64Ptr` for the spell names.
- **Saving**: Pak_CompareNameBytes compares n + 1 bytes and calls the names
  equal only when the extra byte differs. On the N64 that byte is stack
  padding after OSPfsState.game_name and happens to differ. Natively it
  matched, so the game never found its own save and tried to allocate it
  again (PFS_ERR_EXIST). The port compares the 16 name bytes. Saving, the
  rescan and the header read-back now work; `ELTALE_TRACE_PAK=1` logs every
  Pak file call.
- **Busy wait**: the menu loop's exit after a rest (`while (bgm_current != -1)
  Bgm_Update();`) calls Port_OsSpinWait too.

Not built natively: pakmenu, the Controller Pak note manager that opens when
Start is held at power-on or the Pak needs repair. It draws into the frame
buffer with the CPU, which the RT64 path does not show; stubs.c returns 0
("nothing to do").

## Loading, and N64-compatible saves (2026-10-29)

- Saves go to the Pak in the N64's format. The 0x68-byte SaveHeader (player
  stats, play time, map and return points, options) is byte-swapped to
  big-endian when written and back when read, in the game's own
  Pak_WriteFile/Pak_ReadFile (`Port_SwapRecord`). The rest of a save is bytes.
  A `--pak` file therefore moves between the port and an emulator or the
  N64 (an emulator's .mpk is the same raw 32 KiB image).
- `ELTALE_LOAD_TEST=1 --pak FILE` loads the first save from the title's
  Continue and prints `P1_LOAD`; its pad picks Continue with stick right, since
  the title's items run left to right. `tests/p1_checks.sh ... --tour` saves at
  the map 18 inn (`tests/tour-save.input` with the tour) and loads it again:
  `P1_LOAD map=18 sub=7 hp=50/50`.

## Controller Pak manager, the menu, reset (2026-10-29)

**Controller Pak manager** (overlays/pakmenu, Start held at power-on): now built and run
(`func_801009EC` calls PakMenu_Run). It draws with the CPU into its own framebuffers
(0x80200000/0x80225800, native u16 stores). Two port changes:
- PakMenu_DrawChar read its 4bpp font as u32 words: on a little-endian host the nibbles came out
  in the wrong order. It reads big-endian words now.
- Showing a CPU-drawn screen: the RT64 bridge treats any framebuffer other than the game's two as
  CPU-drawn. `Port_BuildCpuFrameImage` puts its pixels big-endian into the frame image with a
  display list (at the gfx frame area, unused at boot) that copies them into game framebuffer 0 with
  40 texture rectangles of 6 rows; that is drawn and shown.
- `--pak-manager` holds Start on the first controller read; the menu has "Reset to the Controller
  Pak manager". Headless: `P1_CPU_FRAME n state hash` per screen (ELTALE_DUMP_CPU_FRAMES=1 writes
  out/cpu_frame_N.ppm). tests/pak-manager-delete.input deletes a save: last screen
  `P1_CPU_FRAME n=9 state=1 hash=43842e6ff23bd261` (Windows and Linux), part of p1_checks.sh --tour.

**Saves in a window**: without `--pak`, a window now keeps its Controller Pak in the data folder
(`%APPDATA%\eltale\controller-pak.mpk`, or ELTALE_DATA_DIR). Before this, window play kept saves
in memory only.

**Menu** (since R0 in port/host, eltale_host.dll: docs/renderer/HOST_LAYER.md; before, port/rt64/menu.cpp, in eltale_rt64.dll): F1 or the controller's Back button. ImGui
through RT64's own Inspector class (D3D12 or Vulkan backends) drawn from RT64's present hook. The
game pauses while it is open (ElRt64_Present keeps re-presenting the frame). Pages: Game (resume,
reset, reset to the Pak manager, save folder, quit), Audio (volume, mute in background), Graphics
(fullscreen F11/Alt+Enter, RT64 resolution, MSAA, scaling filter, three-point filtering, frame
rate), Controls (fixed bindings for now). Settings in settings.json in the data folder (RT64's
UserConfiguration JSON under "graphics"). Host pacing no longer races to catch up after a pause
(Port_HostPace rebases when more than 100 ms behind).

**Reset** (Windows): `Port_HostSnapshot` copies the exe's writable PE sections and RDRAM just
before Boot_Main; Reset stops the scheduler, `Port_HostRestore` copies them back, the Pak is
reloaded from its file, and the game boots again. Fiber stacks are fixed slots and the window and
sound live in the DLL, so nothing else needs undoing. Unattended check: ELTALE_HIDDEN_WINDOW=1
ELTALE_MENU_TEST=N (opens the menu at frame N, resets into the Pak manager, quits) with
ELTALE_DATA_DIR pointing at a scratch folder holding settings.json `{"audio":{"volume":0}}`.

## English text (2026-10-31)

- **Script:** `translations/en.v2.tsv`; the build turns it into `text/en.tsv` next to the exe
  (`tools/text/runtime_text.py`: each row keyed by the ROM offset of the Japanese it replaces).
- **Lookup by content:** `port/text/en_text.c` hashes each Japanese string from the ROM; the game's
  text is found by its bytes wherever it was copied to (NPC and sign messages, speaker names, the
  ending, bosses, items, choices, menu strings). Three hooks: `Text_OpenWindow` lays the message
  out for that window, `Text_DrawEncodedString` swaps strings drawn as they are, `Item_BuildMessage`
  builds "Obtained <item>!" from its pieces. Untranslated strings stay Japanese
  (`ELTALE_TEXT_LOG=1` prints them as `P1_TEXT_MISS`).
- **Font:** font 1 (byte 0xC1) is the game's own Latin glyphs (font pages 0-2) spaced by their
  inked width, plus the port's page 30 (byte 0x9E, at RDRAM 0x807F0000, past the 4 MiB the game
  uses) with the glyphs the font lacks (`- * ; % < > ( ) /` and a serifed I), drawn in the same
  stroke / edge / shadow style (`tools/text/enfont.py --header` makes `port/text/en_glyphs.h`).
- **Layout:** word wrap to the window's width (width - 20) and rows ((height - 20) / 13), extra
  pages when a page overflows; the game's own breaking (10-pixel columns) is off for English
  windows (`TEXT_COLS`). Japanese is untouched: the checks' hashes are unchanged.
- **Language:** the F1 menu's Game page (settings.json `game.language`, default English);
  headless runs are Japanese unless `ELTALE_LANG=en`.
- **Tools:** `tools/text/preview.py` draws messages as the game will; `tools/rdp2d.py` draws the
  2D part of a dumped frame (`ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE=<first>+<step>`), which is how
  the English dialogue was checked in the real engine.
- **Fonts:** font byte 0xC1 + n selects set n: 0 the original (above), 1-5 the optional pixel fonts
  (third_party/fonts, CC0) drawn in the game's style by `tools/text/gen_fonts.py` into
  `port/text/en_fonts.h`, pages of 256 x height at RDRAM 0x807F0800+. A text's font travels in its
  bytes, so a change in the menu applies from the next text. Rows per window follow the font's line
  height; the game's own row and column breaking is off for English windows. `ELTALE_FONT=n`.
  `tools/text/fontlab.py` compares fonts in the window. A generated bold original was rejected.
- **Pictures** (`port/text/en_images.c`): the build makes `en_images.inc` from the ROM and `art/en`
  (`tools/images/build_en_images.py`): every picture at its original size, format and palette,
  swapped for the Japanese while the language is English (checked before each graphics task, so the
  menu switch applies at once): in the native image for data compiled from the ROM, in the loaded
  ROM for what the game reads later (the maps), and in RDRAM too for the common block's HUD font.
  The artist's labels are shrunk to size; the pause-menu headings get the original bullets; the
  prologue pages (Quest Square) and warning screens and the epilogue page (the game's font) are
  drawn from `art/en/pages.json`; the world maps have their Japanese names painted over with
  matching map from the same picture and English drawn on (`art/en/maps.json`). The title menu's
  English strip is wider: at RDRAM 0x807F8000, drawn by the title screen itself, each item starting
  where the Japanese one does. The logo (title screen and the prologue's copy) is the artist's
  "Eltale Monsters" at 194x76 with 255 colours of its own (its palette is swapped too), with a
  dark outline and drop shadow added by the build (as Quest 64's logo has) so it reads on the gold.
- **Spell names:** the battle HUD's 8x9 font gets Latin letters (Royalty Mono shapes, the kana's
  colours) and the names come from `spell.*` rows in en.v2.tsv (0x83 marks English: 6-pixel
  advance). The "can't use" label is "Sealed".
- **Controller Pak manager:** its pictures are swapped like the rest; its messages draw English
  through the same fonts (`PakMenu_DrawText`), and a run of spaces snaps the next word to the
  Japanese 5-pixel column grid so Yes / Cancel sit under the cursor. Its rows are 14 pixels apart in
  every English font (the original font's; before 2026-11-01 they followed the font, and in Rune Mono
  or Quest Square the button row landed in the upper window): a message is 7 rows, the text in rows
  0-3, the buttons in row 6. Note names on the Pak (the
  game's own "エルテイル") stay as stored.
- **Windows that grow (2026-11-01):** the windows a string is drawn into as it is were sized for the
  Japanese's 10-pixel glyphs, and much of the English ran past them (the item description's third
  line over the item row, the save menu's "Press the A Button to exit the menu."). In English only
  (Japanese draws the same commands as before):
  - **Item menu** (item.c): the description frame is 272 wide (x 24, centred on the 4:3 screen) and
    as tall as the tallest of the 26 descriptions needs in the current font (measured again when the
    font changes); the description wraps to 263 pixels, the name sits after the "Name" label, and the
    item row (frame, "Item" label, icons, cursor, arrows) moves down by as much as the frame grew
    (27 pixels in the original font: 4 lines).
  - **Save menu** (save_menu.c) dialogs and operation panels: `Port_TextFit` wraps a message line
    wider than 296 pixels and `Port_TextGrowBox` grows the panel about its centre to the text plus
    the Japanese insets (6 around, 5 below), inside the screen less 6. The Yes / No (Repair /
    Reconnect) panel grows to the left, so its rows stay clear of the cursor at x 258.
  - **Title screen** messages (title_draw.c): the box (0x3E, 0x28, 0xC4 x 0x67) widens about the
    centre to the message, wrapped at 296.
  - `tools/text/fit_check.py ROM` measures every English row in all six fonts against the room the
    window it is drawn in gives (the grown windows' limits) and prints FIT_CHECK_OK or the rows that
    do not fit; run it after any change to en.v2.tsv or a font (tests/p1_checks.sh runs it first:
    `english fit: FIT_CHECK_OK`). The rows it once listed:
    translations/README.md, "Fitting the windows".
  - `ELTALE_SAVE_MENU_AT=VI` (port/p0/quicksave.c, headless tests) opens the save menu in the field as
    an innkeeper's "Yes" does; with a quick save and `--pak` holding a save it shows "Overwrite the
    game data?" and its Yes / No panel, and B then "Exit the menu?".

## Quick saves: F2 and F4 (2026-10-31)

F2 takes a quick save, F4 loads it: for playing, and above all for debugging, since a quick save loads
in any later build (`port/p0/quicksave.c`). It is the game's own save, not a memory snapshot:
- **Saving** calls the save menu's own `SaveMenu_CaptureGameState` (into its slot 0 and state block,
  put back after) and keeps the record as `Pak_WriteFile` writes it to the Controller Pak: the
  0x68-byte SaveHeader big-endian (stats with the current HP and MP, play time, day, map and
  sub-area, return points, respawn point, options, the cleared flag) and the 0xC8-byte block of
  chests, spirits, items and bosses. To that it adds what an inn save does not need: the position
  (x, y, z), heading, the time of day (`env_clock`) and the random seed (`math_rand_seed`).
- **Loading** restarts the game's session the way the end of the ending does (`core_game_mode = 0`,
  `core_flags |= 0x8200`: the gameplay loop ends and stops the music, and `Core_MainThread`'s outer
  loop resets the game: bosses, chests, spirits, player, items, clock, `Core_ResetGameState`). There
  `Port_QuickLoadApply` does what the title's Continue does after the save menu's Load:
  `SaveMenu_RestoreGameState`, `Player_SetSpawnFromSaveTable`, `D_8007A172 = 2`,
  `Bgm_ApplyVolumeOption`, gameplay. The spawn point is the saved place with entry -1 (as the
  save table's own spawn points are), so `Map_LoadArea` and `Player_Spawn` put the player there and
  find the floor under it. Then the clock and the seed. The first frame prints where the player is
  against the file (`P1_QUICKLOAD ... (as saved)`).
- **When:** F2 only in the field with the player in control: the game's own test for opening the
  Start menu, and no message, item menu, spell input, level-up screen, inn question, area change,
  scene or dying player. F4 anywhere in play except during an area change (battles, messages and the
  item menu are put away by the session reset, as at the end of the ending), and on the title screen.
  Otherwise the key is refused with a message (on screen, and `P1_QUICK` on stderr). Battles cannot
  be saved: the save holds no enemies or battle state, and the game's own saves are never taken in one.
- **File:** `quicksaves/quick0.elq` in the data folder (`%APPDATA%\eltale` or ELTALE_DATA_DIR; headless
  runs use ELTALE_DATA_DIR or the current folder). "ELTQUICK", format version 1, the PC block's and
  record's sizes, an FNV-1a hash; the PC block (flags, time saved, map, sub-area, x, y, z, heading,
  clock, seed); the record (0x80 + 0xE0 bytes, as on the Pak). Every number little-endian, byte by
  byte, so nothing depends on the build; a reader takes the PC fields it knows, so the block can
  grow. A file that is not a quick save, of another version, cut short or damaged is refused and the
  game goes on. The slot number is in the name for more slots later.
- **Not restored**, as after any Continue: what the session keeps that no save holds (the map is
  entered afresh: NPCs, enemies, effects; the camera starts behind the player). The floor is found
  again from x and z, so on stacked floors it is the one the game picks (y is checked, a different
  one prints `NOT as saved`).
- **Port fix on the way:** `Player_Init` swapped the player's anim sets at once, but some of their
  nodes are in the block each map load puts at 0x80200000. After a power-on that memory is empty;
  after a session restart (a quick load, or the end of the ending) it held the last map's data, and
  the display-list numbers read from it ran the swap of the player's display-list table into his set
  table: the player drew with the wrong parts. `Port_SwapAnimSets` now swaps such sets' headers only;
  the anim functions swap the rest on use, as they already did after a power-on.
- **Tests:** ELTALE_QUICK_SAVE_AT=VI and ELTALE_QUICK_LOAD_AT=VI save or load slot 0 at the first frame
  from that VI where it is allowed (`P1_QUICK save waits: <why>` once). `tests/quicksave-walk.input`
  starts a New Game, reads the opening's messages and walks. p1_checks.sh --tour saves at VI 1500 in
  one process and loads it from the title in another:
  `P1_QUICKSAVE map=13 sub=17 x=-11.11 y=4.00 z=36.40 rot=1.131 hp=50/50 mp=15/15 ...` then
  `P1_QUICKLOAD map=13 sub=17 x=-11.11 y=4.00 z=36.40 rot=1.131 hp=50/50 mp=15/15 ... (as saved)`.
  Loaded in play (the same run, VI 1700) or from the title, the frames after it draw the same
  display lists (but for which of the two framebuffers). A quick save taken right after a load has
  the same record. Loaded during the map tour's map 30 boss battle: back on map 13, as saved.
- **Fixed 2026-11-01: a quick save from the evening crashed a load from the title.** The user's quick
  save (clock 22134) crashed every build, main included and either renderer or headless, when loaded
  before any map had been drawn in the process (F4 on the title, or `ELTALE_QUICK_LOAD_AT=100`): a
  null read in `Map_BlendPaletteByClock`. Its statics start as if palette 1 (the evening's,
  clock 0x4F1A..0x6513) were already blended in, with one step left, so a first call in those hours
  set no blend and read the null `map_clock_blend_from` (and would have divided by a zero length).
  The game never gets there (a New Game, a Continue and the ending's restart all start in palette 0's
  hours); a quick load can. map.c (port only) now takes that palette as it is on such a first call.
  Merely having the file in the data folder does nothing: nothing reads it before F4. The quick save
  needs nothing else in the data folder (no Pak image). Loaded in play after a map had been drawn, the
  same file always worked.
- **Other times and builds:** p1_checks.sh --tour rewrites the walk's quick save with
  `tools/quicksave.py edit` and loads each from the title: `--clock 0x5676` (the evening) and
  `--pc-extra 16` (a later build's longer PC block) load as saved; `--version 2`, `--truncate 300`
  and `--damage` are refused (`P1_QUICK Quick load refused: a quick save of another format` / `the
  quick save is cut short` / `the quick save is damaged`) and the title goes on. A record longer than
  this build's is refused as "a quick save of a later format". `tools/quicksave.py info` prints a
  file's fields.

## The compass and the spirits (2026-10-31)

Two bugs the user found in play, both from a quick save (map 0 sub-area 1, the stone room with the
haystack). Fixed on the way:
- **The 2D projection was all zeros.** `Camera_InitForMap` builds the HUD's orthographic matrix
  into `D_80301000[i].data.ortho`, but camera.c did not include `port/boot_probe.h`, so its
  `D_80301000` was the raw linker label (RDRAM 0x80301000), not `port_gfx_frames`: the matrix went
  to RDRAM the renderer never reads, and the frame image copied the native, zero `ortho`. Texture
  rectangles take no matrix, so the HUD looked whole, but every quad drawn under that projection
  collapsed to nothing: the compass disc (ring, N, coloured dots, turned by `camera_state.yaw`),
  the element icons' max-level glow and gain flash, the element choice flash and the fade quad.
  main.h now maps `D_80301000` to `port_gfx_frames` for every file, and `gen_abs_symbols.py`
  refuses a raw label inside the frames (0x80301000..0x8031C5A0), so this cannot come back.
- **Spirits were at the map's origin.** `spirit_map_placements` points at `SpiritDef` records in
  the map's block (N64 big-endian), and `Spirit_LoadForMap` read their x and z as native floats:
  1165.0 came out as a denormal near 0. The spirit existed and spawned its sparkles (the rising
  bubble) every frame, at the origin, out of sight, and `Spirit_FindInFront` never matched, so it
  could not be taken. It reads them with `Port_N64F32` now (the flag id is a byte). It was the one
  map record type still read raw: chests, NPCs and the area records are decoded, text spots are
  native tables.
- `ELTALE_SPIRIT_LOG=1` prints `P1_SPIRIT` lines: each sub-area's spirits as loaded, the one in front
  of the player, collected flags and element levels (=2 adds the player's position every frame).
  `tests/spirit-haystack.input` with the user's quick save walks into the haystack and takes it:
  `front=1`, `found flag=1`, `levels 1 1 1 1 -> 2 1 1 1`.
- `tools/rdp2d.py` now also draws textured triangles under an orthographic projection (the compass
  disc; texel colours only, no combiner); `--no-hud-tris` turns that off.
- The checks' hashes are unchanged (they hash the display lists, not the matrices they point at).

## Battles: enemies that never attacked, and the name panel (2026-10-31)

The user's bug (quick save on map 2 sub-area 0, out/bugsaves/battle-stuck): in a random battle the enemy's
name panel showed garbage, then the enemy walked to the edge of its circle and kept walking for ever.
- **Walking for ever: AnimSet lengths read before the set was swapped.** An enemy that starts to approach
  sets its timer from its walk anim's length (`arg2[1].length`, Enemy_StateIdle) before any anim function has
  used, and so swapped, that set: 220 frames read as -9216, -9216 / 10 = -921, and a timer counted down from
  there never reaches 0. The same raw read set the hurt, attack-end and boss timers (a boss's 54-frame walk
  became 13824 frames, its hurt pause 8960) and the wandering NPCs' walks (30 frames became 7680: they walked
  off in one direction for minutes). It was the one AnimSet field game code reads itself. `ANIM_SET_LENGTH`
  (game/actor/anim.h) is now every such read (enemy.c, npc.c, player.c): `Port_AnimSetLength` swaps the set
  first, as its first use would, and reads through the bitmap.
- **Garbage name: monster names were never English.** The panel draws the name in the battle HUD's own
  font, in its kana codes; in English that font's kana are Latin letters (the spell names), so the Japanese
  name came out as letters. Japanese was right. The 75 names (six area sets, eight bosses) are
  `monster.<ROM offset>` rows in en.v2.tsv (runtime rows 0x3000000 + offset; translations/README.md),
  matched by their bytes like the script (`Port_TextMonster`) and drawn like the spell names
  (`Port_HudMonsterName`).
- **Fixed on the way: a crash walking in the field.** `Sfx_StopChannel4` (the map's ambient loop leaving
  earshot) used `D_800559E4`/`D_800559F8`, aliases of `sfx_channel_sound[4]`/`sfx_channel_timer[4]` in
  undefined_syms.txt, which the host link gave their raw RDRAM addresses: it read garbage and indexed the
  sound ids with it. audio.c maps them to the arrays; `gen_abs_symbols.py` now refuses any raw label that
  undefined_syms.txt defines as an offset into a named object (these two were the only ones).
- Checked on the way: EnemyDef and EnemyAttack swap shapes match the ROM records field by field (area 0's
  attacks are all spells, so their hit reach is 0 by design); the name tables are read big-endian.
- `ELTALE_BATTLE_LOG=1` prints `P1_BATTLE` lines: the start (centre, radius, each enemy's stats, walk length
  and name bytes), every enemy state change and, every 20 frames, positions with the distances to the centre
  and the player, and the player's HP. `=2` also starts an encounter after the next 50 units walked.
  `tests/battle-stuck.input` with the user's quick save: before, `state=0->1 timer=-921` and walking to the
  end of the run; now `state=0->1 timer=22`, `1->5` (a spell), `player hp 50 -> 49`, turn after turn.
  Frame 225 (`ELTALE_DUMP_IMAGE`, tools/rdp2d.py) shows the panel: the kana name in Japanese, Were-Rabbit in
  English.
- **Checks:** New Game, ending, camera tour, save/load, Pak manager and quick saves unchanged. The map tour
  changes: 30374 frames, last_hash 46b61f5a7d79c61c, audio a2d2e81245d538d4 (was 56367, 50bf22ee177096dd,
  d04b0bbcb16b02f5). The tour's only battles are three boss fights (it arrives by teleport, so no random
  ones); with the right lengths the map 30 boss (Guilty) walks in after 54 frames and hits the level-0 player
  at once, where before it stood hurt for 8960 frames and walked for 13824 (VI 53599 to 100931). With only
  `Port_AnimSetLength` reading raw again, the build gives the old three values exactly, so nothing else
  changed them. Linux not rerun (its Makefile is stale).

## Keyboard, mouse and the mouse camera (2026-11-01)

Full keyboard play with Quest 64 Enhanced's keys, every binding (keyboard, mouse buttons, controller)
changeable on the F1 menu's Controls page and kept in settings.json, and Quest 64 Enhanced's mouse camera
(right-drag or Tab to turn, wheel to zoom, middle click to reset; movement follows the view; floor and
outdoor wall stops; billboards not culled by direction under it). See docs/CONTROLS.md. Input scripts take
`mouse VI DX DY WHEEL FLAGS` rows; `ELTALE_INPUT_TEST` drives the host's SDL path in a hidden window;
`ELTALE_MOUSE_LOG=1` prints `P1_MOUSE_CAMERA`. With no mouse input every check is unchanged (New Game
74b8baf63f9c1889, map tour 46b61f5a7d79c61c / audio a2d2e81245d538d4, quick save and load as before).

