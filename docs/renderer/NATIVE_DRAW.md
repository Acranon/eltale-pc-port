# Native draw (R2, 2026-11-01)

R2 of [PLAN.md](PLAN.md): the native renderer's core, and the first kind drawn with it (map objects).
Read [SURVEY.md](SURVEY.md) for how the game draws, [HOST_LAYER.md](HOST_LAYER.md) for the DLL around
it and [REFERENCE.md](REFERENCE.md) for the reference pictures it is compared with.

## Architecture

```
game (exe)                                         eltale_host.dll
 patched draw sites (work/src, patches/p0.patch)
   | nd_map_draw(obj, pass, Mtx, dl, ...)           port/render/nd_renderer.cpp
   | nd_camera / nd_lights / nd_env_color ...        resources: 4 GPU storage buffers that only grow
   v                                                  (vertices, materials, texels, palettes)
 game/native_draw/                                   recordings: one per tick (state blocks + items)
   native_draw.c  core: switches, tick, state  ---->  render(): per item, per group, one draw
   nd_map.c       the map kind                ElRenderApi (port/include/port/render.h)
   nd_model.c     model loader + caches               shaders/nd.vert, nd.frag (-> nd_shaders.h)
   nd_internal.h  what a kind's file gets
 port/gfx/host_bridge.c: loads the DLL, nd_attach(ElHost_RenderApi()), hooks the tick's end
 port/os/kernel.c: Port_NativeTaskHook (end of tick)   port/p0/host.c: Port_DmaForgetHook (ROM DMA)
```

