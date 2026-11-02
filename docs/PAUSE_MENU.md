# The pause menu: one screen for the map, status, elements and options

The game's Start menu has four full-screen pages you slide between (Status, Map, Elements, Options). The port shows
them as one screen instead, after the owner's spec (`eltale-pause-menu-spec.md`, its mockup, and the owner's changes:
no prompt strip, the map smaller at 4:3, a marker of the player's own with a cyan compass dot, the game's own art).
Code: `port/p0/pause_menu.c`, `port/include/port/pause_menu.h`; the hooks in `patches/p0.patch` (menu.c `Menu_Update`,
hud.c `Hud_DrawCompass`, save_menu.c); the keys in `port/host/input.cpp`. On by default in a window (F1, Controls:
**Unified pause menu**); off, the game's four pages are back exactly as they were.

## Using it

- **Open / close**: Start (Enter, the controller's Start) as always, or **Esc**. B closes it too (the game's own), and
  so does the **Close** button (top right). It opens where the game opens its menu: in the field, not in a battle, a
  text window, a cutscene or while a spell is being put together (the game's own test, `Core_RunGameplayLoop`).
- **The map** (left): the world map of a region, the game's own pictures (the Japanese or the English ones, with the
  language). It opens on the region the player is in, centred on the player.
  - **Regions**: the arrows by the name (click), **Z** / **L** (Q / E) or **C left** (Left arrow) for the one before,
    **R** (R) or **C right** (Right arrow) for the next. The list is the game's ten regions in the game's order, of
    which those with a map picture (the six field regions) that the player has been in, and the one the player is in
    now (a region with no map, Aqua Island, Lake Lenos, Shelwood or the Floating Sanctum, shows its name panel).
  - **Zoom**: 1x, 1.5x, 2x (the picture's pixels, nearest neighbour): the wheel, **+** / **-** (the camera's zoom
    keys, `=` and `-`), the **+** and **-** buttons at the map's bottom right, or **C up** / **C down** (Up / Down
    arrows, the right stick). The view's middle stays put.
  - **Pan**: drag the map with the left button, the D-pad (I J K L: 8 pixels a press, repeating when held) or the
    stick (W A S D). The picture never leaves the view.
  - **The player** is the game's red pin (blinking, as on the game's map page), in the region the player is in: at
    the player's place in the field, at the town's or the dungeon's point when inside one.
  - **The marker**: a **right click** on the map puts the player's marker (the game's quill) there; a right click on
    it takes it away. **A** (Space) does the same at the cross in the middle of the map. One marker at a time: a new
    one replaces the old, in any region. It stays until taken away.
- **Status** and **Elements** (right): HP and MP (now / most), Defense and Agility, each with its bar and the "exp"
  of its growth as the game's Status page shows it; the four spirit levels out of 50 with their bars; the next
  spirit's gauge (the Elements page's orb) and its percentage.
- **Options**: the button at the top right, **O** or the controller's **Y**. The game's own two rows, Text Speed and
  Sound (5 steps each), laid out as the game's Options page: the D-pad, the stick or the C buttons move the hand
  (up and down) and the value (left and right); a click on a slider sets it, its knob can be dragged; **Back**, B,
  Start, Esc or O closes it (the menu stays). A change is the game's at once: the text speed is used by the next
  text window, the music's volume is applied (`Bgm_ApplyVolumeOption`). Both are saved with the game, as always.
