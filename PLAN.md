# Eltale Monsters PC — Project Plan

Goal: a native PC version of **Eltale Monsters**, fully translated into English. Eltale Monsters is the
Japanese final release of Quest 64, with its extended story and rebalanced magic.

The route is the one that worked for King's Field IV (kfiv-decomp + holyland):
1. a **byte-matching decomp** of the Eltale ROM;
2. a **naming and structure pass** that turns it into workable source code;
3. a **native PC port** built from that source:
   - first on our own stand-in for the N64 hardware;
   - then drawing, sound and the rest rebuilt natively at the game's level.

There's no emulator, no recompiler, and no RT64 / N64ModernRuntime / Rainchus recomp layer. It's ours
from top to bottom. Nobody else is doing this: every other project targets the US Quest 64.

## Status (2026-11-01)

- **Decomp:** `eltale-decomp` rebuilds the ROM byte for byte (874 of 896 functions matching, the rest
  equivalent C) and is named; the port builds from it (`decomp/` submodule plus `patches/`).
- **Port:** the game plays in a window on Windows, with sound, keyboard, mouse and controller, the F1
  menu and quick saves (docs/P1_OS_STATUS.md).
- **Renderer (P2):** our own native renderer is the only one: every kind drawn from the game's data,
  240 pictures a second, widescreen, resolution, MSAA, draw distance, shader warm-up
  (docs/renderer/PLAN.md, R0-R8 done). RT64 is gone; paraLLEl-RDP is kept as the tests' pixel reference.
- **Linux:** builds (`tools/build_linux.sh`) and matches Windows headless; its window is not yet tried.
- **Translation (P3):** the English text and pictures are in.

> Replaces the 2026-10-04 plan (an N64Recomp build on our own runtime). Its research, the translation
> plan and the enhancement list carry over. The recompiler step is dropped: holyland showed that a
> matching decomp, compiled natively, gives a far better base for a renderer and mods.

## Ground rules

- **No ROM content in either repo.** The ROM is user-supplied. Never commit ROM bytes, extracted assets
  or Japanese script dumps. Docs describe formats and offsets, and tools rebuild the content locally.
- **Base ROM:** `Eltale Monsters (J)`, NETJ, F3DEX 1.23, entry 0x80000400, sha1 `50ea6c3d4de4…` (z64
  order). The user's original is .n64/v64; the tools accept every byte order. Local copy:
  `c:\code\quest64\rom\eltalemonsters.jp.z64`.
- **References only:**
  - Quest 64 (US, NETE) and Holy Magic Century (PAL, NETP): the English script, the English font, and
    the US code's differences.
  - The Quest 64 decomp (Rainchus, `c:\code\quest64\decomp`): we read it to understand code, its build
    setup and its libultra. We don't copy its symbols or names. Our names are our own, from Eltale's
    code. Credit it in the README.
- **Naming rules** (from the KF4 naming rounds):
  - behaviour names in `Module_Verb` form, read off the code;
  - an honest `func_` beats a guess;
  - an independent reviewer pass on every naming batch, with the code but not the namers' reports;
  - outside sources (fan wikis) cited where used.
