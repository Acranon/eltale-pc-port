# The native renderer: plan

Goal: replace RT64 with our own renderer that draws the game from its own data, and switch over
completely, with no N64-style drawing left in play (unlike KF4, which still leans on the GS for some
kinds). RT64 stays only as a fallback while the new renderer grows, and paraLLEl-RDP only as a
pixel reference for tests. Both go once the owner has checked the new renderer in game.

Read first: [SURVEY.md](SURVEY.md) (how the game draws: draw kinds, the feature census, camera and
frame, the HUD) and [HOST.md](HOST.md) (what RT64 does for us today, what comes from KF4, the
technology choice).

## Decisions

- **Stack:** Granite (Vulkan) as in KF4, with SDL3 for the window, input and audio, and ImGui for
  the F1 menu. Everything builds with the MinGW clang we already use.
- **One host DLL:** window, input, audio, settings and the menu live in `port/host/`, and the
  renderer in `port/render/`. Reset restores the exe's writable data, so all of that stays in a DLL.
- **Data in, not display lists:** every 3D draw is one rigid matrix plus a static model display
  list from the ROM. No skinning, and model lists load no matrices. A model loader turns those lists
  into GPU meshes and materials once, cached until a DMA overwrites the block. At draw time the
  game code calls the renderer directly (`nd_*` calls, patched in at each draw site): which mesh,
  which matrix, which colours, which palette. 2D draws (HUD, text, menus) become native quad calls
  at their sites. Nothing walks a display list while the game is playing.
- **Recording each tick:** the game updates state while it draws (animation, effects, camera,
  random numbers), so a frame can't be redrawn. Each game tick is recorded once (draw items with
  stable ids, the camera, the lights, fog, palettes), and the GPU draws from recordings. Pictures
  between ticks interpolate two recordings.
