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
| Z: compare, update, decal | reversed-Z D32F (depth = near/w: infinite, exact near plane); decal = GREATER_OR_EQUAL + depth bias; no Z without `G_ZBUFFER`; far plane cut in the pixel stage only for draws whose box crosses it |
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

Each run of `tests/render_frames.txt` is one game with `ELTALE_ND_COMPARE=1 ELTALE_DL_KINDS=<kinds>
--dump-frames`: eltale-rdpref draws the reference of those kinds alone (`<work>/<run>/png`), the native
renderer writes its own (`native`), `render_compare.py` counts the solid differences against
`tests/render_baselines/<run>.txt` (`diff` pictures). About 20 s for all five runs.

R2's numbers (map objects, 47 frames; solid = differing pixels whose 3x3 neighbourhood also differs, at
5 bits with one step of tolerance):

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
| backdrop | map.c `Map_DrawBackdrop` | 1-cycle, no Z, texture perspective off, TEX0xENV; its palette `map_backdrop_tlut` (native .bss) is read per frame already |
| actor | actor/anim.c (`Anim_DrawModel`, `Anim_DrawNode`), player.c, npc.c, chest.c, enemy.c, boss.c | node trees, billboard nodes, dlSeq flipbooks (a mesh per list), PRIM tints, texgen |
| shadow | gfx/shadow.c | decal Z, alpha-only combiner, reversed fog range (its own fog position) |
| billboard | map.c `MapObj_DrawBillboards`, `MapObj_DrawAnimTexBillboards` | cut-outs (c8113078); animtex frames load a texture per frame: put the frame in the entry list or the draw |
| effect (R4) | gfx/sprite_effect.c, gfx/effect.c | many small draws: a texture per template |

map.c is shared by backdrop and billboard (different functions: merges are clean if each keeps to
its own). The loader has no 2D yet: texture rectangles and fill rectangles (R5) need native quads,
not lists.

## What is left after R2

- The other kinds (R3-R5); under `--renderer native` only map objects draw (the rest of the frame is
  the clear colour).
- Pictures between ticks (R6): the recording keeps what it needs (ids, inputs); `render` draws one.
- Resolution scale, MSAA, widescreen (R7): the picture and depth images are 320x240; the half-pixel
  move and the 8-pixel border scale with it.
- The pipeline list / warm-up (R7): one program, few pipeline states.
- Near-plane clipping as the RSP does it, the RDP dither (N64 dither switch): the residual differences.
