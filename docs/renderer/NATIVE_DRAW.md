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
  writable data (the loader's caches go back to empty): `nd_after_reset` tells the DLL (`reset`, render.h
  v9) to drop its resources and its recordings too (see "Resets and full buffers").
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
| `ELTALE_WIDESCREEN=off\|16:9\|window\|N` | R7a: the picture's shape (overrides the Graphics page; headless dumps are 4:3 without it); see "Widescreen" |
| `ELTALE_DL_CENSUS=1` | at exit, `P1_DL_CENSUS`: the display-list commands left in the frame lists, per kind (R5's exit check: 0 with every kind native) |
| `ELTALE_ND_SMALL_BUFFERS=n` | a test: the vertex, material and texel buffers hold n KiB each, so they fill in play (tools/lifetime_test.py uses 512); `P1_ND_LIFETIME` at exit |
| `ELTALE_ND_LOG=2` / `3` | also each 2D rectangle with its material (3: and the colour registers as set) |

Kinds (`EL_KIND_*`, render.h): map, billboard, backdrop, actor, shadow, effect, screen (fade,
letterbox), hud, text, menu, picture (title, intro, ending, picture cues). The frame list wraps each
kind's section in `Gfx_BuildFrameDisplayList` (main.c) with `ND_DL_BEGIN/END`, and map.c wraps the map
and billboard sections inside the map functions, so `ELTALE_DL_KINDS` works for every kind already.

## The C API (port/include/port/render.h)

- Resources: `texture_upload` (texels, 32-bit each) -> offset; `mesh_upload` (vertices, groups,
  materials, bounds) -> id; `palette(id, entries)` (raw 16-bit, per frame); `resources_reset`. A full
  buffer answers `~0u`: the loader drops its caches and loads again what it draws, and the renderer keeps
  what its recordings still name (see "Resets and full buffers"). `reset` (v9): the game's Reset.
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
- **Anchors** (`EL_RD2D_ANCHOR_*` in `ElRd2D.flags`): HUD meters and element levels LEFT,
  the compass and the enemy panel RIGHT, spell input, spell name and allocation CENTRE, markers, status indicators,
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

`tests/render_frames_panels.txt` (2026-11-01) adds the battle's spell input panel (spellin: 8 frames, 2,674 solid,
the field behind it as in panel 225) and the spirit's element allocation panel (alloc: 8 frames, 5 solid);
baselines in `all/` and `wide/`, reference hashes in `tests/render_ref.txt`.

## Pictures between ticks (R6, port/render/nd_interp.cpp)

Gameplay runs at 30 ticks a second (a recording every 2 retraces); the menus, the title, the intro and the
ending at up to 60. With the Graphics page's **frame rate** other than *Original*, the window shows pictures
at the fixed rate (60-240) or the display's, each made from the two recordings around its time. The game
does not change at all: everything happens in the DLL, from recordings the game already made, while the
game's thread waits for its next event. Original (the Classic presets) is the game's timing exactly as
before: the newest recording at each retrace.

### Time and pacing (render_native.cpp, nd_renderer.cpp `NativeDraw::picture`)

- **When a recording is due.** Each recording is stamped with its time: the retraces presented when it was
  finished plus the game's virtual clock's phase in that retrace (render.h v5 `frame_phase`, from
  `osGetTime()` at `nd_frame_end`; a gameplay tick finishes ~0.3-0.5 of a retrace after its retrace). It is
  due `BETWEEN_DELAY` = 2.25 retraces later: one tick (2 retraces) so that the pair a picture needs is
  always there, and a quarter for the real time the game's thread takes to build it. That is ~1.4 retraces
  more latency than Original, the price of never guessing ahead. A picture at time v (retraces, with the
  fraction since the last) shows the pair (a, b) with a due <= v < b due, at t = (v - a due) / (b due - a due);
  past the newest's due time (the game late with its next tick) it shows the newest as it is.