- **Reference:** offline `eltale-rdpref` (paraLLEl-RDP + an LLE RSP running the ROM's own F3DEX)
  renders dumped frame images exactly. `render_compare.py` counts the pixels that differ at 1x,
  against a baseline per test.
- **Modern by default:** fix N64 artefacts at the source and default the fix on, with a switch to
  restore the N64 look where it matters (KF4 rule).
- **Branch:** all work is on `native/render`, with main merged in often. Agents work in worktrees,
  at most 5 at a time. Every phase ends at an owner gate: a play build and a short list of what to
  look at.

## Phases

| Phase | What | Agents | Gate |
|---|---|---|---|
| R0 | Host | 1 | The game runs on the new host with RT64 drawing; sound, keys, F1/F2/F4, reset and settings all work |
| R1 | Reference and tools | 1 (with R0) | The reference matches emulator pictures; compare and dump tools work headless |
| R2 | Core: recording, model loader, materials, first kind (map objects) | 1 lead | Maps draw natively, close to the reference |
| R3 | 3D kinds: backdrop, actors, shadows, billboards | 3-4 | All 3D native; per-kind switch; compare baselines |
| R4 | Effects, fade, letterbox, compass | 2 | Spells and battles look right |
| R5 | 2D: HUD, text and fonts, menus, title, intro, ending, picture cues, Pak manager | 2-3 | Display-list census = 0 in play; RT64 not needed |
| R6 | Pictures between ticks (60/120/240) | 1-2 | Smooth play at 240 with no flashes (flash_find, pace test) |
| R7 | Modern options: resolution, MSAA, widescreen, draw distance, shader warm-up | 2-3 | Owner's settings at 240 |
| R8 | Retire RT64 and the frame image from play; Linux build; docs | 1 | Owner sign-off |

### R0: Host (`port/host/`)
- Move SDL window, fullscreen, input mapping, the F1/F2/F4/F11 keys and notices, the audio ring,
  the data folder and settings I/O out of `port/rt64/` into shared host code (SDL3).
- Split the F1 menu into core pages plus a Graphics page for each renderer. The menu currently
  draws through RT64's Inspector, and its pause relies on RT64's debugger.
- Add `port/gfx/host_bridge.c` and `--renderer rt64|native`. To start, `native` only presents a
  clear and the Pak manager's CPU frame, as one texture.
- Granite brought over from KF4 (its pin, its CMake patch scripts), plus KF4's presenter
  (present_vk.cpp), PNG writer and pacing.

### R1: Reference and tools (in parallel with R0)
- `port/rsp/`: the RSP interpreter from `port/audio/rsp_lle.c`, made general enough to run the
  ROM's F3DEX 1.23 over a frame image. Its output feeds paraLLEl-RDP.
- `tools/rdpref`: frame image in, PNG out. `render_compare.py`, `frame_test.py`, and a headless
  `--dump-frames` for the native renderer.
- Baselines: the New Game, map tour, ending and battle frames.

### R2: Core
- `game/native_draw/` (the adapter called by game patches) and the `render.h` C API.
- **Recording:** draw items keyed by stable ids (MapObj/Actor pointers, effect slot plus birth
  tick), the camera (with shake), 3 directional lights plus ambient, fog, and the run-time palettes
  (`map_clock_tlut`, the backdrop's fade).
- **Model loader:** an F3DEX model list becomes meshes plus material state. This needs CULLDL, the
  list branches, texgen, and the cache invalidated when a DMA overwrites a block.
- **Materials:** the 22 combiners and 18 render modes from the census. That covers 2-cycle with
  fog, CVG×alpha cutouts, decal Z, translucent without Z write in the game's order, and
  wrap/mirror/clamp with masks and odd spans. Textures CI4/CI8 with run-time palettes,
  RGBA16, I4/I8/IA4. 3-point filtering with a switch.
- **Reversed-Z D32F** from day one.
- **First kind:** map objects (static, wave, spline, spinners, translucent, scrolling textures).

### R3: 3D kinds (parallel worktrees, one per kind)
- **Backdrop:** a cylinder kept at the eye, with the day-phase palette fade.
- **Actors:** one shared walker for AnimSet node trees (the player, NPCs, chests, enemies, bosses),
  with billboard nodes and display-list flipbooks.
- **Shadows** (aligned to the floor, decal), **billboards** and **animated-texture billboards**.
- Each kind has its own switch (`--native-kinds`). While a kind is off, RT64 draws it in the same
  frame, until R5.

### R4: Effects
- Sprites, streaks, rings, spell model effects, markers, floating numbers (texgen, scrolling,
  flipbooks).
- Fade quad, letterbox, camera shake, additive env flashes, and the compass's 2D triangles.

### R5: 2D
- Texture rectangles become native quads: HUD, text windows, the English fonts, menus, the title
  (including the English strip and logo), intro, ending, picture cues, and the Pak manager.
- **Exit check:** a census run over New Game, the map tour, the ending, a battle and the menus
  shows that 0 display-list commands reach any renderer. From here RT64 is only a fallback.

### R6: Pictures between ticks
- Gameplay runs at 30 ticks a second (2 retraces). Interpolate:
  - the camera;
  - actor transforms and the animation frame as a fraction;
  - wave, spline and spinner objects;
  - effects;
  - fade alpha, HUD meters and the compass.
- Snap on animation switches and flipbooks.
- KF4 tools: flash recorder, `flash_find --frames`, present simulation, pace test, and a visible
  fallback (no hidden safety nets).

### R7: Modern options
- Resolution scale and MSAA.
- **Widescreen:**
  - aspect in the perspective call and in `Camera_WorldToScreen` x;
  - a wider billboard cone;
  - HUD anchored left, right or centre;
  - fade and letterbox at full width;
  - full-screen pictures pillarboxed.
- **Draw distance:** grid radius, larger object lists, the fog and far plane per map.
- **Shader warm-up:** the pipeline list made at start, as in KF4, so first visits don't freeze.

### R8: Retire
- Remove RT64 and the frame image from the play build (keep the frame image only for the test
  reference).
- Linux `.so` and a Linux build.
- Update PLAN.md, P1_OS_STATUS.md and the README.

## Owner decisions (2026-10-31)
1. **No 8-pixel border.** The game's scissor is 304x224 inside 320x240. Draw to the edge of the
   screen by default.
2. **Flicker matches the current look.** Effects that flicker on odd and even ticks (the candle
   flames) look as they do now, including in pictures between. The owner checks this by eye.
3. **Three presets, as in KF3:**
   - **Classic:** like the N64. 30 fps, 4:3, the border, 3-point filtering, N64 fog and dither.
   - **Classic + visuals:** improved picture only (resolution, MSAA, filtering, draw distance),
     with the game's look and timing kept.
   - **Everything on:** 240 fps, widescreen, the lot. This is the default.

   Every feature is also its own switch in the F1 menu. The presets only set them.
4. **RT64 stays as a fallback until R8.**

The finished renderer should look like today's picture plus the modern extras: widescreen, high
frame rate, MSAA, draw distance and shader warm-up.