- **The exe and the DLL stay apart.** The game side calls the renderer only through the function
  table `ElRenderApi` (render.h: plain C, plain data, copied by the callee). Reset restores the exe's
  writable data (the loader's caches go back to empty): `nd_after_reset` tells the DLL to drop its
  resources too.
- **Data in, not display lists.** A model's static list (ROM or a map block) is walked once by
  `nd_model.c` into a mesh: triangles grouped by material, with the decoded textures. Nothing walks a
  list per frame. The top-level frame list is not read at all: the state the game queues there (camera,
  lights, colours, fog) is sent from where it is queued (`nd_camera`, `nd_lights`, ...).
- **Recording per tick.** `nd_frame_begin` (top of `Gfx_BuildFrameDisplayList`) starts one;
  `nd_frame_end` (the graphics task's start, numbered as `P1_GFX_TASK`) finishes it. A recording is the
  camera, state blocks (lights, env/prim/fog/blend colours, fog position) and items in the game's draw
  order (translucent things are in that order: no sorting). The renderer draws the newest recording at
  each present. Items carry a stable id (`kind << 56 | pass << 48 | object address`) and the inputs of
  their matrix (`xform`, `pos`, `rot`, `scale`) besides the matrix itself, for R6: pictures between ticks
  are two recordings and a time, items matched by id.
- **The display-list path is untouched.** Each patched site keeps its code and, when its kind is native,
  rewinds `gfx_dl_head` over what it queued (`ND_DL_BEGIN`/`ND_DL_END`). The game does the same work
  either way (the same matrices, the same `gfx_mtx_index`, the same updates): the map tour with map
  objects native gives the same audio hash and frame count (a2d2e81245d538d4, 30374) and the quick-save
  values are the same. Without the native renderer every kind's commands stay: the frame lists are byte
  for byte the original (tests/p1_checks.sh prints the same lines).

### Switches

| Switch | What |
|---|---|
| `--renderer native` | the window's renderer; native kinds draw through the recording |
| `--native-kinds all\|none\|map,...` / `ELTALE_NATIVE_KINDS` | which kinds the native renderer draws (default all; only done kinds can) |
| `--dump-frames DIR` | headless: the native renderer without a window writes `DIR/frame_N.png` (320x240, 1x, nearest, the N64 look: border, three-point, N64 fog) for the frames `ELTALE_DUMP_FRAMES` (else `ELTALE_DUMP_IMAGE`) names, else every frame. Works with a window too |
| `ELTALE_ND_COMPARE=1` | native kinds keep their display-list commands too (one run gives the reference's frame image and the native picture) |
| `ELTALE_DL_KINDS=map,...` | the frame list keeps only these kinds' commands (dropped after they are queued: the game runs the same): the reference of some kinds alone |
| `ELTALE_ND_LOG=1` / `2` | a line per mesh made (2: and its materials), and anything the loader did not expect |
| `ELTALE_ND_VIEW=1..4` | a test view: the shade, TEXEL0, the shade alpha (fog), the combiner output |
| `ELTALE_ND_DEBUG=1` | per picture: camera, item count, the first item's first vertex in clip space |
| `ELTALE_DL_CENSUS=1` | at exit, `P1_DL_CENSUS`: the display-list commands left in the frame lists, per kind (R5's exit check: 0 with every kind native) |
| `ELTALE_ND_LOG=2` / `3` | also each 2D rectangle with its material (3: and the colour registers as set) |

Kinds (`EL_KIND_*`, render.h): map, billboard, backdrop, actor, shadow, effect, screen (fade,
letterbox), hud, text, menu, picture (title, intro, ending, picture cues). The frame list wraps each
kind's section in `Gfx_BuildFrameDisplayList` (main.c) with `ND_DL_BEGIN/END`, and map.c wraps the map
and billboard sections inside the map functions, so `ELTALE_DL_KINDS` works for every kind already.

## The C API (port/include/port/render.h)

- Resources: `texture_upload` (texels, 32-bit each) -> offset; `mesh_upload` (vertices, groups,
  materials, bounds) -> id; `palette(id, entries)` (raw 16-bit, per frame); `resources_reset`. A full
  buffer answers `~0u`: the loader resets everything and loads again what it draws.
- The tick: `frame_begin(clear colour, mode)`, `camera`, `lights`, `env_color`, `prim_color`,
  `fog_color`, `fog_position(fm, fo)`, `draw(ElRdDraw)`, `frame_end(frame)`.
- Matrices are the N64's (row vectors, `m[row*4+col]`), decoded from the fixed-point Mtx the RSP would
  load: the very numbers it would use. The camera is the projection stack (view x perspective x shake).
- A draw's matrix comes with its inputs for R6 (`xform`, `pos`, `rot`, `scale`, and `inputs[4]` for what a
  kind needs beyond those: `EL_RDX_ACTOR_NODE` names the AnimSet, frame, node and list, `EL_RDI_*`).

## The model loader (game/native_draw/nd_model.c)

`nd_model_get(dl, entry)` -> mesh, cached by (list, entry list). It runs, as F3DEX 1.23 and the RDP do:

- an **entry list** first: a short native list the kind builds once (`nd_map_attach`) with the state
  the frame list has when it calls the model (cycle type, the map's setup list `D_10000E8`, the
  translucent render mode, a scroll texture's load). Colours an entry list sets stand for the draw's;
  colours the model sets are its own (`EL_RDM_OWN_*`). Othermode starts as F3DEX's at a task start;
- segments, DL calls and branches, `CULLDL` (ignored: the GPU clips), `VTX` into the 32-entry cache with
  the geometry mode and `TEXTURE` scale of that moment (lighting, texgen and fog are computed per draw
  from the vertex's normal), `TRI1`/`TRI2` (flat shading copies the first vertex);
- the RDP state per triangle: combiner, othermodes, colours, the 8 tiles and sizes, TMEM as the RDP
  fills it (`LOADBLOCK` with dxt, `LOADTILE`, `LOADTLUT`, odd lines swapped) and reads it;
- each tile a triangle samples is decoded from TMEM over what it can address (its mask, or its clamp
  extent), as RGBA8 or palette indices (CI4: 0..15 of its palette; CI8). Textures are shared by content.
  Palettes are not baked: a material names where its TLUT came from, and each frame the palettes of
  the drawn meshes are read again and sent when they changed (`map_clock_tlut`, the backdrop fade,
  map 29's rotating palette at 0x80124E00 all work this way);
- the mesh remembers every byte it read (lists, vertices, textures); a ROM DMA over any of it drops the
  mesh (`Port_DmaForgetHook`). Native objects (the game's .bss: `Port_N64ToNative`) are read in host order.

Not handled (logged): `TEXRECT` in a model list, `MTX`/`MOVEMEM`/`MOVEWORD` other than segments inside
one (never seen in the census).

## Materials and shaders (port/render/shaders/)

`tools/build_shaders.py` compiles them with glslang into the committed `port/render/nd_shaders.h`
(glslang is at `C:/code/kf4-tools/glslang-build/StandAlone/glslangValidator.exe`, or `--glslang`, or
`$GLSLANG`). One program for everything (an "uber" shader: the material is data), so the pipeline
count is only the GPU state combinations.

| N64 | Native |
|---|---|
| per-vertex lighting: ambient + 3 directional lights | the lights moved into object space per draw on the CPU (the RSP's way: normalized), `max(N.L,0)` sum clamped |
| texgen / texgen linear (LookAt) | same formulas (GLideN64's), in object space; untested on screen (R3: player, spells) |
| fog: `FOG_SHADE_A`, `gSPFogPosition` | the RSP's factor per vertex: `z/|w| * fm + fo` clamped, in the shade alpha (`N64 fog` on, the dumps); per pixel from z/w (off: modern) |
| 1/2-cycle combiner, all inputs | integer `(A-B)*C+D`, the 9-bit clamp; TEXEL0/1 swap in the second cycle |
| blender | the first cycle (fog) in the shader; the last cycle with the picture (`FORCE_BL`, M = memory) is the GPU's blend; without `FORCE_BL` P is written |
| CVG x alpha cut-outs, alpha compare threshold | discard below 32 / below the blend alpha |
| Z: compare, update, decal | reversed-Z D32F (depth = near/w: infinite, exact near plane); decal = the RDP's two-sided test in the pixel stage (`|z - z before| <= dz max`, below); no Z without `G_ZBUFFER`; far plane cut in the pixel stage only for draws whose box crosses it |
| culling | `G_CULL_BACK/FRONT` as the GPU's cull mode |
| textures RGBA16/32, CI4/8 (RGBA16 or IA16 TLUT), IA4/8/16, I4/8 | decoded at load; the palette looked up per pixel |
| tiles: shift, clamp, mirror, mask, odd spans, `SETTILESIZE` | the RDP's arithmetic per sample (S10.5, clamp before mask; mask 0 forces clamp) |
| filtering: point, N64 three-point (bilinear) | same; "N64 texture filtering" off = four-point bilinear |
| texture perspective off | a screen-linear copy of the coordinates |
| the RDP samples a pixel at its corner | the viewport moves half a pixel |
| scissor 304x224 (the border) | the N64 border switch (Classic preset); off by default: the picture fills 320x240 |

Graphics page: the native renderer now wires *N64 border*, *N64 texture filtering* and *N64 fog*.

## Compare (tools/frame_test.py --native)

```
python tools/frame_test.py --rom ROM --saves C:/code/eltale/out/bugsaves --native [KINDS] [--write-baseline]
```

Each run of `tests/render_frames.txt` and `tests/render_frames_actor.txt` is one game with
`ELTALE_ND_COMPARE=1 ELTALE_DL_KINDS=<kinds> --dump-frames`: eltale-rdpref draws the reference of those
kinds alone (`<work>/<run>/png`), the native renderer writes its own (`native`), `render_compare.py`
counts the solid differences against `tests/render_baselines/<set>/<run>.txt` (`diff` pictures). KINDS
defaults to every kind done (set `all`); the sets are in [REFERENCE.md](REFERENCE.md), "Native
baselines". About a minute for all six runs.

**All kinds together (R3 merged, 2026-11-01; `tests/render_baselines/all/`)**:

| run | frames | solid sum | worst |
|---|---|---|---|
| ng | 7 | 692 | 174 (225, the throne room's dither) |
| tour | 18 | 6,973 | 2,653 (24633, map 29's lava near the camera) |
| ending | 9 | 2,102 | 1,676 (9950, the harbour water, near-clipped) |
| battle | 7 | 1,367 | 312 (820) |
| menu | 6 | 298 | 121 (620) |
| chests | 7 | 1,720 | 1,529 (12651, the map's floor near the camera; map alone counts 2,332) |

Against `map,actor` (the two big kinds) every frame is the same within 4 pixels except battle 100
(126 against 60): the player's shadow on the bottom of the player's own cloak. The reference's decal
test passes there (the cloak touches the ground; the RDP's stored dz is the cloak triangle's), the
native one, with the memory's dz estimated from neighbouring depths, does not. Named, left for later.

R2's numbers, map objects alone (47 frames; solid = differing pixels whose 3x3 neighbourhood also
differs, at 5 bits with one step of tolerance; R3's decal test lowered tour to 7,501 and ending to 2,110):

| run | frames | solid sum | worst |
|---|---|---|---|
| ng | 7 | 760 | 190 (the throne room: the reference's dither) |
| tour | 18 | 7,504 | 3,291 (map 29's lava near the camera) |
| ending | 9 | 2,119 | 1,685 (the harbour water, near-clipped) |
| battle | 7 | 1,288 | 299 |
| menu | 6 | 288 | 119 |

What is left in those numbers: triangles the near plane cuts (the RSP's clipper makes new vertices
the GPU doesn't; translucent water and lava near the camera), the RDP's dither in the reference, a
few lighting tones (one or two 5-bit steps on some floors), texture detail at grazing angles. Lessons
from getting there: Granite needs `set_opaque_state()` per render pass (else the colour write mask is
0); the RSP's fog uses |w| (a vertex behind the camera is fully fogged, then clipped); the half-pixel
move was worth 2x; shade interpolated perspective-correct fits the reference better than screen-linear.

## Adding a kind (R3 and on)

1. **A file of its own**: `game/native_draw/nd_<kind>.c` (the build takes every `.c` there). Include
   `nd_internal.h`. Add one row to `nd_kinds_table` in native_draw.c and its attach function to
   nd_internal.h: those are the only shared lines.
2. **Entry lists** in the attach function: a short native list (`Gfx` array, gbi macros) that sets what
   the frame list has set when the game calls the model (look at a dumped frame: `ELTALE_FRAME_IMAGE=1
   ELTALE_DUMP_IMAGE=N`, then a disassembly of the root list). Anything the game sets per draw at the
   top level (a PRIM tint, an env flash, a texture it loads before calling the model) is either the
   draw's state (`nd_prim_color`, `nd_env_color` from where it is queued) or a different entry list (a
   mesh per entry list: keep their number small).
3. **The draw site** (work/src, then `python tools/patches.py save`): keep the code; wrap what it queues
   in `ND_DL_BEGIN(mark)` ... `ND_DL_END(EL_KIND_X, mark)`; then `if (ND_ON(EL_KIND_X)) nd_x_draw(...)`
   with the Mtx it built (`&arg0->mtx[gfx_mtx_index - 1]`), the model list, and the matrix's inputs for
   R6. Never skip the game's own work (matrix pool, counters, updates) when native.
4. **State**: if the kind changes lights/colours/fog at the top level, call the `nd_*` state functions
   right where the game queues them (they cost nothing without the native renderer).
5. **Test**: `frame_test.py --native <kind>` (or `map,<kind>`), look at the diff pictures, then
   `--write-baseline` for the new kind's numbers. Keep tests/p1_checks.sh and `frame_test.py --twice`
   unchanged (the display-list path must stay byte for byte).

Kinds and the files they touch (to run them in parallel with few conflicts):

| Kind | Game files (work/src) | Notes |
|---|---|---|
| backdrop | map.c `Map_DrawBackdrop` | **done** (nd_backdrop.c, "Backdrop" below) |
| actor | actor/anim.c (`Anim_DrawModel`, `Anim_DrawNode`), player.c, npc.c, chest.c, enemy.c, boss.c | **done** (nd_actor.c, "Actors" below) |
| shadow | gfx/shadow.c, gfx/shadow.h | **done** (nd_shadow.c, "Shadows" below) |
| billboard | map.c `MapObj_DrawBillboards`, `MapObj_DrawAnimTexBillboards` | **done** (nd_billboard.c, "Billboards" below) |
| effect, screen (R4) | gfx/sprite_effect.c, gfx/effect.c, gfx/fade.c, sprite callbacks in map/spirit.c and map.c, one hook in actor/anim.c | **done** (nd_effect.c, "Effects" below) |

### Backdrop (R3, nd_backdrop.c)

- **Draw site**: `Map_DrawBackdrop` (map.c) calls `nd_backdrop_draw(backdropId, Mtx, dl_addr, env r g b,
  eye x, eye y - 10, eye z)` after queuing its list; `Gfx_BuildFrameDisplayList` already wraps the call
  in `ND_DL_BEGIN/END(EL_KIND_BACKDROP)`. The item's id is `EL_KIND_BACKDROP << 56 | backdropId`; its
  inputs are `EL_RDX_ROT_RAD` with no rotation, scale 1 and the eye translation (R6 interpolates the
  eye).
- **The list is self-contained**: segment 1's 0x01001300 / 0x01002410 / 0x01003630 (three lists for
  eight backdrops) sets 1-cycle, `TEX0 x ENV`, render mode 0f0a4000, clears Z/cull/fog/lighting
  (SHADE|SMOOTH only), and loads 8 CI8 strips from 0x8007C6B0 + n*0x800 (`map_backdrop_tex`) and the
  TLUT from 0x800806B0 (`map_backdrop_tlut`), then sets 2-cycle again. So the entry list is just the
  frame's 2-cycle. Texture perspective is never set by the game (F3DEX's start value stays): the
  loader's state gives the same picture as the reference.
- **ENV** is the draw's (`D_80084B60..68`): sent with `nd_env_color` just before the item; the
  following `Env_ApplyLightSlot` sends the light slot's again, as in the frame list.
- **Palette fade**: `map_backdrop_tlut` is a native object (host order); the loader reads it at the end
  of each tick and sends it when it changed (`Env_StepPaletteFade`). A new backdrop's texture arrives
  by ROM DMA into `map_backdrop_tex`, which drops the mesh (`nd_forget`).
- **Map 29's rotating buffers** do not feed it: the backdrop lists load only the two addresses above
  (`D_80124E00` and `D_80081AB0` are not referenced by them).
- **Compare** (`--native backdrop`, the backdrop alone): 0 solid pixels in every run (ng 0 frames
  with a backdrop; differ counts tour 984, ending 717, battle 461, menu 347, all the reference's dither
  and single pixels on the cylinder's lower edge). `--native map,backdrop` (the baselines): ng 760, tour
  7,504, ending 2,119, battle 1,285, menu 288, the same as map alone (battle 3 fewer).

map.c is shared by backdrop and billboard (different functions: merges are clean if each keeps to
its own). The loader has no 2D yet: texture rectangles and fill rectangles (R5) need native quads,
not lists.

## Billboards (R3, EL_KIND_BILLBOARD)

`game/native_draw/nd_billboard.c` (+ `nd_billboard.h`, which map.c includes). Draw sites: map.c
`MapObj_DrawBillboards` and `MapObj_DrawAnimTexBillboards`, one call each after the game's own quad
call. Their commands were already wrapped in `ND_DL_BEGIN/END(EL_KIND_BILLBOARD)` by R2, around the
callers' sections. The game still does the culling (1,500 units, the ±fovy cone), the matrices,
`Shadow_Queue` and `model++`. The renderer draws what the game drew.

- **Billboards (kind 0x10: trees, bushes).** The mesh is (`animTexDls[type].b`, an entry list made the
  first time each setup list `animTexDls[type].a` is seen: 2-cycle, `D_10000E8`, render mode c8113078,
  the setup list, bilinear). The entry list names only the setup list's address. A DMA over the area
  block drops the meshes, and the next draw reads the new list. There are 64 entry slots (a map area
  has 4 types).
- **Animated-texture billboards (kinds 0x11/0x12: the candle flames).** There are 16 entry lists,
  made at attach: [kind][flipbook frame] = 2-cycle, `D_10000E8`, render mode 0c1849d8, bilinear,
  `mapobj_animtex_dls[k].a` (setup + TLUT), and the frame's CI8 16x32 `gDPLoadTextureBlock`. The frame
  is `(model >> 1) & 7`, as the game loads it. The flicker is `mapobj_animtex_alpha[model & 3]` in the env
  alpha. The site sends it with `nd_env_color` right where it queues `gDPSetEnvColor`, whether or not
  the kind is native, because it is the frame list's state. So each draw records that tick's own alpha.
- **For R6.** The id is `EL_KIND_BILLBOARD << 56 | pass << 48 | MapObj address` (pass 0 = billboard,
  1 = animated), which is stable over ticks. The xform is `EL_RDX_ROT_RAD` with rot = (0, camera yaw, 0),
  pos and scale. Between ticks, rebuild the matrix with the **interpolated camera yaw**, so the item
  keeps facing the camera. The flipbook frame (a different mesh) and the flicker alpha (the env state)
  **snap**: keep each tick's value, never blend them. The owner checks the flicker by eye.
- **Not here: the map emitters.** `Effect_QueueMapObjEmitter`, `MapObj_EmitArcSprite` and
  `MapObj_EmitDropSprite` only allocate sprites. The sprite pool draws them (`Effect_UpdateAndDrawSprites`,
  whose step callbacks set the env colour), so they are R4's effect kind.

Compare: `frame_test.py --native billboard` (the kind alone against a reference of the kind alone;
`tests/render_baselines/billboard/`):

| run | solid sum | worst |
|---|---|---|
| ng | 0 | 0 (the throne-room flames, frames 225-700) |
| tour | 2 | 2 (634) |
| ending | 101 | 101 (8200: small bushes, far away; minified texture sampling) |
| battle | 2 | 1 |
| menu | 0 | 0 |

With `--native map,billboard` against R2's map baselines: ng 762 (+2), tour 7,497 (-7), ending 2,111 (-8),
battle 1,290 (+2), menu 288. Ticks 693-700 of the throne room (map,billboard) each count 190-195,
the same as the map alone, and the flames' brightness follows the reference's flicker tick for tick.

## Shadows (R3, nd_shadow.c)

`Shadow_DrawAll` (gfx/shadow.c) calls the common list's setup `D_1000270` once (a 16x16 I8 blob,
2-cycle, colour 0 / alpha TEX0 x SHADE, fog then alpha blend, Z compare decal without update, fog colour
black, back faces culled), sets the fog range reversed (`gSPFogPosition(far, near)`: the shade alpha is
1 near the camera and fades out with distance), then per queued shadow (64 max, 2-24 seen) loads a
matrix (`Math_MtxFAlignYToVector(floor normal)`, position, `Math_MtxFScaleAxes(scale)`) and draws the
4-vertex quad `D_1000230`.

- **The mesh**: one, from a native list that calls `D_1000270` and draws the quad, entered with what
  `Env_ApplyLightSlot` (just before) leaves: bilinear, no TLUT. The fog colour is the material's own.
- **State**: the reversed fog range and the black fog colour are queued at the site
  (`nd_fog_position(far, near)`, `nd_fog_color`) whether shadows are native or not: they stay for what
  is drawn after (effects), as in the frame list.
- **Ids**: `Shadow_Queue` has no owner argument, but every caller passes its object's `x` (`obj->x`,
  `p->actor.x`, `s->x`, `player_shadow_pos.x`). shadow.h (P1 only) turns `Shadow_Queue(x, ...)` into
  `Shadow_QueueOwned(&(x), x, ...)`, which keeps the owner beside the queue: the id is
  `EL_KIND_SHADOW << 56 | owner address` (no caller file changed; tools/build_windows.py compiles every file on each build, so a header
  change needs nothing special). R6 inputs: `xform = EL_RDX_ALIGN_Y`, `pos`, `rot`
  = the floor normal, `scale`.
- **Decal** (a core change, commit "the RDP's two-sided decal test"): the RDP passes a decal only where
  `|z - z in memory| <= dz max` (the new and the stored pixel's `|dz/dx| + |dz/dy|`, the larger, rounded
  up to a power of two). GREATER_OR_EQUAL + bias also drew a shadow floating above the ground (the map
  tour's player at y 0 over lower ground; on slopes the part of the quad above the ground). Now the
  renderer copies the depth buffer (`depth_before`) when the first decal comes and again after a depth
  write, and decal groups test against it in nd.frag (`OPT_DECAL`), in N64 screen-z units
  (`Draw.fog.w`: depth is near/w, and the N64's screen z is affine in 1/w). The window is kept one N64
  pixel's slope wide at any picture size. The memory's dz is estimated from the neighbouring depths.

Compare, `--native map,shadow` (`tests/render_baselines/map+shadow/`): ng 764 (worst 191, the throne room's dither), tour 7,491 (worst 3,291, map 29's
lava), ending 2,110, battle 1,291, menu 288. Against map alone the shadows add 1 solid pixel per ng
frame and 3 on battle frame 100; tour frame 30344 (a shadow the RDP hides) is 0. The decal test also
lowered map alone (tour 7,504 -> 7,501, ending 2,119 -> 2,110: map decals).

## Actors (R3, game/native_draw/nd_actor.c)

The player, NPCs, chests, enemies and the field boss: everything `Anim_DrawModel` draws. The walker
stays the game's (anim.c): the actor's `Math_MtxFRotScaleTrans`, the node chain at the TCB frame,
pivots, billboard nodes aimed at the eye, the `dlSeq` flipbooks picked by `Actor.ticks`,
`anim_node_world_positions` (the player's shadow and floor probe read them). Each node it queues
comes to `nd_actor_node` with the Mtx it loaded and the list it called; a list is a mesh once
(`nd_model_get`), so a flipbook is one mesh per list and a swap is just another mesh id.

- **Hooks** (game/native_draw/nd_actor.h, included by anim.h on the PC): `nd_actor_begin`/`nd_actor_end`
  around the node loop of `Anim_DrawModel`, `nd_actor_node` at the end of `Anim_DrawNode`, and
  `nd_actor_filter` where the draw functions set the texture filter (Player_Draw: its setup list,
  Npc_DrawAll: at its start and per NPC, Enemy_DrawAll, Boss_Draw). The model effects (effect.c) walk
  their trees with `Anim_DrawNode` too, without a begin: those calls draw nothing here (R4's).
- **Display lists**: the actor sections of `Gfx_BuildFrameDisplayList` (Player_Draw, Npc_DrawAll,
  Chest_UpdateAll; Enemy_DrawAll, Boss_Draw) are already wrapped in `ND_DL_BEGIN/END(EL_KIND_ACTOR)`
  in main.c (R2): that is where the commands are dropped. Not per node: `Anim_DrawNode` is shared with
  the effects.
- **Entry state, found from frame images** (all 47 reference frames, every top-level model call in the
  actor sections, walked as the RSP would): actor lists set their own combiner, render mode, geometry
  mode, `TEXTURE`, tiles, loads and PRIM (no triangle reads a PRIM, a texel or a combiner its list did
  not set). They take othermode H from outside: 2-cycle, the player's setup list `D_10000A0` (the
  same commands as the map's `D_10000E8`: texture perspective, bilinear, no TLUT, ...) and the filter:
  `0x182C00` (bilinear) or `0x180C00` (an NPC beyond ~141 units of the player, point-sampled). So two
  entry lists. Chests keep the last NPC's filter, as the frame list does. The ENV the second cycle adds
  (`(1-0)*ENV+COMB`: the light slot's additive flash) and the lights are the draw's state:
  `Env_ApplyLightSlot(player_light_slot)` in Player_Draw already calls `nd_env_color`/`nd_lights`.
- **PRIM tints** (`(COMB-0)*PRIM+ENV`, `PRIM*SHADE`) are the lists' own `SETPRIMCOLOR`s
  (`EL_RDM_OWN_PRIM`). **Texgen** (the player's two shiny parts, a boss's armour) is the loader's and
  the shader's: it matches the reference (tour 22780).
- **R6 inputs**: id = `EL_KIND_ACTOR << 56 | node index << 48 | the Actor's address`; `xform =
  EL_RDX_ACTOR_NODE`, `pos`/`rot`/`scale` the actor's (x y z, rotX rot rotZ in radians, scale: a flying
  enemy's raised y as drawn), and `ElRdDraw.inputs` (render.h `EL_RDI_*`): the AnimSet's address (an
  animation change is a new set: snap), `frame | ticks << 16`, `node | AnimNode.flags << 8 (1 billboard)
  | Anim_DrawModel's flags << 16`, and the list drawn (a dlSeq swap: snap).

Compare (`--native actor`, a reference of the actor sections alone; `tests/render_baselines/actor/`):

```
python tools/frame_test.py --rom ROM --saves C:/code/eltale/out/bugsaves --native actor
```

| run | frames | solid sum | worst |
|---|---|---|---|
| ng | 7 | 13 | 4 |
| tour | 18 | 137 | 23 (25287: the player close up) |
| ending | 9 | 0 | 0 (no actors) |
| battle | 7 | 72 | 16 |
| menu | 6 | 17 | 9 |
| chests (render_frames_actor.txt: chests, point-sampled NPCs) | 7 | 88 | 38 (13854) |

What is left is edges and the RDP's dither (the yellow in the diff pictures). With map and actor both
native (`--native map,actor`) the sums are the map's or a little under (ng 682, tour 6986, battle
1298): actors hide map pixels, and the depth test between the two kinds agrees with the reference. The
map tour with every kind native keeps its frame count and audio hash (30374, a2d2e81245d538d4).

## Effects (R4, game/native_draw/nd_effect.c)

`EL_KIND_EFFECT` (the sprite pool, streaks, stat-up markers, terrain rings, model effects: every spell's
visuals) and `EL_KIND_SCREEN` (the letterbox bars and the fade quad), one file. The game keeps all its
updates (the callbacks move the sprites and pick the flipbook frames, the emitters spawn, `Math_Rand`);
each site calls `nd_effect_*` right after it queued a draw, with the Mtx it loaded.

- **Where the state is.** Unlike map and actor lists, effect lists are mostly geometry. The sprite lists
  set their cycle type, render mode, geometry, combiner and `TEXTURE` but take the texture and the TLUT
  mode from the top level (`Effect_LoadSpriteTexture`). Ring segments, streaks, markers and the model
  effects' node lists are geometry only; their state is queued at the top level (`D_800C4780` and an 8x8
  I8 texture, `D_800EB880`, `D_800EB990`, `D_800EB6D0` and `Effect_ConfigureModelEffectTexture`: an I8 or
  CI8 texture and TLUT, the combiner, `TEXTURE`, lighting/texgen; then culling). So each draw gets an entry
  list made of those same commands (the game's macros; for sprites the game's own
  `Effect_LoadSpriteTexture`, run into a scratch list) after the state the section starts with: 2-cycle
  (every effect setup list sets 2-cycle again; only the floating numbers' `D_800C40C0` sets 1-cycle, and
  it comes last), `D_10000E8`, bilinear, and the TLUT mode the last sprite load left (CI sprites: RGBA16;
  model effects end with none). Entry lists are interned by content in a static pool (1,024 of them: they
  must stay below 4 GiB and live as long as the meshes keyed by their address). A battle with every
  element's spells makes about 80 meshes.
- **Colours are the draw's.** Every `gDPSetEnvColor` of an effect also calls `nd_env_color`: the six
  sprite callbacks (effect.c, spirit.c, map.c), streaks, markers, rings, model effects and the fade. So
  the odd/even flicker (`life & 1`, `stateFrames & 1`, `mapobj_arc_flicker_tick`, model flag 2) is that
  tick's value in the recording. Lit model effects (flag 0x20) use the lights
  `Env_ApplyLightSlot(player_light_slot)` sent.
- **Model effects** walk their AnimSet with `Anim_DrawNode` and no actor begin. anim.c calls
  `nd_effect_node` beside `nd_actor_node`; it draws only between `nd_effect_model_begin/end`
  (Effect_UpdateAndDrawModelEffects). The texture scroll is the draw's tile offset (`EL_RDM_TILE_SCROLL`:
  the entry list sets the tile size at 0,0 and the draw adds `tex.s, tex.t`); the flipbook frame
  (`age >> shift`) is the entry list's image. The magic block's lists, vertices and textures are read
  big-endian from RDRAM (only its records are swapped in place).
- **The screen.** A new draw flag, `EL_RDD_SCREEN` (render.h): the matrix goes to clip space without the
  camera. The fade quad is `fade_quad_dl` with the game's own ortho times identity, and an entry list per
  render mode (translucent, or opaque at the ramp's end). The letterbox is two quads in pixels drawn with
  `fade_letterbox_dl`'s own state, full width (the fill rectangles stop one column short, inside the N64
  border). The ending's own fade (mode 5) is part of its picture section, so the native fade draws in modes
  1, 6 and 7 only (R5: call it from the picture kind).
- **Floating numbers** are texture rectangles (2D). `Effect_UpdateAndDrawAll` now drops its effects by
  kind itself and the numbers as `EL_KIND_HUD`; main.c's effect section keeps only the sprites. R5 draws them.
- **Camera shake** was already in the camera (the shake matrix in `nd_camera`'s projection). The additive
  ENV flash on models is the actor kind's (`Env_ApplyLightSlot`).
- **For R6.** Ids: `EL_KIND_EFFECT << 56 | pass << 48 | sub << 40 | slot << 32 | birth tick` (pass 0 sprite,
  1 streak, 2 marker, 3 ring with sub = segment, 4 model node with sub = node). The birth tick counts
  effect ticks (`nd_effect_begin`). A slot's effect starts when its life did not count down by one from the
  last tick (sprites, streaks, markers), at a ring's first growing frame, and at `tick - age` for model
  effects. Inputs (render.h):
  - sprites: `EL_RDX_AIM_EYE` (pos, roll in `rot[2]`, scale x and `more[0]` y; rebuilt with the
    interpolated eye), or `EL_RDX_ALIGN_Y` (flag 1);
  - streaks and ring segments: `EL_RDX_ALIGN_Y_AXES` (direction, pos, scale and `more[0..1]`);
  - markers: `EL_RDX_AIM_EYE`;
  - model nodes: `EL_RDX_EFFECT_NODE` (the root's pos, rot and scales, the AnimSet copy, the frame, node
    and list as for actors).

  Snap, never blend: the flipbook frame (a different entry list, so a different mesh; a sprite's
  `inputs[0]` is its texture), the env colour (the flicker) and a new birth tick. Screen draws are
  `EL_RDX_MATRIX`.
- **ELTALE_ND_EFFECT_LOG=1** prints per tick, on stdout before its `P1_GFX_TASK` line, how many sprites,
  streaks, markers, ring segments, model nodes, fades and letterboxes were drawn natively. That is how the
  test frames were found.

Compare (`frame_test.py --native`, all kinds, `tests/render_baselines/all/`), with the new
`tests/render_frames_effects.txt`. Its spells run has footstep dust, the four elements' level-1 spells,
the enemies' wind, rings and a stat-up marker; its spirit run has the fade in and the spirit's sparkles.

| run | before (R3) | now | notes |
|---|---|---|---|
| ng | 692 | 520 | the fade quad is now in both pictures (225: 174 -> 2) |
| tour | 6,973 | 6,973 | |
| ending | 2,102 | 2,005 | the letterbox |
| battle | 1,367 | 1,506 | worse: the terrain rings (+3 to +89 per frame) |
| menu | 298 | 159 | the fade |
| chests | 1,720 | 1,720 | |
| spells (new) | | 1,726 | worst 1370: 326, 1570: 315, 600: 312; mostly the map's (the sea in the top corner, ~90 a frame, there before) |
| spirit (new) | | 11 | |

The battle frames got worse only along the terrain rings. A ring segment is a quad one or two pixels
tall that samples an 8-texel profile, so sub-pixel differences between the RDP's edge walker and the GPU
change the line's brightness: same place, different intensity (battle 500 has the most, +89). Sprites, the
spell models (texgen, scrolling, flipbooks, CI8 with TLUTs) and the fade agree to a few edge pixels. Not
in any test frame: streaks (melee hits) and the map-object arc and drop sprites (they share the sprite
path). The map tour with every kind native keeps 30374 frames and audio a2d2e81245d538d4.

## 2D (R5, game/native_draw/nd_2d.c)

`EL_KIND_HUD` (meters, element icons and glows, the compass, the spell name and input, the allocation
panel, the enemy panel, markers and the floating numbers), `EL_KIND_TEXT` (text windows: the game's font
and the port's English fonts and pages), `EL_KIND_MENU` (the item menu, the pause menu's pages, the save
menu) and `EL_KIND_PICTURE` (the title with the English strip and logo, the intro, the ending's pages,
its fade and credits, the camera tour's picture cues). With every kind native, **no display-list command
is left in the frame lists** (`ELTALE_DL_CENSUS=1`, below).

### How it works

- **The core, not the sites, knows the RDP.** 2D is texture rectangles, fill rectangles and a few
  triangles. A 2D file includes `nd_2d_gbi.h` last: it redefines the gbi macros the others are built
  from (`gSPSetOtherMode`, `gDPSetColor`, `gDPSetPrimColor`, `gDPSetCombine`, `gSetImage`,
  `gDPSetTile`, `gDPLoadTileGeneric`, `gDPLoadBlock`, `gDPLoadTLUTCmd`, `gSPTexture`, the geometry
  modes, `gDma1p` (display lists and matrices), `gSPTextureRectangle`, `gDPFillRectangle`) with the
  SDK's bodies plus one call into the core with the values the macro was given. So `gDPLoadTextureTile`,
  `gDPLoadTLUT_pal256`, `gDPSetRenderMode` and the rest reach it unchanged, and the queued words are
  the same. Code that writes words itself (matched decomp: `Text_DrawTexturedRect`, a combiner in
  `Hud_DrawSpellInput`, the overlays' `GFX_WORDS` and `GFX_TEXRECT_STEPS`) calls the core next to them
  (`nd_2d_*`, `ND2D_WORDS`, `ND2D_TEXRECT`). The floating numbers in effect.c call it directly.
- **State kept as the RDP holds it**: othermodes, combiner, the colour registers, the 8 tiles, `TEXTURE`,
  the matrices, and **TMEM by reference**: each load remembers which memory bytes went to which TMEM
  bytes (a tile load row by row at the tile's line, rows longer than the line running into the next as
  on the RDP; a block as one run; the TLUT entry by entry). When a rectangle samples a tile it is decoded
  from those bytes over what it can address (its mask or clamp extent) into texels (RGBA8 or palette
  indices) through `nd_model_texture` (shared by content), once per load (cached until TMEM or the tile
  changes). CI palettes are `nd_model_palette_source` (read again each frame) or fixed values.
- **A rectangle is one `ElRd2D`** (render.h v4): its screen rectangle in 320x240 units, s/t at the corner
  and the steps (S10.5, S5.10 as given), a material (`material_2d`: combiner, othermodes, the decoded
  tiles; deduplicated by content) and the colour registers. A FILL-mode fill rectangle includes its
  corners and writes the fill colour (a PRIM-passing material); a 1-cycle one uses the state.
- **Static lists** (the HUD's setup and icon lists in the common block, the text glyph setup, the title's
  and intro's lists) are read once into calls (`Op`) and replayed; a ROM DMA over one drops it. One with
  triangles (the compass disc, the element glows and flashes) becomes a mesh: `nd_model_get(list, entry)`
  with an entry list made from the 2D state at the list's call (othermodes, combiner, geometry, `TEXTURE`,
  the TLUT loads; interned in the image, since the loader takes 32-bit list addresses), drawn with the
  model matrix the code loaded times the 2D projection (the game's ortho times its translate).
- **Lists the code builds itself** (the intro's and the ending's sprite lists) are called with
  `gSPDisplayListBuilt`: their commands reached the core as they were queued, so the call is not followed.
- **Sections.** main.c calls `nd_2d_section(kind)` where each 2D section starts (text, HUD with the
  floating numbers, menu, picture cues; mode 2 menu; modes 3-5 picture). Items are recorded only while
  the section's kind is native; the state is kept whenever the native renderer is on, so a section sees
  what the one before it left, as the RDP does. The ending's fade (mode 5) stays in the picture section:
  fade.c draws it natively when `EL_KIND_PICTURE` is (`ND_FADE_NATIVE`), nd_effect.c's quad.
- **Order**: 2D items go into the same recording as the 3D ones, so they draw in the frame list's order.
  The renderer draws a rectangle from a vertex buffer of the picture's own, no depth, with the texel of
  each pixel's corner (the half-pixel move) plus 1/256 texel, so an exact texel boundary never floors to
  the texel before: text and pictures are pixel-exact at 1x.
- **Recordings wait for their task.** The menu, title, intro and ending loops build the next frame before
  the last one's graphics task starts, so `frame_begin` can come before `frame_end`: the renderer keeps
  built recordings in a queue and `frame_end(n)` numbers the oldest (before this, those modes' pictures
  were one frame late; gameplay is unchanged).

### For R6 and R7

- **Ids**: `kind << 56 | element << 40 | the item's number in it`. `nd_2d_element(element, anchor)` names
  the element (the HUD functions each set one: meters 2, element levels 3, compass 4, spell input 5, spell
  name 6, allocation 7, markers 8-13, status indicators 14, enemy panel 15, popups 16; the floating
  numbers 1).
- **Anchors** (`EL_RD2D_ANCHOR_*` in `ElRd2D.flags`): HUD meters, element levels and spell input LEFT,
  the compass and the enemy panel RIGHT, spell name and allocation CENTRE, markers, status indicators,
  popups and floating numbers WORLD (placed by `Camera_WorldToScreen`), fill rectangles covering the
  width STRETCH, everything else (text, menus, pictures) CENTRE. Each item also gives its screen
  rectangle (a mesh: where its origin lands), and a mesh its model matrix alone (`ElRd2D.model`): the
  compass's rotation for R6.

### The Controller Pak manager

It draws into its own framebuffers with the CPU and never queues a graphics task, so it has no
recording: on a retrace whose framebuffer is not one of the game's, `--renderer native` uploads the
320x240 RGBA5551 pixels as one texture and presents them (R0, host_bridge.c `present`). That is already
the scheme's end state (no display list involved); `tests/host_checks.sh native` checks it
(`P1_NATIVE_CPU_FRAME`: the GPU's copy has the same hashes as `P1_CPU_FRAME`).

### The census (R5's exit check)

`ELTALE_DL_CENSUS=1` counts, at the end of each frame list, the commands still in it per kind and the
rest ("state": segments, the clears, the camera, lights and fog the native kinds send from where they are
queued), and prints `P1_DL_CENSUS` at exit. A kind's commands are dropped where its section ends; when no
kind kept any and the frame is not a compare run, `nd_frame_list_end` drops the frame's own set-up and
state too, so the list is just its FullSync and end. With every kind native (`--renderer native`, or
headless `--dump-frames`), 2026-11-01:

| run | frames | commands left |
|---|---|---|
| New Game (title-new-game.input) | 700 | 0 |
| map tour (ELTALE_MAP_TOUR, 258 sub-areas) | 30,374 | 0 |
| ending + camera tour (ELTALE_START_MODE=5) | 10,000 | 0 |
| battle (battle-stuck.input) / spells (battle-spells.input) | 1,500 / 1,600 | 0 / 0 |
| menus (item-menu.input: item menu, status and map pages) | 650 | 0 |
| save menu (title-continue.input) / intro (no input) | 500 / 1,200 | 0 / 0 |

The map tour natively keeps 30,374 frames and audio a2d2e81245d538d4; New Game audio be0b67b7b718648e.
Without the native renderer nothing changes (tests/p1_checks.sh prints the same lines).

### Compare

`tests/render_frames_2d.txt` (title_en, font1-5, intro and intro_en, save and save_en, compass, panel and
panel_en, menu_en, ending_en: 45 frames) runs after the other lists. The 2D kinds alone
(`--native hud,text,menu,picture`) against a reference of them alone count **0 solid pixels in every
frame** of render_frames.txt and render_frames_2d.txt (the few differing pixels are the RDP's dither in
translucent pictures and the compass's rotated edges). With every kind native (`tests/render_baselines/
all/`, rewritten 2026-11-01):

| run | frames | solid sum | worst |
|---|---|---|---|
| ng | 7 | 499 | 167 (600: the throne room behind the window) |
| tour | 18 | 6,891 | 2,617 (24633, map 29's lava) |
| ending | 9 | 1,987 | 1,676 (9950, the harbour water) |
| battle | 7 | 1,504 | 317 (820) |
| menu | 6 | 125 | 121 (620) |
| chests | 7 | 1,712 | 1,521 (12651) |
| spells / spirit | 11 / 3 | 1,721 / 11 | 326 (1370) / 10 |
| 2D list | 45 | 3,076 | 326 (panel 225: the battle field behind the panel) |

No frame got worse than the R4 baselines; menu (298 -> 125), ng (692 -> 499) and ending fell, as the 2D
now covers 3D differences. What is left is the 3D kinds' (near-plane clipping, dither).

## What is left after R5

- Every kind draws natively under `--renderer native`; the display-list census is 0 (R5's gate). RT64 and
  the frame image are only the fallback and the reference (R8 removes them from play).
- Pictures between ticks (R6): the recording keeps what it needs (ids, inputs); `render` draws one.
- Resolution scale, MSAA, widescreen (R7): the picture and depth images are 320x240; the half-pixel
  move and the 8-pixel border scale with it.
- The pipeline list / warm-up (R7): one program, few pipeline states.
- Near-plane clipping as the RSP does it, the RDP dither (N64 dither switch): the residual differences.
