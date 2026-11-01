# Eltale Monsters: how the game draws (survey for the P2 native renderer)

Research only (2026-10-31). Nothing in the repo was changed. Code refs are to `c:\code\eltale\work\src`.
Census tools and their JSON output are in this folder (see "Reproducing" at the end). The frame dumps
were deleted afterwards.

## 0. The short version

- **Every matrix is built by the CPU and loaded at the top level of the frame list.** Model display lists
  never load a matrix (15,027 MV loads seen, all at depth 0) and never share vertices across matrices
  (0 triangles span two matrices). So **every 3D draw is "one rigid matrix + one static display list"**.
  The static lists live in ROM-loaded blocks and can be turned into GPU meshes once and cached.
- **No framebuffer tricks.** Only one colour image (the frame) and one depth buffer. No render to
  texture, no textures read from the framebuffer or Z, no motion blur, no wipes. The only "screen
  effects" are a full-screen fade quad, letterbox bars, camera shake and model colour flashes. The
  Controller Pak manager is the one CPU-drawn screen.
- **The map is lit at run time**: map geometry is drawn with `G_LIGHTING` (normals, 3 directional
  lights + ambient from the clock-driven environment), not with prelit vertex colours, and fogged
  in the blender. Map CI textures use a palette blended by the clock.
- **Draw code is also update code.** `Gfx_BuildFrameDisplayList` runs the camera, advances every
  model's animation, steps effects, fades, HUD meters, palettes and the RNG. The game can't be asked
  to "draw again" for a picture between frames: the renderer has to record what each tick drew.
- **Gameplay runs at no more than 30 fps** (it waits for 2 retraces per frame, with no delta time).
- The game is small: a busy gameplay frame is about 3,200 triangles, 190 matrices, 5,500 vertices and
  300 texture loads. Totals over 244 frames: 22 combiner modes, 18 render modes, 6 texture formats.

## 1. One gameplay frame, in draw order

`Gfx_BuildFrameDisplayList` (game/core/main.c:317) for modes 1 (field), 6 (message cutscene) and 7
(camera cutscene):

| # | Call | Draws | Also updates |
|---|---|---|---|
| 0 | frame setup | seg 0/1/2, `D_1000010` (viewport, clip ratio), Z clear and colour clear as FILL rects 8..311 x 8..231 | |
| 1 | `Camera_Update` | projection (+ shake) and view on the projection stack, LookAt, PerspNormalize | **runs the camera mode handler** (follow, fixed eye, battle, delayed turn), preset changes on R |
| 2 | `Env_PrepareFrameLighting` | fog position, env colour | light directions (one is camera-relative) |
| 3 | `Env_ApplyLightSlot` + `Map_DrawBackdrop` | sky/far-ground cylinder | light slot animations step |
| 4 | `MapObj_UpdateAndDrawNearby` | map geometry, spinners, billboards | grid cell search, wave objects move, TCB-animated objects step, emitters spawn sprites (RNG), clock palette blend, palette rotation, ambient SFX |
| 5 | `Player_Draw` | player model | **Anim_AdvanceFrame**, floor probe for the shadow |
| 6 | `Npc_DrawAll` | NPCs within 300 units of the player | Anim_AdvanceFrame |
| 7 | `Chest_UpdateAll` | chests | chest open timer and anim |
| 8 | `MapObj_DrawTranslucent` | wave/translucent objects, scrolling-texture objects, animated-texture billboards | scroll offset, flipbook frame |
| 9 | `Enemy_DrawAll`, `Boss_Draw` | enemies (battle), field boss | Anim_AdvanceFrame; flying enemies drawn at a raised y |
| 10 | `Spirit_SpawnSparkles` | (spawns sprites) | |
| 11 | `Shadow_DrawAll` | queued blob shadows (64 max) | |
| 12 | `Effect_UpdateAndDrawSprites` | particle pool (256) | **sprite update callbacks run here** |
| 13 | `Effect_UpdateAndDrawAll` | streaks, stat-up markers, terrain rings, model effects, floating numbers | all effect timers; `Math_Rand` for marker alpha |
| 14 | `Fade_UpdateAndDraw` | letterbox bars, fade quad | ramp step |
| 15 | `Text_UpdateAndDrawWindows` | dialogue windows | text reveal |
| 16 | `Hud_Draw` | meters, element icons, compass, battle HUD, markers | meter easing, panel slide, timers; writes `hud_npc_marker_screen_y` which **game logic reads** (player.c:347 picks the top or bottom text window from it) |
| 17 | `Item_UpdateInventoryMenu` | item menu | menu logic |
| 18 | `Cutscene_UpdateAndDrawImageCues` (mode 7) | ending-tour pictures | |

