# The area map (automap)

A PC overlay that shows the floor plan of the place the player is in, in the style of the maps of the King's Field
ports (KF1, KF3, KF4): a full map on a key and a minimap in a corner. It is not the game's own map in its pause menu
(that one is untouched). It draws the area accurately: the true floor plan, the walls, water, the player's place and
facing, in every area of the game; the doors and ways out, and the people, where they are now; the inns, the
Traveler's Wings and the bread givers and the doors to them, the people with a story line, the signs; and, as cheats,
the chests, the spirits, the bosses, the story item givers, the battle zones and a danger meter. Pointing at a mark
names it. Fog of war comes later (see "Next").

## Using it

- **M** (the Controls page's "Area map (open/close)", rebindable) opens and closes the full map; **Esc** closes it
  too. The wheel zooms (x1.25 a notch, 1x to 24x) about the point under the cursor, a left drag moves it, a right
  click fits it to the area again. It opens fitted to the floor the player can walk to.
- The game is **not** paused while the map is open (a switch on the F1 menu's Map page). The keyboard and the pad
  still drive the game; the mouse belongs to the map (no mouse look, the wheel does not zoom the camera).
- Point at a mark on the full map: its name in a box beside it (a person's name, what a chest holds, where a door
  leads and what a locked one needs).
- The **minimap** (on by default) sits in a corner of the game's picture, north up, centred on the player. Indoors
  (an area with no sky: houses, inns, castle halls, caves) it has its own reach, and a room smaller than that reach
  fills the minimap and stays put while you walk in it (the whole room, its doors and its people); a bigger hall or
  a dungeon follows the player at the indoor reach. **`]` / `[`** (the Controls page's "Minimap zoom in/out") or the
  **wheel over the minimap** (when the mouse is not turning the camera) zoom it while you play, x1.25 a step: that
  changes the reach of the kind of place you are in (indoors or outdoors), kept in settings.json; the reach shows
  on the minimap for a moment ("the whole room" when a room is fitted).
- The F1 menu's **Map** page: pause while the map is open (off), the map's background (92%), the marks (doors and
  ways out, people, inns/Wings/food, story people, signs: all on), the minimap on/off, its corner (bottom right),
  size (24% of the picture's height), reach outdoors (800 units from its centre to its edge, 30 to 3000), reach
  indoors (150, 20 to 1500), fit small rooms (on), opacity (85%), and the **Cheats**: show chests, show spirits, show
  bosses, show story item givers, the battle zones button, the danger meter (all off). settings.json:

```json
"map": { "minimap": true, "minimap_corner": 1, "minimap_size": 24, "minimap_reach_outdoor": 800,
         "minimap_reach_indoor": 150, "minimap_fit_rooms": true, "minimap_opacity": 85, "background": 92,
         "pause_game": false, "doors": true, "people": true, "services": true, "story": true, "signs": true,
         "cheat_chests": false, "cheat_spirits": false, "cheat_bosses": false, "cheat_givers": false,
         "cheat_zones": false, "cheat_danger": false }
```

- **Battle zones** (cheat): with "Battle zones button" on, the full map of an area with random battles has a
  "Battle zones (hold)" button at its bottom. While it is held with the left button, or the `map_zones` key (B,
  Controls page) is held, the zones are shaded (translucent red, a red outline; on the minimap too while the key is
  held); let go and they hide. Pointing at a shaded spot names it: "Battle zone 2 of 4" and its monsters by group
  ("Were-Rabbit x1-3 / Hellhound x1-2": a group's kinds and how many of each, the groups the game picks from).
- **Danger meter** (cheat): a bar against the minimap (above it in a bottom corner, below it in a top one; in the
  minimap's corner when the minimap is off). It reads "Battle 12% next step": the chance that the next 50 units
  walked start a battle, (battle_encounter_roll_limit + 50) / 2000, capped at 100%, as a green-to-red bar, with a thin
  strip under it filling as you walk toward that roll (battle_travel_accumulator, 0 to 50). Where no battle can start
  (outside the zones, in a town, while a battle is on or the game stops the roll) it says "Safe here" (with the chance
  the roll would have in a zone: the game rolls everywhere in an enemy area, and a lost roll outside a zone still
  raises the limit). The strip slides with the pictures between ticks (a roll wraps it to empty) and the bar eases
  to a new chance in about a tenth of a second, at any frame rate up to 240. Placed by the minimap because that is
  where the eye is while walking, and the full map covers the game anyway.

(v1's `"minimap_reach": 8`, hundreds of units, is read as the outdoor reach.)

### What it shows

| | |
| --- | --- |
| Floor | warm grey; lighter above the player's height and darker below it (smoothly: x1.25 at 300 units up, x0.6 at 300 down), shaded by its slope (lit from the upper left), so hills, ramps and stairs read as shapes |
| Out of reach | dark grey, faint: floor the player cannot walk to from where they are (the slopes and fields beyond a town's walls, the scenery of an area that shares its geometry with another) |
| Water | blue: the wave, translucent and scrolling-texture objects (the sea, rivers, ponds) where they are seen from above |
| Wall | light lines: the wall segments the player is pushed out of |
| Ledge or drop | thin darker lines: where a walkable floor ends with no wall (an edge, a step between levels) |
| Door | an orange bar across the doorway, where the game's door object stands (the door the game swings or slides open) |
| Door to another area | the bar with a white arrow pointing the way out |
| Locked | red (a door or a way out that needs an item you have not got; the label names it); grey: shut because you hold its item |
| Way through | a faint white box, the exit's own trigger box, with an orange arrow the way through (stairs, a passage) |
| Way to another area | the box with a white arrow (a road's end, a cave mouth) |
| Person | a pink disc with a white rim, where they stand now (moving with them, between ticks too) |
| Inn, Wings, Food | the person's disc with a badge: a teal disc with a crescent moon (an innkeeper: rest and save), a sky-blue one with wings (gives Traveler's Wings when you have none), a wheat one with a loaf (gives bread when you have none) |
| Door to a service | the door's bar (or a way through's box) with a halo in the service's colour and its badge on this side; its label names the service ("Inn", "Traveler's Wings", "Food") |
| Story line | a light blue ring round the person's disc |
| Sign | a small signpost; its label is "Sign: " and the text's first line |
| Story item giver (cheat) | a gold ring round the person and a gold key on the disc; the label names the item |
| Battle zone (cheat, held) | translucent red with a red outline: where a random battle can start |
| Chest (cheat) | a gold box; opened: its outline |
| Spirit (cheat) | a violet four-pointed sparkle; taken: its outline |
| Boss (cheat) | a red diamond, and a faint red ring: how near you can go before it fights |
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

- **The marks**, every tick (`ElAmPlayer.markers`, `ElAmMarker`, automap.h v2: id = kind << 24 | the game's own index,
  kind, flags, place, an exit's box, a boss's reach, the item, the destination, an English label):
  - **Doors and ways out**: `map_subarea_def->exits` (MapExit). Its flags' bits 8-11 are what `Player_StartAreaExit`
    does: 2 and 3 swing the exit's door object (`map_objs[mapObj]`, `MapObjAnim_Start`; 3 the double doors and big
    gates), 4 slides it, 0 and 1 walk on with no door (bit 1: from any side). A door's mark is the door object's place
    taken along the exit's way through (the object stands at one end of the doorway, the exit's box is in front of
    it; the doorway's half width is the box's), so it sits on the wall line. The item test is `Exit_IsEnabled`'s:
    0x10 needs `MapExit.id` in `item_inventory` (locked until then), 0x20 shuts once it is held.
    **Another area**: the destination is another map, a field or dungeon (an enemy area: sub-area flag bit 0) is on
    one side, and neither side is a building (no sky and no enemies: a house, an inn, a castle hall). So a town's
    gate, a field's road, a cave mouth are "to another area"; a house's or an inn's door (even an inn standing in a
    field), a castle's doors, the stairs between rooms are not. The destination's flags are read from the ROM image
    (`map_area_defs[map]`: the area's block is the ROM from romStart, loaded at dst; its sub-area record's flags and
    its env settings' `mapFlags` bit 2, the sky), as that map is not loaded.
  - **People**: `npc_slots[0..npc_count)` (the sub-area's NPCs: `Npc_InitArea` spawns those whose `NpcDef.area` is
    this sub-area; none in an enemy area), at `actor.x/y/z`. The name is the speaker name (`NpcDef.name`, the
    Japanese bytes in RDRAM) looked up in the port's English script (text/en.tsv, `Port_TextPlain`, en_text.c's
    table by the hash of the Japanese string, whatever language the game's text is in); "Person" if it has none.
  - **Chests**: `chest_objects[0..chest_count)` (`Chest_LoadForMap`: per map `chest_map_placements`, per sub-area a
    `ChestGroup`); opened: `Chest_IsOpened(floorFlags)` (= `ChestDef.flagId`, a bit of `chest_opened_flags`, set by
    `Chest_Open`, kept in saves). What it holds: `ChestDef.animSetIndex` (the actor's), the item id
    `Player_StateOpenChest` adds; its name from the item name table `D_803AB288[item]` (RDRAM, big-endian pointers)
    through the English script.
  - **Spirits**: `spirit_objects[0..spirit_count)` (`Spirit_LoadForMap`, `spirit_map_placements` by map and sub-area);
    taken: `Spirit_IsCollected(flagId)` (`spirit_collected_flags`, set by `Spirit_SetCollected` when you take one).
    A spirit raises the element the player picks there, so the game has no element for a spirit spot: the label says
    "raises an element of your choice".
  - **The boss**: `boss_field_record` while `boss_field_index` (`Boss_LoadForArea`: the boss of this map and sub-area,
    not yet beaten); the fight starts within `D_803AB48C[index + 1]` units (`Boss_CheckPlayerNear`); its name from
    `D_803AC418[index]` (the speaker name `Boss_BeginEncounter` shows) through the English script.
  - **Services** (v3): a person's `NpcDef.talk_kind` and item. Talk kind 6 is the innkeeper (`Npc_StartTalk` offers
    the rest choice, `npc_rest_choice_row0/1`; resting at an inn is where the game saves). Talk kind 2 gives
    `NpcDef.u6.b.item_id` whenever the player holds none of it (`Item_CountInInventory == 0`, then
    `Item_AddToInventory`; otherwise the "you already have one" line): which item it is decides the service, from
    the item's own definition (`D_803AAAA0[item]`, the ItemDef table, swapped to native): handler 2 is
    `Item_UseWarp`, the **Traveler's Wings** (items 14-19, the six town Wings); handler 1 is `Item_UseRestore`, and
    with HP to give (`unk4`, items 0-3) it is **food** (the Honey Bread of the Darlam tavern and the trainee's
    friend, and one 150 HP gift). An MP restorer (handler 1 with only `unk6`: Mint Leaves, Morning Dew, Smelling
    Salts) is not marked (see "Other things"). Any other item (handler 0: no use) is a story key item: the
    **story item giver** cheat (Shannon's Book of Ether and Key to the Dark Prison). Found in the tour: 17
    innkeepers, 6 Wings givers, 3 food givers, 5 MP-remedy givers, 2 story item givers.
  - **Doors to a service** (v3): for each exit whose destination is a building (no sky, no enemies: `dest_info`),
    the destination's people are read from the ROM image at run time as the destination's flags are (that map is not
    loaded): `map_area_defs[map].npcList` (an N64 address in the area's ROM block: u16 count, the NpcDefs' address),
    each 0x2C-byte NpcDef with `area` (u16 at 0) the sub-area, `talk_kind` (u16 at 2) and the item (byte 7), and the
    services above. Then a bounded search one and two doors further in: the destination sub-area's exits (its
    MapSubareaDef at `subareas + sub * 0x18`: the exits' address at 4, their count at 8; each MapExit's destination at
    0x1E) that lead to another building of the same map (not back to the room the door is in), up to two hops and
    16 rooms. The door's `services` are the bits (`EL_AM_SVC_BIT`) of all of them, so a street door to an inn whose
    keeper stands in the inner room still gets its halo. Exits to a street or a field are not service doors (a house's
    way back out does not light up for the town's people). Cached per (map, sub-area).
  - **Story people** (v3): talk kind 5 (`Npc_StartTalk`: a line that changes once a boss is beaten): 56 in the tour.
  - **Signs** (v3): `textspot_list[0..textspot_count)` (`TextSpot_LoadForMap`: the sub-area's entries of
    `textspot_table`), at `TextSpotDef.x/z`, id = the `textspot_table` index. The text is read where
    `TextSpot_LoadMessage` reads it, the ROM text block `D_D37700 + textOffset`, through the English script; the
    label is "Sign: " and its first line ("Sign" when it has no English).
  - **Random battles** (v3, in the plan: `ElAmArea.zones`, `points`): in an enemy area, `Battle_ResetState`'s
    tables, `map_area_def->encounterRegions[map_subarea]` (each EncounterRegion: x, z, x_extent, z_extent and its
    group list, a u16 count and group ids) and `encounterGroups[map_subarea].points`. Each region is named by its
    groups: each group's entries (`EncounterGroup.entries`: enemy def index, count, extra count), the enemy area
    set's def (`enemy_area_set->defs`, loaded for the sub-area) and its name (`enemy_names[def->anim_set_index]`)
    through the port's monster names (`Port_TextMonsterPlain`, English whatever the text language).
  - **The roll** (v3, every tick in `ElAmPlayer`): `battle_travel_accumulator`, `battle_encounter_roll_limit`, and
    whether the roll runs (`EL_AM_DANGER_ROLLS`: an enemy area, no battle on, `battle_flags` 0x8000 clear, the
    player's `actor.animFlags` bit 0 clear) and whether a battle could start where the player stands
    (`EL_AM_DANGER_ZONE`, the rule below at the player's place, which is where the next tick's roll looks: the roll
    runs at the start of the tick, before `Player_Update`).
  Marks are built in play and through the fade of a way out (while the sub-area is still the plan's). Every mark has
  a stable key, (map, sub-area, id), id = kind << 24 | the game's own index (an exit's index in the sub-area's
  exits, a person's place in the sub-area's NpcDef order, a chest's or spirit's slot, a sign's `textspot_table`
  index): the same each visit, so a later fog of war can keep "discovered" bits per save (see "Next").

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
- **Marks**: KF3's and KF4's icons and palette (door orange, locked red, person pink, chest gold, way out white), drawn
  over the floor and walls (ways out first, then doors, chests, spirits, people and the boss on top), sized by the
  zoom within limits. People and the boss slide between the two recordings a picture is made from, as their models
  do. The full map's legend lists the marks the area has. Hover: the nearest mark within reach of the cursor (a
  door's bar or an exit's box under it counts; people win over what they stand on), its label in a box (KF4's).
- **The minimap's view** (`minimap_view`): indoors (`EL_AM_AREA_SKY` clear) the indoor reach; with "fit small rooms"
  a room whose reachable floor (half its larger side, +8% and 6 units) fits in that reach is shown whole, centred on
  the room (houses and inns are 60-100 units across, castle rooms 50-300). Outdoors the outdoor reach, centred on the
  player. The zoom keys (input.cpp `ACT_MAP_ZOOM_IN/OUT`) and the wheel over the minimap call `minimap_zoom`.
- **Battle zones** (v3): the game's rule, `Battle_TryStartRandomEncounter`: each 50 units walked the limit grows by
  50 and if `Math_Rand(2000) <` the limit, a battle starts only if `Battle_ChooseNearbyEncounterPoint` finds an
  encounter point at squared distance 2500..8100 (50 to 90 units) from the player (it stops looking at 6 found,
  which does not change whether there is one) and `Battle_FindEncounterRegion` finds a region holding the player
  (x0 <= x, z0 <= z, x - x0 <= x_extent, z - z0 <= z_extent: the first in the list wins, so its groups are the
  battle's). The host works it out on the reach grid (3-unit cells), on reachable floor only: a cell is shaded when
  some point of it can pass the rule, that is when the cell's nearest and farthest distances from some encounter
  point meet 50..90 (the distance is continuous over the square) and the cell touches a region; its zone is the first
  region holding its centre, else the first it touches. So every place a battle can start is shaded, and at most a
  cell (3 units) more. Drawn as runs of cells and an outline of the cell sides where the zone changes. A first try
  that tested only the cells' centres left 4 of 64 battle starts just outside the shading (the rule held at the
  player's exact place, not at the centre of the cell).
- **Pause**: with the switch on, the game waits in the host while the full map is open, as it does under the F1
  menu (`menu_pause`). Off by default: the game makes its own music, so a paused game is a silent one; KF1's map
  pause kept the music playing, which this game cannot do while it waits.

## Accuracy (how it was checked)

- **The truth view** (`ELTALE_AUTOMAP_TRUTH=1`, a test): the native renderer draws the sub-area's real geometry from
  straight above. The camera the renderer gets is replaced by a nearly orthographic one (a perspective from 100,000
  units up: 1% off at the ends for 1,000 units of height) framing the plan's bounds, north up; every map object of
  the sub-area is drawn (the draw-distance pass widened to the whole grid); no fog, sky or 2D. The game itself sees
  nothing of this (only the recording's camera changes).
  In the truth view every person is drawn, not only those within 300 units of the player (`Npc_DrawAll`, a test
  only: patches/p0.patch), so their marks can be checked.
- **The shots** (`ELTALE_AUTOMAP_SHOTS=1` with `ELTALE_PRESENT_DUMP`, in a hidden window): in each sub-area the
  truth view is on for a stretch, and the host writes `am_MM_SS_truth.png` (the truth view), `am_MM_SS_over.png`
  (the map's walls in magenta, floor edges in yellow and every mark, cheats too, named, drawn over the truth view,
  framed the same, with the arrow), `am_MM_SS_map.png` (the full map as the player opens it), `am_MM_SS_mini.png`
  (the minimap as the player sees it, with `am_MM_SS_mini.json`: where it is) and `am_MM_SS_plan.json` (the walls,
  the bounds, the over picture's marks). With `ELTALE_MAP_TOUR=1` that is every area the tour visits.
  `tools/automap_sheet.py SHOTS OUT` makes a contact sheet per map (a row per sub-area: truth, over, map, minimap),
  a close-up sheet of every mark per map (`marks_MM.png`: the truth view and the over picture side by side around
  each mark, labelled) and an index.html.
- **Battle zones against the game** (v3): `ELTALE_AUTOMAP_BATTLES=1` prints each random battle's start: the game
  side (P1_AUTOMAP_BATTLE: where the roll found the player, the last tick's place, and the region the rule gives
  there) and the host (P1_AUTOMAP_BATTLE_CHECK: whether that place is in a shaded cell, INSIDE; within a cell of
  one, EDGE; else OUTSIDE). `ELTALE_MAP_GOTO=map,sub,seed` (with ELTALE_MAP_TOUR, a test) starts a New Game and
  takes the tour's way to that sub-area, where the tour's pad wanders (a new heading every 400 VIs, the seed picks
  the order; -1: it stands still, as the house minimap test has it) and fights battles with spells.
- **The player's place**: in the over pictures the arrow sits on the player's model in the truth view.
  `ELTALE_AUTOMAP_TRACE=1` prints the player's position every tick (P1_AUTOMAP_POS); `tools/automap_path.py LOG SHOTS`
  draws the path over the over picture and counts steps that cross a wall segment of the plan (0: the game's wall
  push and the plan's walls agree). tests/automap-walk.input walks the user's compass-spirit quick save.

Results (2026-11-01, the map tour, 1280x960): pictures of 257 of the 258 sub-areas (map 23 area 22: the tour's
player is left standing in that area's exit and walks straight on into area 23, so it is never in play there).
The walls lie on the rendered walls, fences, houses and corridors everywhere; the walk test's 848 ticks cross no
wall.

Marks (2026-11-01, the map tour with the cheats on): 539 exits (404 doors, 135 ways out; 45 to another area), 161
people, 88 chests, 98 spirits and 6 bosses in the 255 sub-areas pictured. In the close-ups the people's discs sit on
their models (every person drawn), the chests on the chest models, the spirits on their sparkles (when one is
showing), the bosses on their models, and the door bars on the doors: the castle's green door plates, the dungeon
corridors' end doors, the town gates, the fields' gates. A town house's door is under its roof from above: its bar sits
on the house's wall line where the door object stands, next to the exit's box the player walks into.

Battle zones (2026-11-01): the zones drawn over the truth view for every enemy sub-area of the tour (29 sub-areas,
74 regions: the fields of maps 2, 3, 5, 7, 9, 11, 12, 26-29, 31-33 and the dungeon floors of maps 30 and 34; the
contact sheets' zones.png). Then 64 random battles started by the game itself, the player wandering from the first
entry point of each of 24 of those sub-areas (`ELTALE_MAP_GOTO`, three headings orders each; the other five
sub-areas had no battle before the run ended) and 31 more from the user's field save (battle-stuck, quick loads in
turn and a long walk): every one started in a shaded cell (P1_AUTOMAP_BATTLE_CHECK INSIDE: 95 of 95), and the
game's own region (P1_AUTOMAP_BATTLE rule_zone) was the zone shaded there each time.

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
- Indoors and outdoors are drawn alike on the full map (the minimap's reach is their own).
- The tour leaves the player where the last area had them, often off the floor; its minimap pictures are not where a
  player would stand (the tests' indoor pictures are: New Game's throne room, the map-18 inn).

## Tests

- `python tools/automap_test.py --exe out/p1-windows/eltale-p1.exe --rom ROM` (hidden window, sound off): the full
  map opened, zoomed, dragged and fitted with SDL events (tests/automap.events) after a quick load, in a 4:3 and a
  16:9 window (`ELTALE_AUTOMAP_DUMP`: a picture per view, automap_full_N.png, and the minimap's), with the cheats on
  and the mouse put on a person (`ELTALE_AUTOMAP_HOVER=person|door|exit|chest|spirit|boss`: the first such mark is
  pointed at), then `] ] [` zoom the minimap (`P1_AUTOMAP_ZOOM`: 640, 512, 640 units); the same walk without the
  map and its keys (tests/automap-none.events: the player's positions must be the same: the map changes nothing);
  the tour's first areas' shots (cheats on); and the minimap indoors: New Game's throne room and the map-18 inn
  (saved by the tour with tests/tour-save.input, then the title's Continue, tests/automap-continue.input;
  `ELTALE_AUTOMAP_MINIMAP_AT=18,7` takes the picture there; `P1_AUTOMAP_MINIMAP ... indoor=1 fitted=1`, the
  minimap's square hashed at opacity 100, where it is solid), and the first town's Wings giver's house
  (`ELTALE_MAP_GOTO=15,11,-1`); each prints the room's share of the minimap (85-86%: a fitted room should fill
  80-90%). (v3) The battle zones (tests/automap-zones.events after the quick save battle-stuck, a field): the full
  map's "Battle zones" button held and let go, the B key held and let go (`P1_AUTOMAP_ZONES held=1 by=button`, ...,
  a picture each), the mouse on a zone (`ELTALE_AUTOMAP_HOVER=zone`), the danger meter (`P1_AUTOMAP_DANGER`), and
  the same walk without the map (tests/automap-zones-none.events: the same positions); and from the tour's plans,
  the first town's doors to its inn and Wings giver, the second town's door to food, the field's sign and its five
  shaded zones. The pictures (full map at background 100%) are hashed
  against tests/automap_hashes.txt (`--update` writes it). Prints AUTOMAP_TEST_OK.
- `ELTALE_AUTOMAP_LOG=1` prints each plan (P1_AUTOMAP plan: triangles, walls, exits, bounds, heights, flags), the
  object kinds of the sub-area, and its marks' sources: every exit (box, flags, item, door object, destination), the
  counts (people by talk kind, chests, spirits, boss, text spots, enemy area, encounter regions), each person
  (talk kind, movement, item, English name), chest (flag, item, opened), spirit, boss and text spot.

## Other things the game data could mark (not drawn: for the owner to decide)

- **MP remedy givers** (5: talk kind 2 with an item that restores MP only, `D_803AAAA0[item].unk6`: Mint Leaves
  from Prude in the castle and Garita, Morning Dew from Marble, Smelling Salts from Rhett and Fackel): the same
  "when you have none" gift as the bread, in the same towns. A clear candidate for a fourth service ("Remedies") or
  for the shops later; not marked (the Map page's services are the inn, the Wings and food).
- Nothing else repeats from town to town: no shops, no save points apart from the inns, no warp spots on the field
  (the Traveler's Wings are items), no hidden pickups apart from the chests and spirits, and no event triggers apart
  from the bosses' (the scenes of `SceneEntry` are the ending's). The people with ordinary lines (talk kinds 0, 1)
  carry nothing to mark.

## Next

- **Fog of war**: left out of this pass on purpose (accuracy first: the whole area is drawn). When it comes, explored
  cells on the reach grid, kept per save (KF4's per-save files are the model: live, quick save and Controller Pak
  copies). The marks are ready for it: each has its stable key (map, sub-area, id). A sign would be hidden until
  its cell (or the cells within its `TextSpotDef.radius`) has been seen, then stay on the map for good (its key's
  "discovered" bit kept with the save), so it is easy to find again; the cheats' marks (chests, spirits, bosses,
  story item givers, battle zones) would show anyway, as in KF3 and KF4.
- Place names for the title (the save menu's map labels), a look of its own for outdoor areas.