- **A present queue with due times** (KF4's): pictures are due on a grid at the rate; the retrace hook shows
  one when one is due, and `ElHost_Wait(ns)` (called from `Port_HostPace` in port/p0/host.c just before the game
  thread sleeps until its next event) shows those that come due meanwhile, with a high-resolution timer to
  half a millisecond before and a yield loop after. A picture more than one interval late is left out
  (counted), never shown late after a newer one. Game ticks stay on the VI schedule.
- **Vsync modes** (present_vk.cpp): Original with vsync is FIFO; pictures between with vsync use mailbox (the
  newest picture at each refresh, never a wait that would hold the game's thread), FIFO if there is none;
  vsync off (and hidden windows) immediate.
- **The visible fallback.** When a picture between can't be made it shows the recording a as it is, and
  counts why: `early` (nothing due yet: the start, a reset), `gap` (the pair is more than 4 retraces apart:
  a load, a pause), `mode` (the game mode changed), `camera` (one has no camera), `cut` (a camera cut,
  below). The F1 Graphics page shows the pictures a second, how many were left out late, the counts per
  reason, and the plan and build times; `P1_INTERP` prints them at exit.

### What interpolates and what snaps

Interpolated (matched in b by stable id; the k-th item of an id in a with the k-th in b):

| What | How |
|---|---|
| camera | eye (from the view matrix, so the height oscillation is in it) and the point looked at, lerped; `guLookAtReflectF` and `guPerspectiveF` (fovy, near, far lerped) rebuilt; the LookAt (texgen) from the rebuilt view |
| shake | **snaps**: a's shake until b's own picture (it is a per-tick jolt: lerping it would smear it) |
| map objects: wave, spline, spinner, translucent, scrolling | the root rebuilt from the inputs (`guPosition` degrees, `Math_MtxFRotScaleTrans` radians), angles the short way round |
| billboards | the root with the yaw turned by the interpolated camera's yaw (a's yaw + the camera's turn since a) |
| actors (each node) | root = `Math_MtxFRotScaleTrans(actor rot, scale, pos)` from the lerped inputs; the node chain (the node's matrix with the root taken out) by polar decomposition: rotation slerped, stretch and translation lerped |
| effects | sprites and markers: `Math_MtxFAimAt(pos -> the interpolated eye)` with the roll and scales as the local part; streaks, ring segments and aligned sprites: `Math_MtxFAlignYToVector(lerped direction)` + pos; model effects as actors (their root per effect flag 4) |
| shadows | `Math_MtxFAlignYToVector(floor normal)` + pos, the scale as the local part |
| fade quad | its env colour lerped (the ramp) |
| effect colours that ramp | an effect's env colour going one way over the last three ticks (a sprite fading in or out) is lerped |
| HUD (meters, element levels, markers, floating numbers, spell name) and pictures | rectangles lerped when they keep their material and move at most 24 pixels; 2D meshes (the compass, the element glows) by their model matrix's decomposition |
| full-screen fills (a picture's fade) | colours lerped |

Every matrix uses the correction form `M(t) = lerp(Ma, Mb, t) + R(t) - lerp(R(0), R(1), t)` with R the rebuild
(`local(t) * root(t)`): t = 0 gives a's matrix and t = 1 b's, bit for bit, whatever the rebuild's rounding, and
between them the rebuild's path (an arc where an actor turns, the slerped pose).

**The animation frame.** The node matrices between are the slerp of the two ticks' node chains, not the
game's TCB spline evaluated at a fractional frame: calling the game's math (anim.c) with a fractional frame
would mean running game code per picture from the DLL against state that has moved on. At 30 ticks a second
the frame advances by one key-time step per tick, and over one tick the spline is close to a rotation at
constant speed, which the slerp is. Loops (the last frame to the first) slerp across: their poses meet.

Snapped (a's as it is until b's own picture, never blended):

- meshes: animation switches (a new AnimSet: the chain keeps a's pose while the root still moves), dlSeq
  flipbooks, effect and display-list flipbooks, the candle flames' frames;
- colours: the flame and effect flicker (the state's colours are a's: what the owner sees today), a colour
  that goes back and forth;
- births (items only in b appear at b's picture) and deaths (items only in a stay until b's picture);
- teleports: a root or a matrix that jumps more than 200 units in a tick, a change of xform, a matrix that
  cannot be decomposed;
- camera cuts: the eye or the point looked at moving more than 40 units in a tick, the view turning more than
  ~26 degrees, the eye's distance changing by a fifth, the field of view by 15 degrees: the whole picture is
  a's (counted as `cut`). (Walking moves the eye 1-5 units a tick; a battle's camera cut 80.)
- text windows and menus (`EL_KIND_TEXT`, `EL_KIND_MENU`), and 2D items that change material or jump;
- anything without a match in the other recording.

Known: run-time palettes (`map_clock_tlut`, the backdrop's fade, map 29's rotating palette) are resources, not
part of a recording: a picture between uses the newest palettes (a few pixels in the dumps; see the tests).

### Tools and tests

| Tool | What |
|---|---|
| `ELTALE_DUMP_BETWEEN=N` (with `--dump-frames`) | headless: for each tick dumped, also the N - 1 pictures between it and the tick before, `frame_<a>_<ppp>.png` (ppp = t x 1000) |
| `ELTALE_INTERP_ENDS=1` | also `ends_<a>_0.png` (the interpolator at t = 0) and `P1_INTERP_ENDS frame= differ_at_0= differ_at_1=`: the matrices and camera that differ bit for bit at t = 0 from a's and at t = 1 from b's |
| `ELTALE_DUMP_FRAMES` | now also ranges `A-B` and runs `F+S/R` (R ticks every S from F) |
| `ELTALE_INTERP_LOG=1` | a line per plan with the camera's moves (`P1_INTERP_CAMERA`: for the cut thresholds) |
| tools/interp_ends.py | `ends_<a>_0.png` against `frame_<a>.png` |
| tools/flash_find.py | KF4's: each picture between against its two ticks and their blend; off where it is more than 24 levels from the blend and from every pixel within 6 of its place in both ticks (a shard, a hole, a flash; a thin thing that moves is near its place in both), 3x3 solid, flagged at 0.2% of the picture; contact sheet `flashes.png`. Reads F8 folders too |
| tools/interp_test.py | the suite: New Game, a walk that turns (tests/interp-walk.input), the throne-room flames, a battle with spells (battle-stuck save), the map tour sampled, the ending sampled: ends + interp_ends + flash_find, and the run's `P1_OS_OK`/audio |
| tools/interp_strip.py | a PNG grid or GIF of a range of ticks with their pictures between (the owner's strips) |
| tools/pace_test.py | real time in a hidden window at given rates: pictures a second, interval spread and worst, retraces a second (60 = the game's speed), the picture kinds; `--sim` (`ELTALE_PRESENT_SIM=1`) the schedule alone |
| `ELTALE_PRESENT_LOG=file` | `P wall v kind` per picture shown, `V wall vi` per retrace |
| F8 (or `ELTALE_FLASH_AT=N`: on from the start, written at the Nth picture, a test) | the flash recorder: off until the first F8 (since nr/present: its 64 images are made then, and only then is each picture copied into them), then F8 writes the last 64 pictures shown (kept on the GPU) to `<data folder>/flash_<vi>_<date>/` with `pictures.txt` (retrace and phase); `flash_find.py --neighbours` reads it |

### Results (2026-11-01)

`python tools/interp_test.py --rom ROM` (4 pictures a tick, ELTALE_INTERP_ENDS):

| case | ticks dumped | pictures between | ends (bit for bit) | interp_ends | flash_find |
|---|---|---|---|---|---|
| ng (New Game 200-700: the title, the fade, the throne room) | 500 | 1,500 | 0 / 0 | 0 of 499 differ | 0 flagged |
| walk (tests/interp-walk.input 600-900: a walk that turns) | 301 | 900 | 0 / 0 | 0 of 300 | 0 |
| flames (the throne room's candles 225-330) | 106 | 315 | 0 / 0 | 0 of 105 | 0 |
| spells (battle-stuck, battle-spells.input, 40 of every 100 ticks) | 625 | 1,872 | 0 / 0 | 0 of 610 | 0 |
| tour (the map tour, 8 of every 1,000 ticks) | 238 | 630 | 0 / 0 | 0 of 209 | 0 |
| ending (12 of every 500 ticks) | 240 | 660 | 0 / 0 | 0 of 220 | 0 |

Every run keeps the game's own frame count and audio (New Game be0b67b7b718648e, the map tour 30374 frames
a2d2e81245d538d4): the pictures between are the DLL's alone. flash_find was checked on injected faults (a 12x30
and an 18x18 patch in two pictures between: both flagged); before the camera-cut rule, the spells run's battle
camera cut (eye 80 units, distance 106 -> 70 in one tick) was flagged in 15 places, and before the palettes per
recording map 29's lava (its palette turns every tick) differed by ~10,000 pixels at t = 0.

**Cost** (headless, `P1_INTERP`): planning a pair (matching, decompositions, the ends) 27-83 us on average
(worst 280 us, a battle with spells: ~150 items); building a picture between (the moving items' matrices,
the camera) 1.6-5.6 us on average (worst ~120 us, a first touch of the caches). Drawing it costs what a tick's
picture costs.

**Pacing** (`python tools/pace_test.py`, hidden window, tests/interp-walk.input, the second half: play):

| rate | pictures a second | interval (spread, worst) | retraces a second | between / newest / as is |
|---|---|---|---|---|
| 240 | 240.0 | 4.17 ms (0.11, 6.63) | 60.00 | 5,945 / 215 / 0 |
| 144 | 144.0 | 6.94 ms (0.10, 8.91) | 60.00 | 3,560 / 136 / 0 |
| 120 | 120.0 | 8.33 ms (0.07, 9.26) | 60.00 | 2,992 / 88 / 0 |
| 60 | 60.0 | 16.67 ms (0.19, 17.80) | 60.00 | 1,540 / 0 / 0 |
| Original | 60.0 | 16.67 ms (0.01, 16.99) | 60.00 | (the newest at each retrace) |

(One 60 Hz run had a 2.5 s stall of the whole process, the game's retraces too: the computer, not the pacing;
run again it was even.) The "newest" pictures are where the game's next tick was not there yet: with a delay
of exactly 2 retraces that was a quarter of them (a tick is finished 0.3-0.5 of a retrace after its retrace); with the recordings
timed by the virtual clock and 2.25 retraces, 3.5%, in runs of 2-3 pictures where the game itself took a
retrace longer for a tick (as Original shows it too).

## Resets and full buffers (2026-11-01)

A recording names resources by number: a mesh id (3D items and 2D meshes), a material index (2D rectangles), and
through them vertex ranges and texel offsets. The recordings live longer than a tick (the last three in `hist`,
those waiting for their graphics task in `pending`, the one being made in `building`; R6's pictures between draw
the older ones), so the resources they name must too. Two things used to end them early, with nothing checking:

- **The game's Reset** (F1 menu). `nd_after_reset` cleared the meshes but not the recordings: the next pictures
  drew the pre-reset recordings against emptied tables (out-of-range ids into cleared vectors; it looked right
  only because `clear()` keeps the memory). In the window the play scene went on for ~10 pictures after the
  reset; any upload meanwhile would have drawn other meshes' vertices in it.
- **A full buffer mid-tick** (`nd_model.c` `nd_model_get`, `nd_2d.c` `rect_item`). The loader reset the
  renderer and loaded again, but the items already recorded that tick (and the ticks in `hist`) kept their ids,
  which the meshes loaded next were then given.

### The design

**Reset is a cut** (`reset`, render.h v9; `nd_model_reset_all`). Everything goes: the resources, `hist`,
`pending`, `building`, the interpolator's plan, the palette history (the palette buffer is put to the newest's
first). One black recording with no camera goes in, so the window shows black (the N64's screen at a reset)
rather than its own clear colour until the game's first recording; that one is shown as it is, no picture
between across the reset (the pair's planning sees no camera). The recordings after a reset are numbered from
the next 10000 (`frame_10001` ...: the game counts from 1 again; the dumps and `P1_` lines keep the two apart),
and `P1_ND_RESET vi= frame_base=` is printed.

**A full buffer keeps what the recordings name** (`resources_reset` -> `NativeDraw::keep_recorded`). The loader
still drops its caches and loads again what it draws (`nd_model_reset`), but the renderer does not forget: it
marks every mesh and 2D material any recording names (`hist`, `pending`, and `building` when a tick is being
made), the materials of those meshes and the texture uploads their tiles sample (`texture_upload` now lists each
upload's range), moves them all down to the buffers' start in their order (texels, materials with their tile
offsets moved, vertices, groups, meshes: a move never overwrites what is yet to move) and renumbers them, in the
recordings too. Everything else is dropped. So every item recorded before the full buffer, this tick's too, draws
exactly as it would have, and nothing outlives the last recording that names it.

Then **what was kept is found by content**: the loader's loading again what it draws gives the same textures,
materials and meshes, and `texture_upload`, `material_2d` and `mesh_upload` answer those with the kept ones (a
hash, then a byte compare) instead of storing them twice. Without that the kept set and its copy filled the
buffer again at once (in the walk with 512 KiB buffers: ~14,000 resets in 900 ticks; with it, 4), and a mesh in
use keeps its id across the full buffer, so the pictures between match it as before. The palette ids stay over a
full buffer (they are not in the buffers, and the kept materials name them) unless they are what ran out (4096:
never seen); then they go too, and items recorded before it could show a reused palette for that tick (known,
not fixed).

The other choice, a generation in each id with stale items dropped, would lose the items already recorded that
tick (a picture with holes) and the older recordings' pictures between, and the loader cannot draw them again.

**Checks in render().** Each item's state, 2D item, mesh, the mesh's groups, their materials and vertex ranges
and a rectangle's material are checked against what is there (`item_ok`); an item that fails is skipped and
counted (`stale`). A debug build (no NDEBUG) asserts. `P1_ND_LIFETIME resets= flushes= kept_meshes=
kept_materials= kept_texels= stale=` is printed at exit when any of these happened (or with
`ELTALE_ND_SMALL_BUFFERS`); stale is 0 in every test.

### Tests: tools/lifetime_test.py

| case | what | result (2026-11-01) |
|---|---|---|
| reset | a hidden window (`ELTALE_HIDDEN_WINDOW`, sound off, pictures between at a fixed 120), tests/title-new-game.input into play, `ELTALE_MENU_TEST=900:reset:900`: the F1 menu's plain Reset (new in menu.cpp; the input script starts again, so the game goes back into play the same way), the flash recorder 24 pictures after it, Quit 900 later. Dumps ticks 250-460 and 10001-10060, 10250-10460 with 4 pictures a tick | 1 reset, 0 stale; 255 ticks dumped after it (252 with items); ends 0/0 (448 pairs), interp_ends 0 of 446, flash_find 0 of 1,341 pictures between; every tick after the reset and its pictures between the same picture as the same tick before it (780 of 780); the recorder's 64 pictures (11 before the reset, 53 after: black, then the logo) flash_find --neighbours 0 flagged, none of them the scene before the reset |
| full ng, walk, tour | headless, each run twice: full size and `ELTALE_ND_SMALL_BUFFERS=512` (512 KiB a buffer) | ng 2 flushes, walk 4, the map tour 3,923; 0 stale; P1_OS_OK the same; every frame and picture between the same picture as at full size (ng 2,504, walk 1,505, tour 1,198); ends 0/0, interp_ends and flash_find clean |

Before the fix the reset case fails: no tick is drawn after the reset under its own number, and the recorder shows
the play scene for 10 pictures after the reset (a mean difference of 0.2 levels from the last picture before it;
black and the logo are ~50 off). tests/host_checks.sh runs it with the native renderer.

## Widescreen (R7a, 2026-11-01)

The Graphics page's **Widescreen** switch: *Off (4:3)*, *16:9* or *Match the window* (the window's shape,
from 4:3 to 32:9). Everything on sets 16:9; Classic and Classic + visuals set Off. settings.json keeps it as
`"widescreen": "off" | "16:9" | "window"` (a file from before R7 with `true`/`false` reads as 16:9/off).
`ELTALE_WIDESCREEN=off|16:9|window|<number>` overrides it (tests; headless "window" is 16:9). With 16:9 the
window starts 16:9 (1706x960, smaller if the display's usable area is). RT64 stays 4:3 (the switch is
greyed there).

### The design: the picture is wider, the game's units are not

- **The picture** is 240 units high and Vw = 240 x aspect wide; at 1x its width is an even number of
  pixels (16:9: 426x240), so the 4:3 middle starts on a whole pixel and 2D stays pixel-exact.
  `NativeDraw::picture_size_1x` gives the size, `aspect_of_1x` the shape the game is told (426/240 = 1.775 for
  16:9: the perspective matches the picture exactly). R7b's resolution scale multiplies that size (16:9 at
  4x: 1704x960); the renderer works from the picture's own size (`render()`: `vw = 240 x width / height`, the
  border and the 4:3 middle in N64 pixels times `scale` = height / 240), the depth, the decal's depth copy and
  the MSAA colour follow it (`ensure_targets`), and the presenter fits the picture's own shape (`fit`).
- **The game asks** `ElRenderApi.aspect()` (render.h v7, after R7b's `draw_distance()`) at each tick's start (`nd_frame_begin`, kept in
  `nd_aspect`; 1.3333334f, the game's own constant, whenever the native renderer is off). Three places use
  it (patches/p0.patch, all `port (R7)`):
  - `Camera_Update`: `guPerspectiveF(..., ND_ASPECT(), ...)` instead of 1.3333334f. The vertical field of
    view is kept, so the middle 320 columns of a wide picture are the 4:3 picture.
  - `MapObj_DrawBillboards`: the cone `a < camera_fovy` becomes `a < nd_billboard_cone(camera_fovy)`:
    fovy plus what the wider picture adds at each side (atan(tan(fovy/2) x aspect) - atan(tan(fovy/2) x
    4/3); fovy 35 at 16:9: 41.4 degrees), so billboards don't pop at the new edges. Exactly fovy at 4:3.
  - `Hud_DrawClippedTextureRect`: the HUD's clip columns (0..320) reach the picture's sides for WORLD
    elements (markers, status indicators, popups, the out-of-range icon; `nd_2d_clip_x`), and a WORLD
    texture rectangle's x is read as signed (`nd_2d_texrect`: left of the 4:3 screen is x < 0, which the
    RDP's 12 bits would wrap). LEFT/RIGHT/CENTRE elements keep the 4:3 clip (the HP panel still slides in
    from beyond the left edge).
  - `Camera_Update`'s shake: a translate in clip space, so its x is scaled by (4/3) / aspect to move the
    picture as many pixels as at 4:3 (without it a battle hit shook the 3D 2 pixels further).
- **The spell input rows are CENTRE** (R5 had them LEFT): they sit around the element allocation cross in
  the middle, and LEFT pulled them away from it.
- **Camera_WorldToScreen needs no change.** Its x is 160 - 120 x/w in a 320-wide screen's units, and on
  a picture Vw wide the same point is at Vw/2 - 120 x/w: the same value moved by (Vw - 320) / 2, which is
  what the WORLD anchor does. Its y (which game logic reads: `hud_npc_marker_screen_y` picks the text
  window) is untouched by any aspect.
- **2D is placed by its anchor** (render.h `EL_RD2D_ANCHOR_*`, set by the sites since R5), in the
  renderer only, so pictures between ticks need nothing new (2D items are lerped in 320-wide units and
  placed when drawn): CENTRE and WORLD move by (Vw - 320) / 2, LEFT stays, RIGHT moves by Vw - 320,
  STRETCH (fills covering the width) keeps its left edge and moves its right edge by Vw - 320. A 2D mesh
  (the compass, the element glows) gets the same move as a column change on its clip-space matrix.
  CENTRE items are cut to the 4:3 middle (scissor): text windows and menus are centred and unstretched,
  and full-screen pictures (the title, the intro, the ending's pages, picture cues, the save menu) are
  pillarboxed, with nothing a 4:3 screen hides (a sprite sliding in) showing at the sides.
- **Full width by themselves**: the 3D, the fade quad and the letterbox (`EL_RDD_SCREEN` draws go
  straight to clip space, which is the whole picture), and STRETCH fills.
- **The Controller Pak manager** (320x240, drawn by the CPU) is shown in a 4:3 picture, pillarboxed by
  the presenter.
- **Pictures between ticks**: the camera's aspect is in each recording (`ElRdCamera.aspect`, was a
  constant 4/3) and the interpolator rebuilds the perspective with it; 2D is placed at draw time. A
  picture made just after the switch, from a recording of the old shape, is drawn into the new picture
  (one tick slightly stretched).
- Headless dumps (`--dump-frames`) take ELTALE_WIDESCREEN; a wide dump has no border (the 4:3 dumps keep
  the N64 look with it).

### The 4:3 assumptions left as they are

- The 2D layouts are a 320x240 screen's: text windows, menus, the title, intro and ending pages and the
  picture cues stay 4:3 in the middle (pillarbox); the HUD's corners move out to the edges.
- The Controller Pak manager (CPU-drawn 320x240): pillarboxed.
- The game's 304x224 scissor (the 8-pixel border): the owner's decision is no border; with the border
  switch on, the border is at the wide picture's edges.
- Other culls are not a 4:3 frustum: the map's grid search is a disc around the eye, NPCs a distance from
  the player, `Effect_QueueMapObjEmitter` a half-plane (+-90 degrees: fine to 180), the RSP's CULLDL boxes
  are ignored natively (the GPU clips), the far plane and fog are the map's (R7b's draw distance).
- The backdrop is a whole cylinder at the eye: nothing to widen.
- RT64 (the fallback until R8) and the paraLLEl-RDP reference stay 4:3.
- The display-list path keeps the game's frame list; with widescreen its perspective is the wide one, so
  `ELTALE_ND_COMPARE` pictures of the reference are only meaningful at 4:3.

### Tests (2026-11-01)

- `tools/frame_test.py --rom ROM --wide [--twice] [--write-baseline] [--sheet X.png]`: every list at 16:9
  (no reference: the native pictures only), each picture's middle against the 4:3 native picture of the
  same frame from a `--native` run (`render_compare.py --centre`, inside the border). Baselines
  `tests/render_baselines/wide/`. The game is the same at 16:9 (the audio of every run equals the 4:3
  run's: New Game be0b67b7b718648e, the map tour a2d2e81245d538d4, the battle 9fbe0f649e30e541).
- Results: 0 solid pixels in the middle of every picture without the HUD's corners (New Game, the ending
  and its camera tour, the title, the fonts, the intro, the save menu); the gameplay runs differ by the
  HUD's LEFT and RIGHT items only, which moved to the edges (checked on the diff pictures; tour 46,019 over
  18 frames, battle 27,363 over 7, spells 25,754 over 11, the panel 11,445, the compass 8,026). `--twice`:
  the same pictures.
- The 4:3 checks are as before R7a: `--native` (the R5 numbers: ng 499, tour 6,891, ending 1,987, battle
  1,504, menu 125, chests 1,712, spells 1,721, spirit 11, the 2D list as before), the reference `--twice`
  (every hash in tests/render_ref.txt), tests/p1_checks.sh --tour and tests/host_checks.sh native (the same
  lines with ELTALE_WIDESCREEN=off and with the default 16:9; the window starts 1706x960).
- `ELTALE_WIDESCREEN=16:9 python tools/interp_test.py`: ng, walk, flames, tour and ending OK (ends 0/0, interp_ends
  0 differ, flash_find 0 flagged), the game's audio as at 4:3. Spells: ends and interp_ends clean; flash_find flagged
  one picture (frame_581_750, 0.22% against the 0.20% limit) until the R7 merge: a smoke puff of the spell moving
  fast at the new left edge, smooth in the pictures between and brighter at tick 582 (an effect colour that snaps,
  as designed), checked by eye. The cause is flash_find's reach (6 pixels a tick: how far a thing may move and
  still count as near its place): a perspective picture stretches toward its sides (1 + tan^2 of the angle off the
  axis), and a 16:9 picture's sides are further off the axis than a 4:3 one's, so the same motion is more pixels
  there; the puff moved 7-8. flash_find now scales its default reach by the stretch at the picture's side over a
  4:3 picture's (`edge_reach`, fovy 35: 6 at 4:3, 7 at 16:9, 12 at 32:9), and the picture's 225 off pixels
  drop to 125 (0.12%): 0 flagged. (Scaling the area threshold by the width was not the fix: 0.2% of a 426-wide
  picture is already more pixels than of a 320-wide one, and the shares are of the picture either way.) 4:3
  results are unchanged (reach 6).

### R7a with R7b (merged 2026-11-01)

- One picture size: `picture_size_1x(aspect)` (an even width) times the resolution scale; `ensure_picture`
  (render_native.cpp) asks `wanted_aspect` and the scale together, so a change of either makes the picture
  again, and the game is told the shape for its next ticks. *Match the window* scales by the window's height
  over 240 (16:9 window 1706x960: 1704x960 at 4x). Depth, the decal's depth copy and the MSAA colour follow the
  wide scaled picture (`ensure_targets`).
- 2D at a scale and wide: the anchors move by whole N64 pixels (`side` = (Vw - 320) / 2 is whole because the 1x
  width is even), so OPT_RECT's edges, rounded up to N64 pixels before the move, stay on the picture's N64 grid
  (the shader's crisp sampling takes `gl_FragCoord % scale`): checked on the 16:9 4x MSAA 4x dumps, every
  anchor's text and panels are whole 4x4 blocks. The CENTRE pillarbox scissor and the border are N64 pixels
  times the scale.
- The Controller Pak manager keeps its own 320x240 image (R7b), shown pillarboxed by `fit()` in a wide window.
- Headless dumps: `ELTALE_WIDESCREEN` times `ELTALE_DUMP_SCALE` (with `ELTALE_DUMP_MSAA`, `ELTALE_DUMP_LOOK`,
  `ELTALE_DRAW_DISTANCE`); a wide dump has no border.
- Pacing at 240 with 16:9, 4x (1704x960), MSAA 4x, far (`pace_test.py --hz 240 --scale 4 --msaa 4 --distance
  far --widescreen 16:9`): 240.0 pictures a second, interval 4.17 ms (spread 0.07, worst 5.21), 60.00 retraces a
  second. `ELTALE_OPTIONS_CYCLE=120` (MSAA, resolution, distance and widescreen changed in play, 101 changes
  over 1,500 ticks): P1_OS_OK, `P1_PIPELINES made_in_play=0`; the shipped pipeline list needed no change (a wider
  picture changes no render pass format or sample count).

## What is left after R5

- Every kind draws natively under `--renderer native`; the display-list census is 0 (R5's gate). RT64 and
  the frame image are only the fallback and the reference (R8 removes them from play).
- Pictures between ticks: R6, above.
- Resolution scale, MSAA, draw distance, warm-up, N64 dither: R7b, "Modern options" below. Widescreen: R7a.
- Near-plane clipping as the RSP does it: the residual differences.

## Modern options (R7b, 2026-11-01)

Every option is its own Graphics-page switch, live; the presets only set them (HOST_LAYER.md, the table).
At the original settings nothing changes: the headless dumps are the N64 look at 1x as before (the native
frame test's 113 pictures are the same bytes as before R7b; `p1_checks`, `host_checks` and the game's audio
and frame counts are unchanged).

### Resolution scale (render_native.cpp, nd_renderer.cpp)

- The game's picture is its 1x size (`picture_size_1x`: 320x240 at 4:3, 426x240 at 16:9, R7a's "Widescreen")
  times the scale: Original 1x, a multiple 1-8, or *Match the window*: the window's height over 240 rounded up
  (at most 9; the height, so a wide window is not counted twice), so the presenter only scales down. Depth, the decal's depth
  copy and the MSAA colour follow the picture's size and samples (`ensure_targets`). CPU-drawn screens (the
  Controller Pak manager) keep a 320x240 image of their own. The flash recorder keeps its pictures at most at
  2x (scaled down).
- **Placement.** The viewport moves half a pixel of the picture (not of the N64): each picture pixel is
  sampled at its own top-left corner, so 3D and 2D keep their N64 places relative to each other, and a
  full-screen picture covers the whole image. At 1x this is the old half-pixel move exactly.
- **2D stays crisp** (`OPT_RECT`): a 2D rectangle in a bigger picture has its edges on the N64 pixels the
  1x picture covers (a fractional edge rounds up), and each pixel samples its N64 pixel's corner, so text,
  HUD and pictures are the 1x texels, each a whole block of pixels, without the filter blending in a keyed-out
  neighbour (the magenta fringes the first try showed). 2D meshes (the compass) are drawn at full resolution.
- The decal test's tolerance and the dither are in N64 pixels (`picture_scale()`: the depth's height / 240).
- `ELTALE_RES_SCALE=n` overrides (tests); headless `ELTALE_DUMP_SCALE=n`.

### MSAA

- 2x, 4x, 8x (as the GPU can: `msaa_supported`): drawn into a multisampled colour and depth, resolved into the
  picture by the render pass (the resolve attachment; a decal's depth copy ends the pass and the next one
  loads and resolves again).
- **Cut-outs** (CVG x alpha, the alpha compare) are shaded at every sample (Granite's sample shading, 3D
  only): the discard per sample gives the trees, fences and grass smooth edges, as KF4's cut-out AA.
- **Decals** read the multisampled depth copy (its first sample: `nd_ms.frag`, the same shader built with
  `ND_MSAA`); the shadows and map decals look as at 1x.
- Granite's render pass hash left out the sample counts (2x, 4x and 8x were one pass, the first made): the
  Granite patch (`cmake/granite_pipeline_warm.cmake`) now hashes them.
- `ELTALE_MSAA=n` overrides; headless `ELTALE_DUMP_MSAA=n`.

### Draw distance (nd_map.c, nd.vert)

The game's view ends where its grid search ends (SURVEY.md 6: a disc of 7 or 5 cells of 100 units on the fields,
2 of 600 on map 9; pattern 0 maps draw the whole sub-area), with its fog nearly opaque there (fog 750-1000 per
mille of the depth range: at the disc's edge 82-95% fog) and the far plane at 1000-2400. Far (2x) and Farther
(3x) move all three by k:

- **Fog and far plane** (the renderer): the perspective's z/w is A - C/w (A = -m[2][2]), so the fog and the far
  plane as if every vertex were k times nearer use z' = A w + k (z - A w) (nd.vert; `tile_offset.zw` = A, k).
  The fog keeps its colour and shape, k times as far; the backdrop is untouched.
- **More objects, drawing only** (`nd_map_far_*`, nd_map.c; calls in map.c): the cells of a disc k times as
  wide that the game's search left out, read as they are: static objects (spline-animated ones at their
  current frame), translucent and scrolling-texture objects (first, they are the farthest), and billboards past
  the game's 1,500 units out to 1,500k (no shadow, no cone: the GPU clips). The game's own collection updates
  what it collects (waves, spline steps, emitters with `Math_Rand`, ambient sounds, spinners), so the game's
  search, lists (256 each) and matrix pool are not widened: wave objects, emitters, spinners and flames keep the
  game's distance. Ids are the game's, so an object passing into the game's disc stays one item for the
  pictures between. The map tour at 2x keeps its 30,374 frames and audio a2d2e81245d538d4; it adds 30-100
  items a tick on the fields (map 27: 104 -> 207).