Other modes draw only 2D: mode 2 `Menu_Update` (pause menu, no 3D behind it), 3 title overlay,
4 intro overlay, 5 ending overlay (+ fade). The Pak manager (`overlays/pakmenu`) draws into its own
framebuffers with the CPU.

## 2. Draw kinds

Draws per frame are from the dumped frames (tris = triangles; "avg/max" over dumped gameplay frames).
Difficulty is for a game-level port (1 easy .. 5 hard).

| Kind | Code | Data read | Matrices / camera | Draws per frame | Diff. |
|---|---|---|---|---|---|
| Map geometry (static objects) | map.c `MapObj_UpdateAndDrawNearby`, `MapObj_CollectCell` | `map_objs` (MapObj x,y,z,angle,scale,model,kind) in the sub-area grid; `map_obj_models[model].dl` in the map block | `guPosition(0, angle, 0, scale, x,y,z)` per object, world space; proj×view on the projection stack | 13-60 objects (mv loads), 300-1,640 tris, max 3,183 | 2 |
| Animated map objects | same, `MapObjAnim_*`, `Math_EvalTcbSpline` | `mapobj_loop_anims` (TCB tracks, frame in `kind` low byte), 4 `mapobj_anim_slots` (doors etc.) | position/yaw offsets from splines | few | 2 |
| Wave objects (kinds 1-3) | `MapObj_CollectCell` | `sins` of 4 phase pairs | position written back into MapObj each frame | 0-10 | 2 |
| Spinners (0x82) | `MapObj_QueueSpinner`, `MapObj_DrawSpinners` | double-buffered spinner slots | `Math_MtxRotScaleTrans(0, yaw, spin, ...)`, spin -0.9°/frame | 0-8 | 1 |
| Billboards (0x10, trees etc.) | `MapObj_DrawBillboards` | `animTexDls[type]` pair (setup + quad), per object | yaw = camera yaw; culled at 1,500 units and by angle vs fovy; queues a shadow | up to ~90 per map (657 placed in all maps) | 2 |
| Animated-texture billboards (0x11/0x12) | `MapObj_DrawAnimTexBillboards` | CI8 16x32 frame tables, `mapobj_animtex_alpha` | yaw = camera yaw; frame = `model++` | few | 2 |
| Scrolling-texture objects (0x40-0x45) | `MapObj_DrawTranslucent` | 5 RGBA16 32xN textures at fixed 0x8010xxxx/0x801xxxxx; `SetTileSize` t offset +2/frame | guPosition | few | 2 |
| Translucent map objects (0x7F/0x80, waves) | `MapObj_DrawTranslucent` | `mapobj_translucent_alpha[map_id]` via env alpha | guPosition | avg 3-14 tris, max 58 | 2 |
| Map emitters (0x81, 0x83 arcs, 0x84 drops) | `Effect_QueueMapObjEmitter`, `MapObj_Emit*Sprite` | sprite templates | as sprites | via sprite pool | 2 |
| Sky / backdrop | `Map_DrawBackdrop` (map.c:664) | `map_backdrop_defs[id].dl_addr` (segment 1): a 16-sided cylinder, radius 300, CI8 strips from `map_backdrop_tex` (0x4000 bytes per backdrop, DMA'd) with `map_backdrop_tlut` | translate to camera eye (y-10); no Z, 1-cycle, texture perspective off, TEX×ENV | 142 tris (when `map_env_flags & 4`) | 2 |
| Player | player.c `Player_Draw` → anim.c `Anim_DrawModel` / `Anim_DrawNode` | `player_char_data[set]` AnimSets at 0x80206000 (+ per-map block at 0x80200000): nodes with parent, pivot, TCB track (9 channels: T, R in degrees, S), dl index or DL sequence | per node: `Math_MtxFRotDegScaleTrans` × parent (`Math_MtxFMul`), pivot offset; billboard nodes `Math_MtxFAimAt` the eye | 200-290 tris, ~20 nodes | 3 |
| NPCs | npc.c `Npc_DrawAll` | `npc->def->anim_sets` (block at 0x8020E6F0 / 0x802A0000) | as player; skipped beyond 300 units of the player; point-sampled beyond ~141 units | avg 125-175 tris, max 807 | 3 |
| Chests | chest.c `Chest_UpdateAll` | `D_8010FDC4` AnimSets | as player | few | 2 |
| Enemies | enemy.c `Enemy_DrawAll` | `enemy_anim_tables` (area set DMA'd to 0x8020E6F0) | as player; flying ones drawn at max(y, player y)+body height | 0 field, ~180 tris in battle, max 556 | 3 |
| Bosses | boss.c `Boss_Draw` | `D_803ABA58` (common block), DMA to 0x802A7BC0 | as player | field bosses | 3 |
| Shadows | shadow.c `Shadow_Queue`, `Shadow_DrawAll` | segment-1 quad `D_1000230` (4 verts), setup `D_1000270` | `Math_MtxFAlignYToVector(floor normal)` + position + scale; fog range reversed | 2 tris × 2-24 shadows | 1 |
| Sprite particles | sprite_effect.c `Effect_UpdateAndDrawSprites`, `Effect_LoadSpriteTexture` | 256 `EffectSprite` slots (template copy: tex, fmt 0-7, w/h, cms/cmt, dl, update fn) | `Math_MtxFAimAt` eye (spherical billboard) + roll, or aligned to velocity (flag 1) | 0-130 tris | 2 |
| Streaks | effect.c `Effect_UpdateAndDrawStreaks` | 32 slots | aligned to velocity, length = distance travelled | battle | 2 |
| Terrain rings | `Effect_UpdateAndDrawRings` | 4 rings × 16 points sampled on the floor | 16 stretched segments per ring | battle | 2 |
| Model effects (spells) | `Effect_UpdateAndDrawModelEffects`, `Effect_DrawModelEffectGeometry`, `Effect_ConfigureModelEffectTexture` | 32 slots: AnimSet `D_800EB2B4[]` + DL table `D_800CADA8[]` (magic block 0x800C0000), `EffectTexAnim` (scroll s/t, flipbook, I8 or CI8 + TLUT, clamp) | `Anim_DrawNode` hierarchy; texgen + lighting for "shiny" ones | up to ~320 tris | 3 |
| Stat-up markers | `Effect_DrawStatUpMarkers` | `D_800EB9C8` | aim at eye over the player | rare | 1 |
| Floating numbers | `Effect_DrawFloatingNumbers` | 128x10 RGBA16 digit strip `D_8039E290` | `Camera_WorldToScreen` → texrects | ≤10 numbers | 1 |
| Fade | fade.c `Fade_UpdateAndDraw` | RGBA ramps (black, grey, slow, 256-step, damage flash) | `D_2000000.ortho` + identity; quad ±160×±120 | 2 tris | 1 |
| Letterbox | `fade_letterbox_dl` | fixed fill rects 0..319 × 0..40 and 200..239 | screen | 2 rects (modes 6/7, ending) | 1 |
| Text windows | text_window.c `Text_UpdateAndDrawWindows`, text.c `Text_DrawEncodedString` | font CI4 at 0x803245A0 + TLUT 0x80332D30; English pages at 0x807F0000+ (port) | texrects, 1:1 | 30-60 texrects | 1 |
| HUD | hud.c `Hud_Draw` and helpers | common block 0x8039ADB0 (`D_803A86F8`/`D_803A8718` texture descriptors, DLs `D_803A8788`, `D_803AA*`) | ortho + translate(-160,-120); texrects; a few textured tris under a translate (element glows) | 20-35 texrects | 1 |
| Compass | `Hud_DrawCompass` | `D_803AA758` (disc, ring, N, dots) | ortho × (rotate z by `camera_state.yaw`, translate to 277,41) | 2-4 tris + 2 texrects | 1 |
| Pause menu (mode 2) | menu.c `Menu_Update` | menu pictures, map pages (DMA'd pictures) | texrects only | ~40-80 texrects | 1 |
| Item / save menus | item.c, save_menu.c | item icons DMA'd on demand (item.c:320); 1-pixel FILL-mode frame lines | texrects + fill | | 1 |
| Title | overlays/title `Title_DrawScreenAndMoveCursor`, `Title_StepAndDrawScreenFade` | CI8/CI4 pictures in strips (304x6, 320x6) | texrects; fade is a 1-cycle fill rect (0,0)-(336,960) | ~60 | 1 |
| Intro | overlays/intro `Intro_RunSpriteScript` (150 sprite slots with move/clip/fade), splashes | pictures | texrects in an own double-buffered sub-list | ~15-50 | 1 |
| Ending | overlays/ending `Ending_UpdateAndDrawScrollingBackdrop`, `Ending_UpdateAndDrawSprites`; then the camera tour (mode 7) with `Cutscene_UpdateAndDrawImageCues` | pictures at 0x801Cxxxx with own palettes | texrects; tour = full 3D path | | 1-2 |
| Pak manager | overlays/pakmenu | CPU pixels in 0x80200000/0x80225800 | one 320x240 RGBA16 texture | 1 | 1 |

### Model data and how lists are stored

- **Segments**: 0 = physical (absolute addresses), 1 = the common display-list block (ROM 0x71920-0x753F0
  at 0x80095AF0: frame setup `D_1000010`, map setup `D_10000E8`, player setup `D_10000A0`, shadow quad
  `D_1000230/270`, the backdrop cylinders), 2 = this frame's `GfxFrameData` (proj, ortho, view, 550
  matrices, LookAt). Nothing else uses segments.
- **Map data** is linked at its load address (KSEG0 pointers inside): a shared block (ROM 0x753F0, 64 KiB
  at 0x80100000, scroll textures etc.), the area block (`map_area_def->romStart..romEnd` →
  `dst`, 0x8011xxxx-0x801Fxxxx), an extra range for flagged maps (chests at 0x8010FE00), and
  0x801E0000 for maps 3, 5, 7, 12. The port decodes the records (port/data/map_area.c) but leaves
  display lists, vertices and textures as N64 bytes in RDRAM.
- **Map model DLs**: vertices with normals (lit), `G_CULLDL` bounding checks (1,975 seen), DL branches
  (149), their own texture loads (SETTIMG to the map block, mostly RGBA16 32x32), TLUT loads from
  `map_clock_tlut` (0x800810B0, blended by the clock) for CI textures.
- **Actor models**: `AnimSet {nodes, dls, nodeCount, length}`; `AnimNode {parent (1-based), flags (1
  billboard, 2 record world position), pivot, KfAnim track, dl or dlSeq/dlSeqLen}`. Animation =
  Kochanek-Bartels (TCB) Hermite splines over 9 channels (`Math_EvalTcbSpline`, keyframe.c) at an
  **integer** frame; the frame advances by `animStep` per tick in the draw call. A node can swap its
  DL per tick from `dlSeq[ticks % len]` (flipbooks). Rigid parts only: no skinning.
- **Math helpers** (math.c): `Math_MtxFRotScaleTrans` (rot x/y/z radians, uniform scale, translate),
  `Math_MtxFRotDegScaleTrans` (keyframe channels), `Math_MtxFMul`, `Math_MtxFAimAt` (billboard to a
  point), `Math_MtxFAlignYToVector` (shadows, streaks, rings), `Math_MtxFScaleAxes`, plus libultra
  `guPosition`, `guTranslate`, `guMtxF2L`, `guMtxCatF`. There is **no matrix stack**: every MV matrix is
  a G_MTX_LOAD|NOPUSH of a full model-to-world matrix; proj×view sits on the projection stack.

## 3. Feature census

Frames: New Game (18 dumps), map tour of all 258 sub-areas (101), ending + camera tour (50), the
battle-stuck battle (36), pause/item menu run (39) = 244 frames, 325,761 primitives, 451,907 vertices.
Per-run numbers from every frame (`P1_FRAME_IMAGE`, not just dumps):

| Run | Frames | Commands mean / p95 / max | Matrices mean / max | Vertices mean / max | Tex loads mean / max |
|---|---|---|---|---|---|
| New Game | 700 | 1999 / 2657 / 2657 | 44 / 65 | 725 / 1054 | 123 / 157 |
| Map tour | 30375 | 2326 / 4548 / 6605 | 60 / 176 | 1677 / 5540 | 105 / 313 |
| Ending + tour | 10000 | 2000 / 3823 / 4010 | 35 / 128 | 1690 / 4474 | 106 / 213 |
| Battle | 1500 | 4338 / 4792 / 4917 | 156 / 193 | 3334 / 3653 | 201 / 224 |

Top-level list: mean 447 commands, max 1,099 (buffer holds 2,560). Matrix pool 550 per frame; effects
stop drawing at 486 (`gfx_mtx_index < 0x1E6`).

**Opcodes used**: MTX, MOVEMEM (viewport, LookAt X/Y, 3 lights + ambient), VTX (n up to 32), DL, branch
DL, ENDDL, TRI1, TRI2, CULLDL, TEXTURE, GEOMETRYMODE set/clear, OTHERMODE H/L, MOVEWORD (segment, fog,
numlight=3, clip ratio, persp norm), and the RDP set (SETTIMG, SETTILE, SETTILESIZE, LOADTILE 22,147,
LOADBLOCK 1,602, LOADTLUT 1,844, TEXRECT, FILLRECT, colours, combine, scissor, Z/colour image, syncs).
**Never seen**: BRANCH_Z (no LOD), MODIFYVTX, LINE3D, POPMTX, matrix push/mul on MV, TEXRECTFLIP,
SETOTHERMODE (full), prim depth, YUV/convert, colour key, any unknown opcode.

**Cycle types**: 2-cycle for all 3D except the backdrop (1-cycle); 1-cycle for texrects and 2D tris;
FILL for the clears and the item menu's frame lines. No COPY mode.

**Combiners (22)**, main ones (cycle 1 ; cycle 2):

| Tris | Combiner | Used by |
|---|---|---|
| 218,237 | TEX0×SHADE, a=TEX0 ; COMB+ENV | standard lit textured (map, actors); ENV = the light slot's additive flash colour |
| 17,033 | PRIM×SHADE, a=ENV ; COMB+ENV | untextured coloured parts |
| 13,792 | TEX0×SHADE ; COMB×PRIM+ENV | player/enemy parts tinted by PRIM |
| 11,476 | TEX0×ENV (1-cycle) | backdrop |
| 4,923 | TEX0 (1-cycle) | 2D texrects |
| 2,846 / 1,970 | (1-TEX0)×ENV+TEX0, a=TEX0×ENV [×SHADE] | spell effects (I8 intensity tinted) |
| 2,776 | 0 colour, a=TEX0×SHADE | shadows (alpha only, black) |
| 2,134 / 1,284 / 619 / 456 / 393 / 250 / 170 / 138 / 100 / 62 / 58 / 2 | variants | cutout parts, translucent objects (TEX0×SHADE a=TEX0×ENV), particles, fade (SHADE×ENV), letterbox (ENV) |

**Render modes (18)**: all 3D uses `FOG_SHADE_A` in cycle 1 (fog factor from shade alpha):
c8112038 opaque Z (200,626 tris), c8112078 AA opaque (35,462), c8113078 texture-edge cutout
(CVG×alpha, 13,559: billboards, foliage, player parts), c8112d18 opaque decal, c8104e50 translucent
decal (shadows), c81049d8 / 0c1849d8 translucent Z-tested no Z-write (water, translucent objects),
c8104dd8 translucent decal, 0c184a50 / 0c184b50 translucent / "cloud" (particles, spell effects), and 2D:
0f0a4000 opaque 1-cycle (alpha compare threshold on for glyphs: 3,948 texrects), 00504240 / 00504340
translucent 2D, FILL. Alpha compare: threshold only (2D), plus CVG×alpha cutouts in 3D. Z source is
always per-pixel. Dither off except a few 2D/backdrop draws.

**Geometry modes**: 3D = ZBUF|SHADE|SMOOTH|CULL_BACK|FOG|LIGHTING (map, actors); some without cull
(double-sided); `TEXTURE_GEN` on 900-odd player/enemy/effect tris (`Effect_ConfigureModelEffectTexture`
sets LIGHTING|TEXGEN for shiny spell models; LookAt from `guLookAtReflectF`); effects and shadows
unlit; backdrop SHADE only (no Z). Lights: always 3 directional + ambient (`Env_FillLights3`):
light A = the sun/area direction (rotates with the clock when `map_env_flags & 1`), lights B and C =
a fixed (-89,89,20) direction **in camera space** (`Env_ComputeLightDirs`), recomputed per frame.

**Textures**: 1,613 distinct (address, format, size). Formats: RGBA16 (most 3D), CI8 and CI4 with
RGBA16 TLUTs (map CI textures, backdrop, all 2D), I4/I8/IA4 (effects, shadows). Sizes 8x1 to 64x64 in
3D (32x32 dominant), 2D strips up to 320x6 / 304x13. Odd tile extents (9x9, 33x65, 129x11) come from
SETTILESIZE spans one texel larger than the mask for bilinear edges. Wrap modes: wrap and **mirror**
both common (s: 216k wrap / 76k mirror / 7k clamp; t: 162k / 118k / 19k), masks 3-7, no shifts.
Filter: bilinear (N64 3-point) in 3D, point for most 2D; perspective correction on except backdrop.
No mip-mapping, no detail/sharpen use (one sharpen bit set with LOD off), no TMEM tricks seen beyond
TLUTs at TMEM 0x100 (256 entries ×1,486, 16 entries ×358).

**Fog**: `gSPFogPosition` per map from the environment keyframes (values 750..1015 for near, ~1000 far,
i.e. the fog runs over the last part of the depth range: with near 15 / far 2400, near=920 starts at
about 175 units). Shadows set the range reversed. Fog colour = the light set's 4th colour, also the
clear colour on `map_flags & 0x80` maps.

**Z buffer**: one, 0x802DA800, cleared with FILL 0xFFFC each frame. Decal Z mode on shadows and some map
decals; translucent mode without Z-write on water, particles and spells. 2D has no Z.

**Rects**: 5,127 texrects, all 1:1 (dsdx = dtdy = 1.0), none flipped; extent 0..320 × 0..224. Fill
rects: the two clears, letterbox bars, title fade, item menu frame lines.

**Viewport and scissor**: viewport 320x240 centred; scissor (8,8)-(312,232): the game draws a 304x224
picture inside an 8-pixel border, the clears cover the same box.

**Frame-buffer tricks**: none. Two colour images (Z clear, frame), no texture from FB or Z, no CPU reads
of the frame, no motion blur, no wipes. CPU-drawn screen: the Pak manager only.

**Hard for a "game level" path**: nothing in the census needs N64 emulation semantics beyond: the
2-cycle combiner + FOG_SHADE_A blender, N64 per-vertex lighting and texgen formulas, mirror/clamp/mask
addressing with odd tile spans, 3-point bilinear, TLUT palettes that change at run time, CVG×alpha
cutouts (alpha test), decal Z, and drawing translucent things in the game's own order.

## 4. Camera and the frame

- **Projection**: `guPerspectiveF(fovy, 4/3, near, far)` in `Camera_Update` (camera.c:168): aspect
  hard-coded. Presets per sub-area (`CameraPreset`: mode, eye for fixed modes, heights, distance, pitch
  and yaw limits, fovy, near, far). Seen in the ROM: fovy 25-40, near 15, far 1000-2400 (default preset
  25/15/2500). Cutscenes set fovy 35 (cutscene.c:607) or the scene's own (scene.c:81).
- **View**: `guLookAtReflectF(eye, at + height oscillation, up y)` from `camera_state` (eyeX/Y/Z,
  atX/Y/Z, yaw, pitch...). Camera modes (`camera_mode_handlers`): follow (spring: velocity, damping 0.8,
  gain 0.02), fixed eye, battle, heading, delayed turn. Cutscenes evaluate a KfAnim spline at an integer
  frame (`Cutscene_ApplyCameraFrame`). Shake: a translate on the projection that flips sign and decays
  ×0.8 each frame (`camera_shake_x *= -0.8`).
- **Stacks**: projection = [shake translate] × proj × view; MV = model-to-world per draw. The HUD/fade
  use `ortho` (guOrtho ±159 × ±119, y down, near 10 far 200) built once per map.
- **Rate**: gameplay, message cutscene and camera cutscene loops wait for `nnsc_retrace_count >= 2`
  before sending the next frame: **≤30 fps** (NTSC), and the game simply slows if a frame takes longer
  (no delta time; `D_8007A8B8` measures the frame time but nothing reads it). The port runs 2.06
  VIs/frame (tour 30,374 frames in 62,511 VIs; battle 1,500 in 3,085). Menu, title, intro and ending
  loops only wait for the previous frame's swap (2 VIs in the port; hardware pacing to be calibrated,
  as the P1 notes say).
- **Update/draw split**: the loop body (main.c:504-) runs input, battle, `Player_Update`,
  `Npc_UpdateAll`, `Magic_UpdateEffects`, `Env_Update`, `Map_StartFrame`, `MapObjAnim_UpdateAll`, then
  `Gfx_BuildFrameDisplayList`, which does the rest of the tick's updates (section 1). Two frame data
  buffers alternate (`gfx_frame_index`); some state is double-buffered by it (spinners, light structs).
- **Where pictures-between-frames hook**: at the end of `Gfx_BuildFrameDisplayList`, the tick's
  recording is complete. Record, per draw: a stable id, the static DL (or mesh id) and the inputs that
  make its matrix; then present lerp(previous tick, this tick) at the display's rate. State to
  interpolate:
  - camera eye/at (+ height oscillation), fovy (snap on preset change), shake (keep per tick or smooth);
  - map objects: wave-object positions, TCB-animated object offsets, door slot offsets, spinner angle;
    billboards follow the interpolated camera yaw;
  - actors: x,y,z, rot (yaw), rotX, rotZ, scale, and the animation frame as a fraction (the TCB
    evaluator already divides `frame/len` in float, so a fractional frame is a small change); an
    animation switch (`animIndex` change, flag 0x400) or a dlSeq swap is a snap;
  - node matrices that aim at the eye (billboard nodes) recomputed from the interpolated camera;
  - sprites (x,y,z, scale, roll; slot reuse detected by life/template), streaks, rings (scale), model
    effects (position, frame, texture scroll), shadows (follow their owners);
  - 2D: fade alpha (ramp step), HUD meter widths and panel slide, compass yaw, world-anchored markers
    (project with the interpolated camera).
  Stable ids: MapObj pointer, Actor pointer, effect/sprite slot index + birth tick, node index.

## 5. HUD and 2D layout (320x240)

The 3D picture is 304x224 inside an 8-pixel border (scissor 8..312, 8..232). HUD positions (hud.c):

| Element | Position (x, y) | Anchor for widescreen |
|---|---|---|
| HP/MP meters panel | origin (28,24), ~62x35; slides in from the left (`hud_panel_transition_frame`) | top-left |
| Element levels (4 icons + numbers, max-level glow tris) | origin (28,198), icons every 22 px | bottom-left |
| Compass disc + N + target dot | origin (260,24), disc centre (277,41), r≈17 | top-right |
| Enemy info panel (battle) | origin (181,26), 110 wide | top-right |
| Spell input panel | origin (34,140), 110 wide, 4 rows | centre (R7a: its rows sit around the allocation cross) |
| Spell name label | origin (105,50), 110 wide (centred) | centre |
| Element allocation panel | origin (129,72), 64x80 (centred) | centre |
| Text windows | (34,148,252,68) bottom, (34,20,252,68) top, (40,30/150,240,52), choice (180,110,30,40) | centre |
| Talk/spirit/chest/sign/target markers, floating numbers, status indicators, out-of-range icon | `Camera_WorldToScreen` (math.c:417: x = 160 - x/w·160·0.75, y = 120 + 120·y/w) | world-anchored |
| Fade quad | ortho ±160 × ±120 | full width |
| Letterbox | fill 0..319 × 0..40 / 200..239 | full width |
| Pause menu, item/save menus, title, intro, ending, Pak manager | full 320x240 pictures and layouts | pillarbox (or extend backgrounds) |

Widescreen needs: the perspective aspect (camera.c:168) and `Camera_WorldToScreen`'s 160/0.75
(only x changes; y feeds logic through `hud_npc_marker_screen_y` and must keep its value); HUD elements
offset by their anchor (left items -(W-320)/2, right items +(W-320)/2 in a 320-high virtual space);
fade and letterbox to the full width; a decision for the 8-pixel border (modern default: full frame);
the billboard cone cull (below) widened.

## 6. Special things

- **Clock palette** (`Map_BlendPaletteByClock`, map.c:1003): 3 source palettes (`map_clock_tlut_srcs`,
  ROM 0xD86170) chosen by `env_clock` thresholds (`map_clock_tlut_schedule`), blended over 0x1C or 0x80
  ticks into `map_clock_tlut` (0x800810B0, 512 entries), which map CI textures load as their TLUT.
  Called inside the draw each tick.
- **Backdrop palette**: four day-phase palettes per backdrop (DMA'd), faded into `map_backdrop_tlut`
  (0x800806B0) by `Env_StartPaletteFade` / `Env_StepPaletteFade` over `fadeFrames`.
- **Palette rotations**: `Map_RotateHalfwordBuffer` rotates 200 entries of `D_80081AB0` every tick;
  `Map_StartFrame` rotates 18 halfwords at `D_80124E00` on map 29. Neither address appeared as a
  SETTIMG/TLUT source in the sampled frames (0x80124E00 is referenced from a pointer table in that
  block): find their reader before porting map 29.
- **Texture animation**: scroll by `SetTileSize` (scroll objects +2 t per tick; model effects s/t
  velocity), flipbooks (animtex billboards step `model`, model-effect frames by `age >> shift`, sprite
  templates by tex pointer, node `dlSeq`), env-colour alpha flicker on odd/even ticks (streaks, rings,
  model effects flag 2, arc sprites: 250/150 or 0xDC/0x78), TCB-animated nodes.
- **Per-frame CPU writes to textures**: only the palettes above. Item icons (item.c:320) and menu map
  pictures (menu.c:548) are DMA'd on demand; the port's English glyph pages are built once.
- **Lighting animation**: light slots (`Env_ApplyLightSlot`) step env-colour (additive flash) and light
  colour frame tables per tick; `env_light_slots[player].state = 2` brightens the player's lights;
  environment keyframes interpolate ambient, 3 light colours and fog over `env_light_time`.
- **Culling and draw distance**:
  - grid search around the **camera eye** (`MapObj_UpdateAndDrawNearby`): pattern 1 disc r7 (157
    cells), 2 disc r5 (89), 3 3x3, 4 disc r2 (21), 0 whole sub-area list. From the ROM
    (`mapstats.txt`): 28 of 36 maps use one list per sub-area (everything drawn); the fields (26, 27,
    29, 30, 35) use disc r7 of 100-unit cells (~700 units; map 29 200-unit cells, ~1400); 28 and 31-33
    disc r5; 9 disc r2 of 600-unit cells. Plus each grid's always-drawn list. `mapobj_*_list` hold 256
    objects each (more needs bigger lists);
  - billboards: < 1,500 units from the eye and |angle to camera yaw| < `camera_fovy` degrees (a
    horizontal cone of ±fovy around the object centre; fine for 16:9 (fovy 35: half-FOV 29°; fovy 25:
    21.5°), too narrow past an aspect of about 2.1:1, so 21:9 pops billboards at the edges);
  - NPCs: within 300 units of the player (`dist² < 90000`), point-sampled beyond ~141 units;
  - RSP `CULLDL` boxes in model lists; far plane 1000-2400; fog.
- **LOD**: none geometric (no BRANCH_Z, no mip-maps). The only "LOD" is the NPC filter switch above.
- **Data-driven quirks to keep**: `Enemy_DrawAll` draws flying enemies at a raised y (temporary
  write), `Anim_DrawNode` writes `anim_node_world_positions` that the player's shadow and floor probe
  use, `Hud_DrawNpcTalkMarker` writes the y that picks the text window.

## 7. The hard parts

1. **Draw = update.** Camera, animation frames, effects, fades, palettes, HUD timers and some RNG
   calls run inside the draw functions. The renderer must record once per tick and never call the
   draw functions twice; 240 fps comes from interpolating the recordings.
2. **Integer animation time and snaps.** Fractional frames are easy for TCB tracks, but anim switches,
   DL flipbooks, texture flipbooks and odd/even alpha flicker are discrete. Decide per effect: hold
   the tick value (N64 look) or average (modern).
3. **The N64 shading model** must be reproduced in shaders for the "compared against reference" goal:
   per-vertex F3DEX lighting (3 lights + ambient, one set camera-relative), texgen, 2-cycle combiner
   with the additive ENV flash and PRIM tints, FOG_SHADE_A fog from the depth-range fog position,
   CVG×alpha cutouts, decal Z, mirror/clamp/mask addressing, 3-point bilinear, palette textures with
   changing TLUTs.
4. **Aspect assumptions** in three places: the perspective (4/3), `Camera_WorldToScreen` (x only),
   the billboard cone cull. Plus the 8-pixel border and 320-wide full-screen pictures.
5. **Draw distance** is bounded by the grid patterns, the 256-entry object lists, the 2,560-command
   frame list and the 550-matrix pool (the last two disappear with a native renderer), and by the fog
   and far plane, which are per-map data.
6. **Translucency order** is the game's code order (no sorting); keep it.
7. **Data still in N64 form**: display lists, vertices, textures and TLUTs are big-endian RDRAM bytes
   at their N64 addresses; a mesh cache keyed on DL address must be invalidated on DMA over that range
   (map loads, enemy area sets at 0x8020E6F0, the 0x80200000 block).

## 8. Suggested porting order

Each step switchable and compared against the frame-image reference (paraLLEl-RDP, REFERENCE.md):

1. **Infrastructure**: a per-tick recording (camera, lights/fog, ordered draw list with ids), a
   static-DL → mesh converter (F3DEX vertex/tri/texture/state walk, as census.py does) with a cache keyed
   on address + DMA invalidation, and a shader that implements the handful of combiner/blender cases
   above. Present the recording at the tick rate first.
2. **Map geometry** (static objects, then spinners/waves/TCB objects, translucent and scroll objects):
   most of the triangles, rigid, one matrix each. Clock palette as a palette texture.
3. **Backdrop** (one cylinder, one palette): small, sets up the 1-cycle no-Z path.
4. **Actors** (player, NPCs, chests, enemies, bosses): one hierarchy walker shared by all, including
   billboard nodes and dlSeq. Fractional animation frames here.
5. **Shadows**, then **billboards / animtex billboards** (camera-facing, cone cull).
6. **Camera interpolation and pictures between frames** for steps 2-5 (the bulk of the screen).
7. **Effects**: sprite pool, streaks, rings, model effects (texgen, scrolling, flipbooks), stat markers.
8. **2D**: texrect batches (text, HUD, menus, title, intro, ending), fade, letterbox, compass; then
   widescreen anchoring and world-anchored markers from the interpolated camera.
9. **Pak manager** as a texture, cutscene image cues.
10. **Modern options**: widescreen aspect, no border, draw distance (bigger grid search + lists,
    far plane and fog), always-bilinear NPCs, MSAA, any resolution.

## Reproducing

All in `C:\Users\Sean\AppData\Local\Temp\claude\c--code\e28e4613-47ce-4c8d-9eb8-47918177bdc8\scratchpad\rsurvey\`:
- `runall.sh`: the four headless runs (copy of eltale-p1.exe, each in its own folder so
  `out/frame_N.bin` lands there): New Game (`tests/title-new-game.input`, dumps 20+40), map tour
  (`ELTALE_MAP_TOUR=1`, 100+300), ending (`ELTALE_START_MODE=5`, 30+200), battle (copy of
  `out/bugsaves/battle-stuck`, `ELTALE_QUICK_LOAD_AT=100 ELTALE_BATTLE_LOG=2`, 100+40). The menu run used
  `s_menu/menu.input` with the same quick save (dumps 120+15).
- `census.py`: F3DEX 1.23 walker and feature census (`python census.py out/frame_*.bin --json x.json`);
  `show.py` prints a JSON section. Results: `census_{ng,tour,ending,battle,menu,all}.json`.
- `mapstats.py` → `mapstats.txt`: per-map grid patterns, object kinds, camera presets, fog, from the ROM.
- `toplen.py`, `scan_*.py`: top-level list length; searches for address references.
The 8 MiB frame dumps were deleted after the census.