- **The compass**: while the player is in the field of the marker's region, the compass shows a **cyan diamond**
  toward the marker (the game's own red diamond, the one it draws for its map page's marker, in cyan), placed by the
  game's own rule: the bearing from the player to the marker less the camera's yaw, 18 pixels out (it turns with the
  view, the mouse camera's too). It fades in, and it is not shown in another region, inside a town or a dungeon, in a
  battle, or within 10 units of the marker (the game's rules for its dot).
- **The mouse**: every button, the arrows, the zoom buttons, the map (drag, wheel, right click), the sliders and Back;
  pointing at a button outlines it in gold; a pressed button moves down a pixel. A right click away from the map is
  B (closes Options, or the menu). Mouse look is off while the menu is shown (as on every game screen).

## Layout (game pixels: 240 high; 426 wide at 16:9, 320 at 4:3; the spec's numbers)

| Piece | 16:9 | 4:3 |
|---|---|---|
| The frame | x 10, y 12, 402 x 204 | x 8, y 12, 304 x 204 |
| The map's title ("Map", the game's) | over the map, on the frame's top edge | same |
| Region arrows (the game's) and name | y 5 to 20, over the map | same |
| The map | (6, 22), 214 x 176 | (6, 22), 150 x 176 |
| Zoom + / - | the map's bottom right, 11 x 11 | same |
| The divider | x 228 | x 162 |
| Options, Close | (327, 5) 53 x 13, (383, 5) 13 x 13 | (238, 5) 44 x 13, (285, 5) 13 x 13 |
| The right column | x 234, 162 wide | x 168, 130 wide |
| Status rows | y 36 + 20 i: label, value over a 58-wide bar at x 38, "exp" at x 101, % at the right | bar 38 wide, "exp" at x 81 |
| Elements rows | y 132 + 16 i: gem, level/50, a 60-wide bar at x 51 | bar 36 wide |
| Next spirit | x 120, 42 wide: "Next spirit", the orb 32 x 32 at y 148, % at y 183 | x 90, 40 wide |
| Options dialog | the game's Options page's 240 x 108 panel, centred, y 66 | same |

The frame is anchored to the picture's left with its own x for each width, so it never moves with the 4:3 middle;
wider pictures than 16:9 centre the 16:9 layout, narrower ones the 4:3 one. Differences from the spec's numbers, all
for the game's pictures: the Status labels are the game's (Def is 36 wide), so the bars start at x 38; the game's
bars are 7 high (the spec's 5); the exp gauge is 56 x 56 in the game, drawn at 32 x 32 (filtered, as the game draws
its menu sprites); the Options dialog is the game's page (240 x 108, the spec's 250 x 106). The prompt strip is left
out (the owner: a help key later, and the key list on the Controls page).

## The game's art (read from the ROM or RDRAM at run time; nothing of it is in the repository)

| Piece | From |
|---|---|
| Parchment, frame lines, shadow, grey band | the menu's sprites 0 (16 x 16, repeated as the pages do), 0x37 / 0x38 / 0x39 (the pages' frame colours), 0x3C |
| Titles: Map, Status, Elements, Options | sprites 0x2B, 0x1A, 0x2D, 0x21 (with the grey bullets; English in English) |
| Hp, Mp, Def, Agi, exp, % and / | sprites 0x1B-0x1E, 0x20, 0x15, 0x14 |
| Numbers | the menu's digits (sprites 0x0A-0x13, 7 apart) |
| Bars | the Status page's frame (0x32) and fill (0x33), stretched to the bar |
| Gems | sprites 0x2E, 0x2F, 0x31, 0x30 (fire, earth, water, wind) |
| The next spirit's orb | the Elements page's exp gauge, its four layers (`menu_exp_gauge_tex`, `Menu_DrawExpGauge`) |
| Region arrows, the hand cursor, + and - | sprites 6, 7 (wobbling as on the pages), 1, 0x16, 0x17 |
| The map pictures | `menu_map_picture_dma` (the ROM's 244 x 185 CI8 maps and the four name panels), read into the port's own copies (the map page loads them into RDRAM at 0x801C0000, which cutscenes use too) |
| The player's pin, the marker and its shadow | sprites 0x2C, 4, 0x55 (the map page's) |
| Sliders | sprites 0x24, 0x25, the knobs 0x26-0x28, Slow / Fast / Soft / Loud 0x56, 0x57, 0x2A, 0x29 |
| The backdrop | the menu's background strips (`Menu_DrawBackground`), over the whole picture and dimmed |
| Region name, "Next spirit", button labels | the HUD's spell-name font (`D_803A8748`, its Latin letters: English only) |
| The compass's cyan diamond | the HUD atlas's red diamond (s 0, t 0x32), each texel's brightness made cyan |