- render.h v6 `draw_distance()`: the game side reads k each tick. `ELTALE_DRAW_DISTANCE=k` fixes it (tests and
  the headless dumps).

### Pipelines made ahead (nd_pipeline_warm.cpp, from KF4's gsn_pipeline_warm.inc)

- The pipeline cache (`native_pipelines.bin`) and every pipeline Granite makes are kept in the data folder
  (`native_pipeline_list.txt`, KF4's line format: shaders by SPIR-V hash and layout, the compatible render pass,
  the state); the shipped list is `port/render/nd_pipeline_list.h` (61 pipelines: MSAA 1x 12, 2x/4x/8x 16 each, the presenter's area filter 1),
  made by `tools/pipeline_list.py` from `tools/warm_test.py` cold runs (New Game and a walk, a battle with every
  element's spells, the item menu and pages, the ending, and the whole map tour) at MSAA 1, 2, 4 and 8 in every look,
  in a hidden 1600x900 window (`ELTALE_WINDOW_SIZE`: no scale of the picture fits it, so Smooth pixel edges goes
  through the presenter's area filter; at 1280x960 every look's picture was the window's size and it was never made).
  A regen where no run makes a pipeline the list lacks keeps it as it is and runs the check.
- **Making it again** (after a shader, Granite or 2D change; stale lines are skipped by their hash, so a stale list
  costs only room), one command, about 6 minutes with `--jobs 2`:
  `python tools/warm_test.py --rom ROM --regen --jobs 2` (cold runs of every case x MSAA x look, merged into the
  shipped list with `pipeline_list.py --keep`, then `build_host.py` and `--check`); `--regen --fresh` empties the
  list and builds first, so the list is made anew without stale lines (two builds). A run's own list holds only
  what the shipped list lacks, hence the merge.
- **The check**: `python tools/warm_test.py --rom ROM --check --jobs 2` runs every case (the map tour included) at
  MSAA 1, 2, 4, 8 in every look (`everything`, `classic`, `cvisuals`, `dither`: Everything on with the N64 dither,
  `border`: 4:3 with the border and N64 fog) from a fresh data folder with the shipped list only: `WARM_CHECK_FAIL`
  (exit 1) when any run makes a pipeline in play. 100 runs, about 6 minutes; `WARM_CHECK_OK` 2026-11-01.
- The looks make the same pipelines (the dither, the fog, the border and the draw distance are push constants or
  scissors, not pipeline state): the MSAA count and what is drawn decide them. Before the tour was in the list, map
  0's stone rooms (sub-areas 1 and 2: state no other case drew) made 2 (MSAA 1x) or 4 pipelines in play, ~90 ms
  each, in every preset.
- At the start the lines for the MSAA count in use are made on threads; a change of MSAA in the menu starts that
  count's. The first picture that draws waits for them (~70 ms, at the start: the boot logos), so no draw makes
  one twice. `ELTALE_WARM=0` makes nothing ahead; `P1_PIPELINES` at exit.
- **The hitch**, measured with the driver's own disk cache off and empty (`tools/warm_test.py`, MSAA 4x, hidden
  window): without the list each case made 9-12 pipelines in play, 330-360 ms in all, the worst 91-105 ms
  each (6 frames at 60) when a map, a spell or a menu was first drawn; with the shipped list from a fresh data
  folder: 0 made in play, 12 made ahead in 0.11 s on the threads, 64-75 ms waited once at the start. MSAA 1x:
  7-10 pipelines, 250 ms, worst 93 ms -> 0.

### N64 dither (Classic)

`OPT_DITHER`: the RDP's 5 bits a channel with its magic square at each N64 pixel, for draws the GPU does not
blend with the picture (the RDP dithers after its blend; blended draws stay smooth).

`ELTALE_DUMP_LOOK=classic` gives the headless dumps the Classic look (the dither on top of the N64 look);
`frame_test.py --native --look classic` compares it with the reference (`tests/render_baselines/all-classic/`,
2026-11-01). Against the default dumps (`all/`): ng 499 -> 8 solid pixels (frame 600: 167 -> 3), the fonts 167 -> 3,
chests 1,712 -> 370, menu 125 -> 103, spells 1,721 -> 1,635, battle 1,504 -> 1,457; worse: tour 6,891 -> 12,617
(24633, map 29's lava: 2,617 -> 6,414), alloc 5 -> 168, compass 1 -> 31. The items run (from the 2D fixes, merged
after): 6 (`all/` 3).

### Scaled pictures against the reference

`frame_test.py --native --hires [SxM]` (default 4x4): the dumps at scale S with MSAA M (1280x960), box-filtered to
320x240 and compared with the 1x reference at two 5-bit steps (`render_compare.py --shrink --level 2`); baselines
`tests/render_baselines/all-s4m4/` (2026-11-01: ng 4, tour 1,466, ending 2,023, battle 1,248, chests 225, spells
1,573, panel 817, spellin 2,565, alloc 234; merged with the 2D fixes' crisp 2D at a scale: spells and spellin frame
1570, the spell panel fading in, 299 -> 300; the items run 13). It guards the high-resolution path (placement, decals and the decal
depth copy at a scale, the MSAA resolve), which the 1x sets never draw: a picture moved by half an N64 pixel (2
picture pixels) takes battle 500 from 194 to 363. 2D drawn crisp at a scale (each N64 pixel a whole block) is a
different check (`frame_test.py --scale`, `render_compare.py --blocks`, from the 2D fixes branch).

### Pacing at the options

`tools/pace_test.py --hz 240` (hidden window, tests/interp-walk.input, the play half), 2026-11-01: 1x no MSAA 240.0
pictures a second, interval 4.17 ms (spread 0.15, worst 5.43); 4x (1280x960) MSAA 4x far 240.0 (0.09, 5.37); 6x
(1920x1440) MSAA 4x far 240.0 (0.10, 5.45); 8x (2560x1920) MSAA 8x farther 240.0 (0.06, 4.73); 60.00 retraces a second
and the game's own P1_OS_OK in every run. (The walk is the throne room, not a long view: the fields add 30-100
draws a tick at 2x.)

### Tests and tools

| | |
|---|---|
| `ELTALE_DUMP_SCALE=n`, `ELTALE_DUMP_MSAA=n`, `ELTALE_DUMP_LOOK=modern` (no border, fog per pixel) or `classic` (the N64 dither too), `ELTALE_DRAW_DISTANCE=k` | headless dumps with the options (contact sheets, `frame_test.py --look`, `--hires`) |
| `tools/frame_test.py --native --look classic`, `--native --hires 4x4` | the Classic look, and 4x with MSAA 4x shrunk to 1x, against the reference: `all-classic/`, `all-s4m4/` |
| `ELTALE_OPTIONS_CYCLE=N` | every N pictures the next MSAA, resolution and draw distance, and every 4N the next widescreen (off, 16:9, the window's), as the menu changes them (targets and pipelines made again in play) |
| `tools/warm_test.py` | the hitch with and without the list (cold, warm, shipped), `--collect` for pipeline_list.py; `--check` (no pipeline made in play, map tour included, every MSAA and look), `--regen [--fresh]` (the shipped list made again in one command) |
| `tools/pace_test.py --scale N --msaa N --distance far` | pacing at the options |


## 2D at a scale (2026-11-01, nr/fix2d)

The owner's Everything on (16:9, Match the window, MSAA 4x, 240) showed thin lines along the HUD digits and
meters, the element levels and the spell panel, and striped item icons. Three causes:

- **MSAA took every 2D edge as half covered.** The viewport moves half a picture pixel (the RDP samples a pixel at
  its corner), so a rectangle's edge at N64 x lands on picture pixel x * scale + 0.5: through the middle of a
  pixel. One sample per pixel decides that by the edge's rounding; MSAA's samples sit either side of the middle, so
  the first pixel inside and the first pixel past each right and bottom edge came out half covered, the one past
  the edge with the texel beyond the rectangle's last row or column (the next glyph's row, a clamped or wrapped
  column). Fix (nd_renderer.cpp `render`, `crisp` = a bigger picture or MSAA): a rectangle's corners go back by
  half a picture pixel, so its edges are whole picture pixels, while s and t stay the same function of the place
  (each pixel still samples its corner, then OPT_RECT its N64 pixel's). Screen draws (the letterbox and the
  fade: `EL_RDD_SCREEN`) get the same half pixel in clip space. At 1x without MSAA nothing moves (the native,
  wide and reference pictures are the same bytes).
- **Item icons (every scale, 1x too):** `gDPLoadTextureBlock` loads a CI8 (or 4-bit) texture as a 16-bit block.
  The icons are DMA'd into the game's own memory (`item_icon_cache_tex`, native .bss: bytes in order), but nd_2d's
  TMEM by reference read a 16-bit load's source as host-order halfwords, so each pair of texels came out swapped
  (the stripes; R4's flame bug in the model loader was the same and is fixed there by waiting for the render tile's
  size). Fix (nd_2d.c `nd_2d_load_block`): such a load's unit is the sampling tile's texel size (`Load.unit` 0).
  No save in the lists had items, so the census and the compare never drew one: `tests/render_frames_2d.txt` now
  has `items` (the "items" quick save: `python tools/make_items_save.py` writes it from battle-stuck with twelve
  items; tests/item-icons.input opens the item menu and moves the cursor).
- **The presenter's scaling** (not the lines, but the same pictures): Match the window rounds the scale up and the
  presenter fitted the picture with a linear `vkCmdBlitImage` (2 x 2 taps a window pixel: 1200 lines into a 1080
  window mixes unevenly, and above 2:1 it skips picture pixels). *Smooth pixel edges* now has its own scaling
  (render_native.cpp `scaled_for_window`, shaders/present_area.frag): the picture is drawn at the size the window
  shows it at (`present_fit_size`) with an area filter on linear light (each window pixel the share-weighted
  average of the picture pixels under it: blocks stay blocks with one mixed pixel at an edge; made larger it is
  sharp bilinear), and the presenter copies that 1:1. *Sharp* (nearest) and *Smooth* (linear) keep the blit.
  `ELTALE_PRESENT_BLIT=1` keeps the blit for every mode (to compare). At 1080p (2130x1200 into 1920x1080) the
  two differ by a mean of 0.4 of 255; pacing at 240 with 5x MSAA 4x far in a 1920x1080 hidden window: 240.0
  pictures a second, interval 4.17 ms (spread 0.08, worst 4.79).

### The check (tools/frame_test.py --scale)

`python tools/frame_test.py --rom ROM --saves C:/code/eltale/out/bugsaves --scale N [--msaa M] [--aspect A]`: every
list with the 2D kinds alone native, each dump at scale N with MSAA M (default 4) and shape A (default 16:9) and,
from the same recording, at 1x without MSAA (`ELTALE_DUMP_PAIR=1`: frame_N.1x.png). `render_compare.py --blocks`
counts the N64 pixels whose block in the big picture is not the 1x pixel throughout (8 bits, 2 steps, no 3x3
rule: the faults are one picture pixel thin), against `tests/render_baselines/scale4-msaa4-16x9/` and
`scale5-msaa4-off/`. What is left is the 2D meshes, drawn at full resolution on purpose (the compass disc, about
670 a frame; the element flash): 0 in every frame without one. The panels list (merged after): spellin 2,285 at 4x
16:9 (2,327 at 5x 4:3; 0 in the frames without a spell glow), alloc 8,851 (9,042: the compass, ~688 a frame, and
the element glows).

| set | before (sum over the lists) | after |
|---|---|---|
| 4x, MSAA 4x, 16:9 | ng 36,445, intro 44,161, menu 24,666, title_en 24,790, ending 14,766, tour 15,150, items 4,420, ... | ng, intro, title, fonts, save, ending, battle, chests, panel 0; tour 8,217, menu 1,986, items 1,318 (the compass) |
| 5x MSAA 4x 4:3, 6x MSAA 8x 16:9, 3x no MSAA 16:9, 1x MSAA 4x | | the same: only the 2D meshes (1x MSAA 4x: 40 over the tour, the compass's edges) |

Tests run for the fix: p1_checks --tour unchanged (New Game 74b8baf63f9c1889 / be0b67b7b718648e, the map tour
a2d2e81245d538d4 30,374 frames, ...), `frame_test.py --twice` OK (the new run's reference hashes added to
render_ref.txt), `--native` and `--wide` every baseline OK and 113 of 115 pictures the same bytes as before the fix
(the two others are the new run's icons), interp_test OK (ng, walk, flames, spells, tour, ending), host_checks
native unchanged.

| | |
|---|---|
| `ELTALE_DUMP_PAIR=1` | each headless dump also as `<name>.1x.png` (1x, no MSAA) |
| `ELTALE_WINDOW_SIZE=WxH` | the window's size (hidden-window tests of the presenter's scaling) |
| `ELTALE_PRESENT_DUMP_AT=n,m` | with `ELTALE_PRESENT_DUMP`: the window as shown at those pictures (`native_shown_<n>.png`) |
| `ELTALE_PRESENT_BLIT=1` | the presenter's linear blit for Smooth pixel edges too |

## The mouse camera's pictures between ticks (render.h v8, 2026-11-01)

`ElRdCamera.flags` (v8): `EL_RDCAM_ORBIT` marks a tick whose view the player turned or zoomed with the mouse
camera (docs/CONTROLS.md; `nd_camera` sets it from `Port_MouseCameraActive`). Between two such ticks
`Interp::eye_at` moves the eye round the point looked at (yaw the short way, pitch and distance lerped; the
endpoints use the same code, so `correct` still makes t = 0 and 1 exact) and the cut test leaves out the
eye's move, the turn and the zoom: a mouse turn of 20 degrees a tick stays smooth. A jump of the point
looked at or of the field of view is still a cut. Ticks without the flag are as before. `nd_billboard_cone`
returns 181 degrees (no cone) while the view is the mouse camera's. `tools/interp_test.py` cases `mouse`
and `mouse-battle`.

## The presenter's robustness (2026-11-01, nr/present)

- **A lost GPU device ends the game cleanly.** The presenter's Vulkan calls (wait, acquire, submit, present, ImGui's)
  go through `check()` (present_vk.cpp), which marks `VK_ERROR_DEVICE_LOST`; `present_lost()` says so and nothing is
  presented after. The renderer (render_native.cpp `show`) then stops drawing and calls `host::fatal` (host.cpp): a
  line on stderr, the line in the data folder's `error.log`, a message box (not with `ELTALE_HIDDEN_WINDOW`) and the
  game asked to quit at its next poll, as closing the window does (its own shutdown path). Granite's own submits only
  log an error, so a loss shows up at the next picture's present (a sixtieth of a second at most).
  `VK_ERROR_SURFACE_LOST_KHR` (acquire, present or the surface's capabilities) makes the window's surface and
  swapchain again in place and the game goes on; a new surface that cannot be presented to in the same format, or
  surfaces lost again and again (16), are a loss as above.
  Test: `ELTALE_SIM_GPU_LOST=device:N` or `surface:N` (the Nth picture presented acts as if the GPU said so, before the
  acquire). Hidden window, New Game walk: `device:300` ends the game 2.5 s in (exit 0, `P1_OS_OK` printed, the
  pipeline cache saved, `error.log` written); `surface:300` makes the surface again and plays on to frame 1500 (the
  same `P1_OS_OK` as without; pictures shown after it: `ELTALE_PRESENT_DUMP_AT`). A real lost device has not been
  tried (Granite's `wait_idle` at the end gets the loss's error and goes on).
- **Minimised, nothing is drawn.** `not_shown()`: the window minimised, or occluded as SDL reports it (a visible
  window only: the tests' hidden windows are not), and no picture is built or presented, at the retraces or between
  them (`wait()` returns at once: the game's own pacing sleeps). The game's timing is its own and goes on: hidden
  window at 240, `ELTALE_SIM_MINIMISED=600-1100` (retraces 600 to 1100 act as minimised): 0 pictures in the span (2,004
  without), 60.00 retraces a second through it, the same `P1_OS_OK` (frames 1500, last hash, DMA chunks) and the same
  7,921 pictures after it as a run without.
- **Resizes settle first.** With Match the window (or the window's widescreen shape) every step of a drag made the
  picture again (a GPU wait) and the Smooth pixel edges image too. Now, while the window's size changed less than
  0.25 s ago (`RESIZE_SETTLE`), the picture keeps its size and shape (the game is told the shape only with the new
  picture) and the presenter fits it (Smooth pixel edges uses the blit meanwhile); the new picture is made once the
  window has settled. `ELTALE_SIM_RESIZE=N` (a test): from the Nth picture, 150 pictures each 4 pixels wider and 2
  taller; `ELTALE_RESIZE_SETTLE=0` the old way; `P1_PRESENT` at the end counts pictures and window images made. Match
  the window and the window's shape, 1280x960 hidden window: pictures made 22 -> 2, window images 149 -> 1; picture
  intervals during the drag mean 14.3 -> 8.8 ms (the swapchain is still made again at every step: it must match the
  window).
- **The flash recorder is lazy.** Its 64 images (at most 2x: 852x480 at 16:9, ~105 MB of GPU memory) and the copy
  into them each picture are made only once F8 has turned it on; F8 again writes them (the Graphics page says which).
- **Shader warm-up** says on the Graphics page that it takes effect at the next start (the list is read and made at
  the start only). The presenter's area-filter pipeline (Smooth pixel edges, `present_area.frag`) is now one the
  warm-up can make: its SPIR-V is in the warm-up's shader table and its lines are tagged `p` (its pass is one sample
  whatever the MSAA), made with the first MSAA count started. A run writes it into the data folder's list
  (`G p ...`), and the next start makes it ahead (11 made ahead, 0 in play, instead of 1 in play). The shipped list
  (`nd_pipeline_list.h`) has its `p` line since the merge into native/render (warm_test's runs now use a 1600x900
  window: at 1280x960 every look's picture was the window's size, so the filter was never used and the first regen
  recorded nothing; without the line a 1600x900 New Game made it in play, 11 ms).

### The terrain rings at a scale (measured, not changed)

The audit's numbers (battle 500, the mean over the reference's ring pixels: reference 163, native 1x 158, native 4x
box-shrunk to 1x 147) suggested the RDP drew the 1-2 pixel ring segments fatter. Measured against the same run with
the effect kind dropped (`frame_test --native` without `effect`: the reference and native pictures without the rings),
the rings' light (the luminance they add, summed over the rings' boxes) is the same at every scale: reference 110,742,
native 1x 110,474, native 4x shrunk 110,352 (within 0.4%). A segment (D_800C4840) is not a thin quad: it is a strip
3 units tall (`ScaleAxes(2, len, 1.5)` of a quad at x = 1, z -1..1: 3 to 10 pixels at 1x in this frame) whose line
is its 8x8 I8 texture's profile across it (rows 0x55, 0xfe, 0x5b of eight: the line is an eighth of the strip). At
1x each pixel samples that profile at a point, so the line comes out one pixel at nearly its peak; at 4x it is drawn
as it is, 0.6 of an N64 pixel wide, and shrunk to 1x it falls across two rows (the 200 brightest pixels: 150 at 1x,
126 at 4x shrunk). The 1x difference on the reference's own pixels is the line's place, not its weight. A minimum
width (the strip's z scaled so its projected width is at least k N64 pixels, tried in nd_renderer.cpp) only adds
light: k 4/5/6/8 -> 1x 111,578 / 112,452 / 113,746 / 118,282, 4x 111,259 / 112,437 / 113,660 / 118,256 (peak at 4x
126 -> 131 at k 8), so the 1x picture moves away from the reference. Left as the reference draws it; the streaks
(D_800EB8E8, melee hits) are in no test frame and were not looked at.