- **Port rules** (holyland's):
  - the game behaves like the N64 by default;
  - every improvement is a switch with a sensible default;
  - artefacts of the original hardware are fixed at the source, modern by default, with a switch back
    to the N64 look;
  - mods are off by default, and the game is byte-identical with them off;
  - game-breaking retail bugs are fixed always on.
- **Two repos, like KF4:**
  - `eltale-decomp`: the matching decomp, pristine;
  - `eltale` (the port): the decomp as a submodule, plus `patches/`, `port/`, `game/`, `tools/` and
    `docs/`.
  - Game code changes in the port only through `tools/patches.py apply/save` against the submodule.
    Keep work files in the decomp's LF line endings, or a hunk becomes a whole-file rewrite.

## Part 1: The matching decomp (`eltale-decomp`)

### D0: Setup (≈1 week)
- **Build:** WSL2 Ubuntu (already set up for kfiv), splat for the N64 ROM, IDO 5.3 (static recomp),
  asm-processor, asm-differ, decomp-permuter, m2c.
  - The Quest 64 decomp builds with **IDO 5.3, `-mips2 -O2 -G 0`**, with a few libc/libultra files at
    other flags (configure.py). Start from the same assumption for Eltale and confirm it on a first
    batch of functions.
- **Splat yaml for Eltale:**
  - segments and overlays;
  - the code start at 0x80000400;
  - data, rodata, bss;
  - the libultra boundaries (find the version; it differs from US).
- **Function boundaries:** from jal targets plus prologue/epilogue scans, cross-checked with our
  function matcher against US (816 US functions: 581 identical, 23 small changes, 98 big changes, 114
  missing).
- **`build.sh`:** rebuilds the ROM and checks it against the retail sha1 ("matches retail"), from
  scratch. `match.py` diffs one function.
- **Progress counter:** matched functions and data. Plus CI on GitHub.
- **Assets** extracted locally by tools: textures, models, the map table at EL ROM 0x577B0 (0x44-byte
  entries), the script at 0xD2E2F0–0xD384B0.

### D1: Code matching
- **libultra first.** It's mostly known code: match it against the right libultra version, so the game
  code's calls are named by the library.
- **Then the game, module by module.** Most US-identical functions need only Eltale's addresses and
  data. The changed modules come last:
  - text and menus: 2F1E0, hud, 27640, 2C110, 328F0;
  - gameplay: 33D0, 15440, 191F0, 1D160, 97B0, 8650 (events).
- **Agent workflows** (at most 10 agents each):
  - disjoint ownership by module, each agent in its own checkout;
  - local commits only; I verify from scratch (`rm -rf asm build && ./build.sh`), then combine.
- **Nonmatching:** a function that won't match after real effort goes to `src/nonmatching/` with its best
  attempt, as KF4's one exception did.
- **Done when** every function is C and the ROM rebuilds byte for byte.

### D2: Data in C
- Data, rodata and bss move into the modules that own them: tables as C arrays, jump tables, and the
  literals each function owns.
- KF4's lesson: a module merge needs that module to own all the rodata between its jump tables.
- Module files merge where the original layout allows it.

### D3: Naming and structure
- **Functions**, then data, then struct fields, in batches with independent review. Evidence first:
  - the code;
  - the game's own text (the script decoder, tools only);
  - the US/PAL code as a reference for what changed.
- **Folders by subsystem**, as kfiv's `src/game/{core,gfx,map,script,battle,magic,ui,...}`, with includes
  by path.
- **Headers per module** with real prototypes. A prototype pass catches the calls that only worked
  through register luck. holyland found many of these; the PC port needs them right.
- **KF4 lessons:**
  - renames can change codegen, so run `match.py` after every rename batch and verify from scratch;
  - `build.sh` may not re-split when only symbols change;
  - lints for readability and symbol rules.
- **Done when** the source reads like a program: few `func_`, named structs, and module docs.

## Part 2: The PC port (`eltale`)

### P0: Native 64-bit build (≈2 weeks)
- **Compile the decomp's C as a 64-bit PC program** (clang on Windows, gcc/clang on Linux), with
  `port/` standing in for libultra and the hardware. holyland's RULES.md carries over:
  - addresses kept in integers;
  - structs read from the ROM or saves keep their N64 layout, while the others may grow;
  - inline asm rewritten in C;
  - calling-convention checks (`port_abi_check.py`).
- **New for the N64: endianness.** The N64 is big-endian and the PC little-endian. Every value read from
  the ROM (models, maps, tables, the script), from saves, or by DMA must be byte-swapped where it's read.
  Find each place in this step, and give it a helper and a census tool, like `port_census.py`.
- **CPU numbers:**
  - the VR4300 FPU's rounding (the game's float mode);
  - integer division by zero and INT_MIN / -1;
  - any libm the game links;
  - checked against an emulator's run where it matters.
