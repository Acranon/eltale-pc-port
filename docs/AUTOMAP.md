# The area map (automap)

A PC overlay that shows the floor plan of the place the player is in, in the style of the maps of the King's Field
ports (KF1, KF3, KF4): a full map on a key and a minimap in a corner. It is not the game's own map in its pause menu
(that one is untouched). This first pass draws the area accurately: the true floor plan, the walls, water, the
player's place and facing, in every area of the game. Marks, hints, icons and fog of war come later; the data
already has room for them (see "Next").

## Using it

- **M** (the Controls page's "Area map (open/close)", rebindable) opens and closes the full map; **Esc** closes it
  too. The wheel zooms (x1.25 a notch, 1x to 24x) about the point under the cursor, a left drag moves it, a right
  click fits it to the area again. It opens fitted to the floor the player can walk to.
- The game is **not** paused while the map is open (a switch on the F1 menu's Map page). The keyboard and the pad
  still drive the game; the mouse belongs to the map (no mouse look, the wheel does not zoom the camera).
- The **minimap** (on by default) sits in a corner of the game's picture, north up, centred on the player.
- The F1 menu's **Map** page: pause while the map is open (off), the map's background (92%), the minimap on/off,
  its corner (bottom right), size (24% of the picture's height), reach (800 units from its centre to its edge) and
  opacity (85%). settings.json:

```json
"map": { "minimap": true, "minimap_corner": 1, "minimap_size": 24, "minimap_reach": 8, "minimap_opacity": 85,
         "background": 92, "pause_game": false }
```

### What it shows

| | |
| --- | --- |
| Floor | warm grey; lighter above the player's height and darker below it (smoothly: x1.25 at 300 units up, x0.6 at 300 down), shaded by its slope (lit from the upper left), so hills, ramps and stairs read as shapes |
| Out of reach | dark grey, faint: floor the player cannot walk to from where they are (the slopes and fields beyond a town's walls, the scenery of an area that shares its geometry with another) |
| Water | blue: the wave, translucent and scrolling-texture objects (the sea, rivers, ponds) where they are seen from above |
| Wall | light lines: the wall segments the player is pushed out of |
| Ledge or drop | thin darker lines: where a walkable floor ends with no wall (an edge, a step between levels) |
| You | the light dart with a black outline, pointing where the player faces |

North (+z, the compass's N) is up; the title says "Map M, area S" (the game's map and sub-area numbers; names
come with the marks pass).

## How it works

### The feed (game side, read-only): game/native_draw/nd_automap.c

At the end of each tick (the graphics task's start, right after the native renderer's `nd_frame_end`, through
port/gfx/host_bridge.c) the feed reads the game's own state and hands it to the host (`ElHost_AutomapArea`,
`ElHost_AutomapPlayer`; the structs are in port/include/port/automap.h). It never writes game state, so every
check's hashes are unchanged, and nothing from the ROM is stored in the repository: it is all read at run time.

- **The plan**, once per sub-area (when `map_id`, `map_subarea`, the grid or the object array change; again if a
  collidable object moves):
  - **Floor**: the collision triangles the player's own floor query can land on. `Coll_ResolveFloorAndWalls`
    tests the map objects of the sub-area's grid (`map_subarea_grid`: each cell's list and the always-tested list),
    objects of kind < 0x10 with a collision mesh (`MapModel.tris/verts`), triangles whose flags share a bit with
    the query (the player's is 0xA01: triangle flags 0x200 or 0x800). Each triangle goes to world space the way
    `Coll_TestObjectFloor` transforms the query (world = object position + R^T * vertex * scale, R the object's
    turn about y; y = vertex y * scale + object y). Each keeps its object's index (`obj`, the id for hover later)
    and flags (bits 1-3 the camera preset, 6-8 the floor effect: no area uses an effect).
  - **Walls**: `map_actor_walls[map_subarea]`, the polylines `Coll_PushFromWalls` pushes actors out of.
  - **Exits**: `map_subarea_def->exits` (box, angle, destination, item condition): carried for the marks pass, not
    drawn yet.
  - **Water**: the triangles the native renderer draws for the wave (kinds 1-3), translucent (0x7F, 0x80) and
    scrolling-texture (0x40-0x44) objects (the cached mesh's positions, `nd_model_positions`), placed by the same
    transform as `guPosition`.
- **The player**, every tick: x, y, z, `actor.rot` (facing (sin rot, cos rot) in x, z), whether the game is in play
  (gameplay mode, no area transition) and in a battle, and the tick's graphics task number.

The game's floor has no step limit: the player stands on whatever floor the query finds under them, at any height,
and only walls keep them in. So "walkable" is a question of walls, which is what the host works out.

### The host: port/host/automap.cpp

- **Reachable floor**: the floor is rasterised into a grid of 3-unit cells (more on the biggest maps, at most about
  4 million cells), the wall segments into blocked cells (densely sampled, so a 4-connected fill never slips
  through), and a fill from the player's position (and from just before each exit that wants a facing) marks the
  cells walked to; a triangle with a reached cell is reachable. Done once per area, when the player is first seen
  in it. The full map fits the reachable floor.
- **Water where it is seen**: the water's surface is rasterised into the same grid; a cell is drawn as water when
  no floor is above the surface there and either the cell is open (reached from the grid's edge without crossing
  floor: the sea, a lake) or the water object is small next to the area (a pond, a river). So the sea mesh that runs
  on under an island, or under a castle's footprint, does not show through.
- **Edges**: triangle sides no other floor triangle shares (at the same height), of reachable floor.
- **Drawing**: ImGui's background draw list, under the F1 menu (the same ImGui frame the menu uses, so it costs no new
  GPU pipelines). The floor triangles are written straight into the draw list (lowest first, so a higher floor covers
  a lower one) with a colour per corner; water as runs of grid cells; lines anti-aliased. Every size is a fraction
  of the game picture's height (the picture's rectangle comes from the renderer: `Renderer::picture_rect`), so it
  looks the same at any window size, 4:3 or 16:9, windowed or full screen; the minimap sits in the picture's corner,
  below the compass or the meters when it is at the top.
- **The arrow between ticks**: the renderer notes which two recordings (by the game's graphics task number) its last
  picture was made from and how far between (`NativeDraw::shown_frames`); the player samples carry the same
  numbers, so the arrow is placed (and turned, the short way round) as the picture places the player, at any frame
  rate up to 240. The overlay is built before the picture it goes with, so it follows the picture before it (a
  quarter of a tick at 240 pictures a second). A jump of more than 200 units (a warp) is not slid across.
- **Pause**: with the switch on, the game waits in the host while the full map is open, as it does under the F1
  menu (`menu_pause`). Off by default: the game makes its own music, so a paused game is a silent one; KF1's map
  pause kept the music playing, which this game cannot do while it waits.

## Accuracy (how it was checked)

- **The truth view** (`ELTALE_AUTOMAP_TRUTH=1`, a test): the native renderer draws the sub-area's real geometry from
  straight above. The camera the renderer gets is replaced by a nearly orthographic one (a perspective from 100,000
  units up: 1% off at the ends for 1,000 units of height) framing the plan's bounds, north up; every map object of
  the sub-area is drawn (the draw-distance pass widened to the whole grid); no fog, sky or 2D. The game itself sees
  nothing of this (only the recording's camera changes).
- **The shots** (`ELTALE_AUTOMAP_SHOTS=1` with `ELTALE_PRESENT_DUMP`, in a hidden window): in each sub-area the
  truth view is on for a stretch, and the host writes `am_MM_SS_truth.png` (the truth view), `am_MM_SS_over.png`
  (the map's walls in magenta and floor edges in yellow drawn over the truth view, framed the same, with the arrow)
  and `am_MM_SS_map.png` (the full map as the player opens it), and `am_MM_SS_plan.json` (the walls, the bounds).
  With `ELTALE_MAP_TOUR=1` that is every area the tour visits. `tools/automap_sheet.py SHOTS OUT` makes a contact
  sheet per map (a row per sub-area: truth, over, map) and an index.html.
- **The player's place**: in the over pictures the arrow sits on the player's model in the truth view.
  `ELTALE_AUTOMAP_TRACE=1` prints the player's position every tick (P1_AUTOMAP_POS); `tools/automap_path.py LOG SHOTS`
  draws the path over the over picture and counts steps that cross a wall segment of the plan (0: the game's wall
  push and the plan's walls agree). tests/automap-walk.input walks the user's compass-spirit quick save.

Results (2026-11-01, the map tour, 1280x960): pictures of 257 of the 258 sub-areas (map 23 area 22: the tour's
player is left standing in that area's exit and walks straight on into area 23, so it is never in play there).
The walls lie on the rendered walls, fences, houses and corridors everywhere; the walk test's 848 ticks cross no
wall.

### Limits seen

- **Sub-areas that share a grid** (map 3 areas 0 and 1, map 7, map 12, ...) have the same floor and different walls:
  the map draws all the floor, and what the walls of this sub-area shut off is out of reach.
- **Reachable floor needs a seed**: before the player is seen in an area (and in the test pictures, which seed from
  the exits alone because the tour leaves the player where the last area had them) the reach can be partial.
- **Lava and other hazards** are not marked: no floor in the game has a floor-effect flag; the red pools of map 29
  are out of reach behind walls and show as such.
- **Moving floors**: a collidable object that moves sends the plan again; none does in the tour.
- **Overlapping floors** (a bridge over a path) are drawn lowest first; the game takes the first triangle its grid
  lists, not the highest, which can differ where floors overlap.
- Indoors and outdoors are drawn alike (the plan has `EL_AM_AREA_SKY` for a later look of its own).

## Tests

- `python tools/automap_test.py --exe out/p1-windows/eltale-p1.exe --rom ROM` (hidden window, sound off): the full
  map opened, zoomed, dragged and fitted with SDL events (tests/automap.events) after a quick load, in a 4:3 and a
  16:9 window (`ELTALE_AUTOMAP_DUMP`: a picture per view, automap_full_N.png, and the minimap's), the same walk
  without the map (tests/automap-none.events: the player's positions must be the same: the map changes nothing),
  and the tour's first areas' shots. The full-map pictures (background 100%) are hashed against
  tests/automap_hashes.txt (`--update` writes it). Prints AUTOMAP_TEST_OK.
- `ELTALE_AUTOMAP_LOG=1` prints each plan (P1_AUTOMAP plan: triangles, walls, exits, bounds, heights, flags) and the
  object kinds of the sub-area.

## Next (the marks pass)

- **Markers** (`ElAmMarker`: id = kind << 24 | the game's own index, kind, flags, place, label) for exits and doors
  (the exits are already in the plan), chests, spirits, people, save spots; hover labels as in KF1/KF4 (the floor
  triangles carry their object's index for hover too).
- **Fog of war**: left out of this pass on purpose (accuracy first: the whole area is drawn). When it comes, explored
  cells on the reach grid, kept per save (KF4's per-save files are the model: live, quick save and Controller Pak
  copies).
- Place names for the title (the save menu's map labels), a look of its own for outdoor areas.