Ours (made in code): flat fills (the dim layer, the plates' bevels, the outlines), the close cross and a 7 x 7 gear.
In Japanese the labels in the HUD font are left out (it has no kana there); everything else is the game's.

## How it works

- **The game's menu runs as always.** Start opens it (game mode 2, `Core_RunMenuLoop`), Start or B closes it
  (`Menu_Update`'s own test). With the switch on, `Menu_Update` asks `Port_PauseMenuFrame` (not in the save menu):
  this screen's input and drawing are done there instead of the pages; the game's background, pages and their
  mouse targets are not run. `menu_flags` page bits are left alone (switching back finds the page it left).
- **Inputs**: the controller's buttons are the game's own (`input_pressed_buttons`, held, the stick) as read for that
  frame. The pointer, the wheel and the keys come in at the controller read (`Port_PauseMenuRead`, after the spell
  UI's and before the menus' pointer; `Port_PauseMenuWheel` before the camera): every click, drag and wheel step over
  the open menu is taken there and acted on at the next frame. Esc and the Close button become the Start the game
  closes (and opens) its menu with, pressed for one read; a right click off the map, its B. Everything is on the
  game's thread, from the reads, so a run with the same input rows is the same run.
- **What it changes in the game**: the two options (`menu_text_delay_option`, `menu_bgm_volume_option`, as the
  Options page sets them, plus `Bgm_ApplyVolumeOption` at once) and `menu_flags` 0x20 while Options is open (the
  game's "page edit mode", so B and Start close the dialog, not the menu; cleared at the next frame). Sounds are the
  game's (`Sfx_Play`: 0 a move, 0x17 the marker placed, 0x13 taken away, 1 and 2 the dialog). The region, the view,
  the marker and the regions seen are the port's.
- **Drawing**: native 2D only, the spell UI's way (docs/SPELL_UI.md): the game's gbi macros with the game's
  textures into a display list of our own that is thrown away (`dl_scratch`), with `nd_2d_push` / `nd_2d_pop`; the
  native core records the rectangles. One render mode (0x504240, the menu's sprites') and two combiners (texture x
  ENV, ENV with the texture's alpha): no new GPU pipelines. The frame is drawn over the whole picture with the LEFT
  anchor (not clipped to the 4:3 middle). Animations, per tick (30 a second): the screen fades in over 4 ticks, the
  map cross-fades over 3 when the region changes, the marker drops 3 pixels over 2, Options fades in and rises 2
  pixels, the compass diamond fades in over 5. The ids carry the region and the zoom, so the pictures between ticks
  never slide one map into another.
- **The compass**: `Hud_DrawCompass` still draws the game's own red diamond for the map page's marker into the
  frame's list (so the frame's list is the same as before), but its native item is muted while the switch is on; the
  cyan diamond is drawn after it, with the HUD's origin and the RIGHT anchor, by the same math.
- **The marker's file (per save)**, as the spell UI's quick bar and the area map's fog of war: the save's record is
  the key. `SaveMenu_CaptureGameState` (the save menu's save, an F2 quick save) and `SaveMenu_RestoreGameState` (load,
  Continue, F4) call `Port_PauseMenuSaveEvent`, which hashes the record (FNV-1a 64 of the play time, day, map and
  sub-area as 32-bit numbers, then the SaveGameStateBlock) and writes or reads `mapmarker/save_<key>.mark` in the
  data folder (text: `ELTMAPMARK 1`, `visited <mask>` of the regions seen, `marker <region> <x> <y>` in the map
  picture's pixels; written to a .new file and renamed). A change is written at once to the current save's file; a
  new game keeps it in memory until its first save; the title and the intro forget it. A save with no file (every
  older save) starts with no marker and counts the regions up to the one it was saved in as seen (the story goes
  through them in order). The save formats are unchanged. With the switch off nothing is read or written.

## Answers to the spec's open questions (from the decomp)

1. **The game's marker**: the JP map page's marker is `menu_map_cursor_x` / `_y` (map pixels, f32), one position for
   every region, read in the current region's scale; `menu_map_marker_x` / `_z` hold its world position
   (`Menu_UpdateMapPage`, `Menu_FindMapPageLocation`). It always exists (set to (115, 86) on the first area load), is
   moved with A and the stick on the map page, and **is not saved** (not in `SaveHeader`, not in the quick save).
   The compass draws a red diamond toward it in the field of any region with a map. So the menu keeps a marker of its
   own (region, x, y; removable) in the per-save file, uses the game's transform, quill, shadow and compass math, and
   in place of the game's red diamond shows its own in cyan. The game's marker is left as it is (the original pages
   still have it when the switch is off).
2. **The player on the map**: the original draws the player's position (a blinking red pin, sprite 0x2C), no facing.
   Field: `x_map = (s32)x / menu_map_world_scale[r] + menu_map_origin[r]`, `y_map = (s32)z / menu_map_world_scale[r +
   6] + menu_map_origin[r + 6]` (world +z is down the picture); inside a town or dungeon (a "spot") a fixed point
   (`menu_map_spot_coords`). The inverse for the marker: `x = scale_x (x_map - origin_x)`, `z = scale_z (y_map -
   origin_z)`, as the game's own marker. The menu shows the game's pin (the spec's facing arrow is not in the game).
3. **Regions**: ten, `menu_map_region` 0..9, found on every area load from `menu_map_location_list` (map and sub-area
   to region and spot): 0 Holy Plain, 1 Darlam Flats, 2 West Angulus, 3 Rideen Suburbs, 4 Dingam Desert, 5 Veragoon
   Moor (each a 244 x 185 map picture), 6 Aqua Island, 7 Lake Lenos, 8 Shelwood, 9 Floating Sanctum (a name panel
   only). The names are painted into the pictures; the English (the port's, art/en/maps.json) is in
   translations/en.v2.tsv as `region.0`..`region.9`. The game's own rule is that its map page shows only the region
   the player is in; the menu lists that one and the map regions the player has been to (kept per save).
4. **Zoom**: the map pictures are 244 x 185, drawn at their own pixels at 1x and nearest-neighbour at 1.5x and 2x.
   The whole picture does not fit the 214 x 176 view at 1x (the spec assumed 162 x 172): it pans a little at 1x.
5. **Bar caps**: `Player_UpdateStatGrowth` grows max HP and max MP only below 500 and Defense and Agility only below
   255; the Status page's bars are HP / max HP, MP / max MP, and Defense and Agility out of 255 (the game's own).
6. The marker stays until taken away.
7. The compass diamond is hidden in another region (and inside a town or dungeon, where the field's coordinates do
   not apply).
8. The Close button is kept.
9. Options has the game's Text Speed and Sound; the dialog is a table of rows (the hand, the sliders) with room for
   more.
10. No menu in a battle (the game's own test).

Also from the decomp: the Status page's exp % is `hp_xp * 100 / player_stat_xp_costs[hp_growth_rank]` (MP: the cost
x 3 with water at 50, else x 4; Defense and Agility: x 0.8 with earth / wind at 50), the next spirit's % is `exp * 100
/ player_level_exp_table[level]` (at most 100), the element levels are at most 50, Sound is the music's volume
(`bgm_volume_levels`, 5 steps; the spec's 11 do not exist), and the text speed is kept reversed (left, slow, is 4).

## Keys (docs/CONTROLS.md)

The game's buttons as the original menu used them: Start and B close, A acts (the marker), Z and R (and L) change
the region as they changed the page. The C buttons and the D-pad (the arrow keys and I J K L) and the stick are the
map's. Two keys more: **Esc** is the spell UI's "take the elements back" key (`spell_clear`): while no spell is being
put together it opens and closes the menu (and closes Options) — a key does one thing in the bindings, so it is the
same action, renamed "Spell UI: take the elements back; pause menu: open / close". Esc's other uses are not touched:
the area map closes with Esc itself on the host (the game never sees that Esc), and while a spell is being made Esc
takes the elements back and the menu cannot open anyway. **O** / the controller's **Y** (`menu_options`, new,
rebindable) opens Options; it was free. The wheel and `=` / `-` (the camera's zoom) zoom the map while the menu is
open (the camera does nothing then). Nothing else changed: Q is Z, E is L, Enter is Start, Space is A, B is the map's
battle zones, `,` `.` `[` `]` and 1-0 are the spell UI's and the minimap's.

## Tests

`python tools/pause_menu_test.py --exe out/p1-windows/eltale-p1.exe --rom ROM [--saves C:/code/eltale/out/bugsaves]`:
from battle-stuck's quick save (the Holy Plain's field), each case at 4:3 and 16:9 with pictures
(`out/pause_menu_test/<case>-43|169/pictures`): `pad` (the controller alone: regions with R, Z, L and the C buttons,
zoom, pan, the marker on, off and on with A, Options with Y, both values, B closes Options, B the menu), `mouse` (the
arrows, the wheel, the zoom button, a drag, right clicks on, off, on, Options, a click on a slider, a knob dragged,
Back, Close), `esc` (Esc opens, O, Esc closes Options, Esc closes the menu), `compass` (a marker north of the player
from its file: the diamond's bearing, angle and place checked against the positions and the camera, the view turned
with the mouse camera and checked again, then the marker moved to another region: the diamond goes), `persist` (the
marker, F2, then a new process: F4 brings it back), `window` (the host's path in a hidden window: Esc, O, a right
click, Close) and `same` (a walk with the switch on and off: the same quick save and the same sound).
`PAUSE_MENU_TEST OK`.

`ELTALE_PAUSE_LOG=1` prints `P1_PAUSE_OPEN`, `_VIEW`, `_MARK`, `_OPTION`, `_PRESS`, `_CLOSED`, `_COMPASS`, `_SAVE`.
The headless runs have the switch off unless `ELTALE_PAUSE_MENU=1`, so `tests/p1_checks.sh`, the picture baselines
and the mouse-in-menus test still see the game's own pages.

## Not done

- A help key (the owner's: later) and more rows in Options (window, resolution, controls).
- The spec's blurred game picture behind the menu: the game draws no world in its menu mode; the backdrop is the
  menu's own background, dimmed.
- The spec's closing fade: the game leaves its menu mode at once.
- Japanese labels for the region name, "Next spirit" and the buttons (the HUD font has no kana for them).
