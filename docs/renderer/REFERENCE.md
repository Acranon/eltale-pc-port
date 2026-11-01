# The pixel reference and the compare tools (R1, 2026-10-31)

An exact, headless picture of any frame the game draws, made the way the N64 makes it, and the tools
the renderer phases (R2 on) use to compare their pictures against it. Plan: [PLAN.md](PLAN.md) R1;
strategy: [HOST.md](HOST.md) section 4.

## The chain

```
eltale-p1 (headless)                 eltale-rdpref (offline, its own Vulkan device)
ELTALE_FRAME_IMAGE=1                 port/rsp: rspboot + F3DEX 1.23 from the ROM   paraLLEl-RDP
ELTALE_DUMP_IMAGE=a,b,c   ------->   over the frame image  --(RDP FIFO)-->  RDRAM --> PNG (+ VI PNG)
out/frame_N.bin (8 MiB + root DL)
```

- **The frame image** (port/gfx/frame_image.c, docs/P1_OS_STATUS.md) is the game's RDRAM as the N64
  would hold it for one graphics task: 8 MiB big-endian, then the root display list's address (4 bytes
  big-endian).
- **port/rsp** (`rsp.h`, `rsp.c`) is the RSP interpreter that used to live in port/audio/rsp_lle.c: the
  scalar unit, COP0 with SP DMA, the vector unit (ares' accumulator, clamp, flag and divide rules). The
  caller gives it DRAM (`dram_read`/`dram_write`, one big-endian byte at a time) and the RDP's DPC
  registers (`dpc_read`/`dpc_write`, COP0 8-15). The audio check (`ELTALE_AUDIO_CHECK=1`) and
  `ELTALE_AUDIO_LLE=1` use it with the game's native memory; eltale-rdpref uses it with the frame image.
- **tools/rdpref/rdpref.cpp** (`eltale-rdpref`): starts the game's graphics task as `osSpTaskLoad` +
  `osSpTaskStartGo` would (the ROM's rspboot in IMEM at 0, the OSTask at DMEM 0xFC0), runs F3DEX to
  its halt, hands every RDP command to paraLLEl-RDP, reads the colour image back and writes PNGs.
- **paraLLEl-RDP** (`third_party/parallel-rdp`, the MIT "standalone" tree
  github.com/Themaister/parallel-rdp-standalone at 388d70f, which pins paraLLEl-RDP 1cecd04 with its own
  Granite subset, volk and Vulkan headers and pre-built shaders). It is bit-exact with Angrylion: the
  rasteriser, TMEM, combiner, blender, dither, coverage and Z are the RDP's own.

The reference never links into the game or the host DLL, and it links nothing from port/render or
port/host: its Granite copy is paraLLEl-RDP's, so it can't clash with the renderer's (KF4's pin).

## Microcode and RDP routing facts (verified)

- The game's task (`Gfx_InitFrameTask`, game/core/main.c): ucode_boot 0x8004B980 (ROM 0x4C580, 0xD0
  bytes), ucode **0x8004BA50** (ROM 0x4C650, 0x1430 bytes of text: the IMEM part and its overlays),
  ucode_data **0x80070260** (ROM 0x70E60, 0x800 bytes), dram_stack 0x80300000 (0x400), yield data
  0x80300400 (0xC00), **output_buff 0x8031C5A0, output_buff_size = 0x803245A0** (an end pointer).
  eltale-rdpref copies rspboot, the text and the data from the user's ROM into the image at those
  addresses on every frame (nothing from the ROM is built in or committed).
- That output buffer means this is the **FIFO build of F3DEX 1.23**: the microcode DMAs its RDP commands
  into the 32 KiB ring at 0x31C5A0..0x3245A0 in RDRAM and moves DPC_START/DPC_END (COP0 8/9). It never
  sets XBUS (DPC_STATUS bit 0): `ELTALE_RDPREF_FIFO=1 eltale-rdpref ...` prints, for the opening scene,
  `dpc_end_writes=2062 xbus=0 rdram=31c5a0..3245a0 words=24798` (the ring wraps about three times a
  gameplay frame; DPC_END is written about once per command).