- **Milestone:** the game code runs its boot, threads and DMA up to its first display list.

### P1: The hardware stand-in (≈2–4 weeks)
- **`port/os`, libultra's OS replaced:**
  - threads as fibers with priority scheduling (holyland's `fiber.c`);
  - message queues and events, PI DMA from the ROM image, the VI retrace tick, timers;
  - the controller, EEPROM or Controller Pak saves as files.
- **`port/rdp`, the reference renderer:** the game's display lists drawn exactly: F3DEX 1.23 plus the
  RDP.
  - Use **paraLLEl-RDP** (LLE, accurate) as the reference renderer, the way paraLLEl-GS was for KF4.
  - Then write our own Vulkan F3DEX/RDP renderer for upscaling, widescreen and MSAA.
  - Compare the two every frame in a test mode (holyland's `--renderer both`).
- **`port/audio`:** the game's own audio code (libultra's synthesizer, from the decomp) builds the RSP
  command lists. We run them with our own HLE of the aspMain ABI, checked against an LLE run
  (RSPRecomp or an emulator) sample for sample on test songs. Check the US envelope bug (1µs releases);
  the PAL values are the fix.
  - **Done (2026-11-02):** sound is native only. `port/audio/aspmain.c` (aspMain ABI 1 in C) is the only
    play path. The game's own microcode on our RSP interpreter (`port/audio/rsp_lle.c`) is the test
    reference (`ELTALE_AUDIO_CHECK`, `tests/audio_checks.sh`): 0 differing tasks over New Game, the intro,
    the ending, both tours, battles and menus, fuzzed too. See docs/AUDIO.md.
- **Test harness from day one** (holyland's best investment):
  - `--headless --fast` deterministic runs, `--input pad` scripts, and `--log` of game state;
  - boot_test and frame_test, with hashes of display lists and pictures;
  - a reference build for off checks;
  - quick saves (F2) and states (F5) that load from the command line;
  - the F1 ImGui overlay for settings, mods and debug.
- **Milestone:** the game plays start to finish in Japanese, matching an emulator's pictures and sound.

### P2: The native renderer at the game's level (≈4–6 weeks; KF4's "option 4")
Holyland drew the GS's packets first, then rebuilt drawing at the game's level. That's where the big
wins were: real depth, 240 fps from the game's own data, and fixes at the source. Here:
- **Model, map, sky, effect and 2D draws** are made from the game's data structures, with the camera and
  matrices it computes. This replaces the display-list path kind by kind, each kind switchable and
  compared against the reference.
- **Pictures between frames:**
  - every model, the camera and effects moved to the picture's time from the game's own data;
  - our own recordings, replayed on the GPU;
  - a visible fallback (no safety nets that hide costs).
- **Native features:** any resolution, widescreen with proper HUD placement, MSAA, draw distance in place
  of the cone cull, and a pipeline warm-up at start.
- **Tools** (KF4's): a flash recorder, a picture-between checker (`flash_find`), a pace test, a sampling
  profiler, and a screen-effects test that covers each effect.

### P3: Text engine and translation (≈3–6 weeks, overlaps P1/P2)
1. **Decode:**
   - the text encoding: kana and kanji glyph tables, control codes, choices;
   - the message loader at vram 0x8000A0F8 (US func_80008A00), with its 0x4B0 buffer;
   - names in code and data tables.
2. **Dump** the script to UTF-8 with a stable id per message, grouped by scene: about 330 dialogue blocks
   (1,561 lines), plus UI strings, sign and examine text, and names in data tables. Tools only; the dump
   is never committed.
3. **Full translation from the Japanese** (decided 2026-10-31). The English is our own, so English play
   needs only the Eltale ROM. Quest 64's script (US, PAL) and TCRF's ending translation are references
   for meaning and tone, never copied: lines that match Quest 64 are rewritten in our own words.
4. **Translate:**
   - Claude drafts every line from the Japanese;
   - the user rewrites for impact;
   - the translation files (our English, keyed by message id) are committed; Japanese stays in the ROM;
   - columns in the working sheet: `id | jp | literal | draft | final | notes`.
5. **Render** English natively. With the source, the message and menu code is ours to change: a real
   English text engine and font, and editable script files. No ROM font hacks.
6. **Image text:** logos and menu graphics redrawn through a texture-override folder.

### P4: Enhancements and mods (≈2 weeks, then ongoing)
- **Carried from Quest 64 Enhanced:**
  - mouse and keyboard camera (orbit, zoom, floor clamp, outdoor wall stop);
  - the shadow fix, the door fade and border in widescreen;
  - tree and object culling;
  - F2/F4 quick saves, load-last-save.
- **Carried from KF4:**
  - an automap with a browser;
  - extended stats (spirit levels, magic progress, hidden values) in the game's own menus;
  - item and spell descriptions;
  - an ailment timer HUD.
  All are mods, off by default.
- **QoL:** mouse spell aiming, click to interact, mouse menus.

## Rough timeline
Part-time, with agent workflows doing the bulk of matching and naming:

| Step | Estimate | Result |
|---|---|---|
| D0–D2 | 3–6 weeks | ROM rebuilds byte for byte from C |
| D3 | 2–4 weeks | Readable, named source |
| P0–P1 | 4–6 weeks | Plays in Japanese natively, matching an emulator |
| P2 | 4–6 weeks | Native renderer, 240 fps, widescreen |
| P3 | alongside | A complete English playthrough |

Eltale should be quicker than KF4. It's a smaller program (~800 functions against 2,991), its display
lists are simpler than VU1 programs and GS packets, and its N64 tooling (splat, IDO recomp, permuter,
paraLLEl-RDP) is mature.

## Known risks
- **Compiler and flags:** if Eltale's game code isn't IDO 5.3 -O2 throughout, identify the outliers
  early, from the D0 test batch.
- **Endianness:** this is new compared with KF4. Missed swaps show as garbage data, not crashes, so they
  need a census tool and data checks.
- **RSP audio HLE accuracy:** an LLE reference run settles it. (Settled: bit for bit, task by task,
  docs/AUDIO.md.)
- **Eltale-only data:** about 180KB of new files at ROM F2C170–F74890, purpose unknown. Identify them in
  D0/D2.
- **Image text:** Japanese baked into graphics needs redrawn art.

## Known facts to start from
- Same 36 maps as US. Only 738 bytes differ, so there are no new areas.
- Map table: EL ROM 0x577B0 (0x44-byte entries).
- Script: EL ROM 0xD2E2F0–0xD384B0; US 0xD305E0–0xD3BE40.
  - US encoding: a–z = 0x00–0x19, space = 0x7F, capitals from 0x80, 0xFF ends a message, and
    messages start with A0 C0.
- Code against US: see D0 (816 US functions compared).
- Existing analysis scripts (eltale*.py, findfn.py, script.py, datadiff.py) are in the Quest 64 session
  scratchpad. Copy them into `eltale-decomp/tools/` in D0.
- KF4 references for how things were done: `c:\code\kf4-holyland\docs\port\` (PLAN.md, RULES.md,
  RENDERER.md, NATIVE_DRAW*.md, SCREEN_EFFECTS.md, AUTOMAP.md, EXTENDED_STATS.md) and the kfiv-decomp
  repo (build.sh, match.py, the lints, and the naming briefs in `c:\code\kfiv-notes\names\`).

## First session checklist
1. `git init` both repos. Copy the analysis scripts. Set up splat, IDO 5.3 and asm-processor in a WSL
   checkout. Get `build.sh` to "matches retail" with everything as asm.
2. Match libultra, and a first game batch of US-identical functions, to confirm the compiler and flags.
3. Start matching workflows by module (at most 10 agents), each verified from scratch before combining.