- The routing: DPC_END takes the words between DPC_CURRENT and DPC_END at once (from RDRAM, or from DMEM
  if XBUS were set; both are wired), so the RDP is never busy: DPC_STATUS reads 0 (no START/END
  pending), DPC_CURRENT = DPC_END, the clocks 0. SP_STATUS reads 0 (no yield). Commands are split by
  the RDP's own lengths (triangles 4-22 double words by their shade/texture/Z bits, texture rectangles
  2) and queued with `CommandProcessor::enqueue_command`.
- The game's frame list never uses BRANCH_Z, but the real microcode makes that moot: what F3DEX does,
  the reference does.
- New in the interpreter for F3DEX: VRCP/VRCPL/VRCPH, VRSQ/VRSQL/VRSQH (ares' tables), LTV/STV,
  LWV/SWV, LFV, the SP_STATUS signal bits. SFV is still missing (fatal if hit). Checked op by op
  against ares (`ares/n64/rsp/interpreter-vpu.cpp`): the compare/select ops, VCH/VCR/VCL, the multiply
  family, the divides and the transposed loads/stores. That found one old bug: **VCL** with differing
  signs and no NE tested `carry` where the 32-bit compare needs `!carry` (ares, and the arithmetic:
  the low halves' 17-bit sum must be 0, or <= 0x10000 with VCE). aspMain never takes that path
  (`ELTALE_AUDIO_CHECK=1` reports no differences before and after; audio hashes unchanged); in the
  reference it moved one near-plane triangle in one test frame (tour 24633).

## Memory and state per frame

- paraLLEl-RDP keeps RDRAM as host-order 32-bit words: the image is swapped word by word into
  `begin_read_rdram()` before the commands, and the picture read back the same way.
- Hidden RDRAM (the 9th bits: coverage) and TMEM are cleared before every frame, so a frame's picture
  doesn't depend on which frames came before it in a batch.
- The colour image is the last SETCIMG of the frame (FB0 0x803B5000 or FB1 0x803DA800, 320 wide,
  RGBA5551). The PNG is 320x240 with each 5-bit channel widened as `c << 3 | c >> 2`. The border
  outside the game's 304x224 scissor is whatever RDRAM held (black in practice).
- The **hash** (`RDPREF ... hash=`) is FNV-1a 64 of the 320x240 big-endian RGBA5551 halfwords of the
  colour image: numbers that later phases can check without pictures.
- `--vi`: the VI scan-out of that colour image with the game's mode (osViModeNtscLan1 with
  `osViSetSpecialFeatures(0x5A)`: gamma and gamma dither off, divot on, dither filter on, AA mode 1;
  the same registers port/gfx/vi.c computes). paraLLEl-RDP gives one 640x240 field; the PNG is
  line-doubled to 640x480. Its hash is `vi_hash` (of the 640x240 RGB8 field).

## Build

```
git submodule update --init third_party/parallel-rdp
python tools/build_rdpref.py          # -> out/rdpref/eltale-rdpref.exe
```

`tools/build_rdpref.py` runs CMake + Ninja on `tools/rdpref/CMakeLists.txt` with the port's MinGW clang
(clang/clang++ 22 from `C:\Users\Sean\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin`, on
PATH), Release, linked `-static` (one exe, no libc++ DLL). It compiles paraLLEl-RDP's sources as listed
in its `config.mk`, plus `port/rsp/rsp.c`. Linux: the same script (needs libvulkan at run time). The game
build (`tools/build_windows.py`, Makefile) now also compiles `port/rsp/rsp.c`.

## Use

```
# dump frames (any run; frame numbers count graphics tasks, as P1_GFX_TASK does)
ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE=100,250,600 eltale-p1 --headless --fast --frames 700 \
    --input tests/title-new-game.input --rom ROM          # -> out/frame_100.bin ... (8 MiB each)
# draw them (one Vulkan start-up for the whole batch; ~0.1 s a frame after the first)
eltale-rdpref --rom ROM --out DIR [--vi] [--dump-rdp] out/frame_*.bin   # or --list FILE
```

Prints `RDPREF frame=frame_N fb=.. rdp_cmds=.. tris=.. rects=.. hash=.. [vi=640x240 vi_hash=..]`.
`--dump-rdp` also writes the RDP command stream (`DIR/frame_N.rdp`, big-endian words) for debugging a
renderer against exact RDP state. No window, no surface: paraLLEl-RDP needs only a Vulkan device (it
picks the first discrete GPU; here the RTX 5080).

### tools/frame_test.py and tests/render_frames.txt

```
python tools/frame_test.py --rom ROM --saves C:/code/eltale/out/bugsaves [--runs ng,tour] \
    [--twice] [--vi] [--write-baseline] [--sheet sheet.png] [--keep-bins]
```

- `tests/render_frames.txt` lists the runs (`name | arguments | frames`): **ng** (title, title menu, fade,
  the window opening, the opening dialogue), **tour** (18 sub-areas of the map tour), **ending** (the
  epilogue pages, the credits over the camera tour), **battle** (battle-stuck save: spells, rings, the
  enemy panel, a floating "Miss!"), **menu** (battle-stuck save with `tests/item-menu.input`: the item
  menu, the pause menu's status and map pages, the page turn) - 47 frames. `@save=battle-stuck` copies
  `<saves>/battle-stuck` (the user's quick save, not committed) into the run's scratch data folder.
- Each run gets its own folder under `out/frame_test/<run>` (its own `ELTALE_DATA_DIR`), up to `--jobs`
  (2) at once. Pictures land in `out/frame_test/<run>/png/`, the rdpref lines in `rdpref.txt`.
- Checks: every picture has more than one colour; `--twice` dumps everything again and requires
  byte-identical frame images and identical picture hashes (it passes: the game, the frame image and
  the reference are deterministic); the hashes against `tests/render_ref.txt`. The frame images are
  deleted at the end unless `--keep-bins`. A full run takes about 15 s (30 s with `--twice`).
- The tour run uses `--frames 300000`, as tests/p1_checks.sh does, so its P1_OS_OK line is the map tour's
  (46b61f5a7d79c61c).

### tools/render_compare.py

```
python tools/render_compare.py REF_DIR TEST_DIR [--out DIFF_DIR] [--name TEST] \
    [--baseline tests/render_baselines/TEST.txt [--write-baseline]] [--vi]
```

- Pictures are matched by name (frame_N.png). Each channel is quantised to 5 bits; a pixel differs if any
  channel is more than 1 step away (the RDP dithers; a modern renderer need not). Only differing pixels
  whose whole 3x3 neighbourhood also differs count ("solid"), so single edge pixels (edge rules,
  coverage) don't.
- Prints `RENDER_COMPARE <test> <frame> differ=<raw> solid=<counted>` and a `RENDER_COMPARE_SUM` with
  the sum and the worst frame. `--out` writes `frame_N.diff.png` (the test picture greyed, solid
  differences red, the rest yellow).
- A **baseline** is numbers only: `frame_N <solid>` lines and `sum <total>`. With `--baseline` the run
  fails (exit 1, `RENDER_COMPARE_BASELINE WORSE`) when any frame or the sum gets worse; `--write-baseline`
  records the current numbers. Keep them in `tests/render_baselines/<set>/<test>.txt` ("Native baselines"
  below). KF4's rule: no frame gets worse without a reason you can name.
- Checked on perturbed copies of the battle pictures: +-1 step noise everywhere counts 0, a wrong
  20x20 block counts 324 (18x18), a 1-pixel shift counts only the solid edge areas.

## Baselines (committed, numbers only)

- `tests/render_frames.txt`: which runs and which frames.
- `tests/render_ref.txt`: the eltale-rdpref picture hash of each of the 47 frames.
- `tests/item-menu.input`: the pad script of the menu run.

## Validation (2026-10-31)

All 47 frames were looked at (contact sheet made with `--sheet`, kept outside the repo). Against what
an emulator (mupen64plus/ares with paraLLEl) shows for this game:
- **Text**: the dialogue window, kana and kanji, the speaker plate, the page-turn quill; the title menu
  and copyright; the epilogue text over the pictures; the credits in the game's Latin font. Sharp, at
  the right places, with the window frame and fade-in.
- **HUD**: HP/MP meters and numbers, the four element gems with levels, the enemy panel in battle,
  "Miss!" floating numbers, the spell input panel.
- **Compass**: disc, needle and the N marker rotating with the camera (the 2D textured triangles under
  the ortho matrix), in every field frame.
- **Fog**: distant geometry fades to the fog colour (fields, the desert, the battle field); fully fogged
  areas show the clear colour.
- **Flame billboards**: the candle flames in the throne room (New Game), camera-facing, with their
  cut-out edges.
- Also: the backdrops (sky cylinders, including the dark storm side of map 2's sky in battle frames
  300/460), the translucent water, terrain rings and wind spell effects, shadows, the letterbox and
  the sepia fade of the ending, the pause menu pages and the item menu.

**Frames that looked wrong, and why:** none is wrong in the reference.
- Two map-tour frames first chosen (map 27/0 at frame 24415 and map 30/0 at 24742) showed only the
  player on a flat fog colour or black. An independent check (each model's CULLDL bounding box
  transformed by its MV matrix and the projection, in Python, straight from the frame image) puts
  every map box outside the view volume in both frames: the tour places the player away from the
  geometry there, so nothing else should draw. They were replaced by 26/2 and 30/5.
- The VCL fix above changed one triangle in tour 24633 (the camera right against the player); the
  committed hash is after the fix.
- Not independently checked: a second emulator's pixels (none is installed here). paraLLEl-RDP is
  bit-exact by its own test suite; what could still be wrong is the RSP interpreter, and its vector
  ops were checked against ares' source as listed above.

## The native pass (R2)

`frame_test.py --native [KINDS]` runs the same list with the native renderer: the reference is drawn
from a frame image holding only those kinds' commands (`ELTALE_DL_KINDS`), the native pictures come from
`--dump-frames`, and `render_compare.py` checks them against a baseline. See
[NATIVE_DRAW.md](NATIVE_DRAW.md).

### Native baselines (R3 on)

`tests/render_baselines/<set>/<run>.txt`, numbers only (`frame_N <solid>`, `sum`), one folder per set
of kinds drawn natively together:

| set | kinds | what it guards |
|---|---|---|
| `all/` | map, billboard, backdrop, actor, shadow (every kind done) | **the main one**: the picture as played |
| `map/` | map objects alone | the loader and materials (R2's numbers, now with the decal test) |
| `billboard/`, `backdrop/`, `actor/` | the kind alone | a kind's own regressions, not hidden by the map (backdrop is 0 everywhere) |
| `map+shadow/` | map and shadows | shadows are decals: alone they have no ground to pass against |

Runs: every run of `tests/render_frames.txt` (47 frames) and `tests/render_frames_actor.txt` (chests:
7 frames of the map tour with chests and point-sampled NPCs), 54 frames per set.

`frame_test.py --native` with no KINDS means `all` (NATIVE_KINDS in frame_test.py: add a kind there
when it is done), runs both lists, and checks `tests/render_baselines/all/`. `--native K1,K2` checks
`tests/render_baselines/K1+K2/` (kinds in the order given: `map,shadow` -> `map+shadow/`); `--list` and
`--baselines` override. A new kind: write its isolated set if it can stand alone, then rewrite `all/`
(`--native --write-baseline`) and name the reason for every frame that got worse.

## What R2 needs from this

- A headless `--dump-frames` (or `ELTALE_DUMP_FRAMES`) in the native renderer that writes
  `frame_N.png` (320x240, nearest, 1x, "N64 look" on, no pictures between) for the same graphics-task
  numbers, so `render_compare.py out/frame_test/<run>/png <native dir>` lines up frame for frame.
  frame_test.py is the place to add a `--native` pass next to the dump.
- The first `tests/render_baselines/<run>.txt` once map objects draw natively (R2's gate).
- `--dump-rdp` streams for exact combiner/othermode/TMEM state when a material looks off.
- If a frame needs an op the interpreter lacks (SFV, VMACQ, VRND*, VMULQ), it stops with
  `RSP: ... not implemented`: add it from ares as the others were.
