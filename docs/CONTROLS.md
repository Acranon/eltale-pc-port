# Controls: keyboard, mouse and controller

Everything here can be changed on the F1 menu's **Controls** page and is saved in `settings.json`
(`"controls"`, see below). A controller works as it always did; with no mouse or camera keys used the
game runs exactly as on the N64 (the checks in `tests/p1_checks.sh` are unchanged).

## Default bindings

The keyboard follows Quest 64 Enhanced's (the same engine). Each action has two keyboard/mouse slots and
a controller slot.

| Action | Keyboard / mouse | Controller |
| --- | --- | --- |
| Move (stick) | W A S D | left stick (always) |
| Walk (hold: half the stick, keyboard) | Left Ctrl | |
| A (talk, confirm, cast) | Space | A (bottom) |
| B (cancel, camera behind) | Left Shift | X (left) |
| Z | Q | left trigger |
| Start (menu) | Enter, keypad Enter | Start |
| L (camera presets; battle camera turn) | E | left shoulder |
| R | R | right shoulder |
| C up / down / left / right (spirits) | arrow keys | right stick |
| D-pad | I J K L (up, left, down, right) | D-pad |
| Mouse look (hold) | right mouse button (dragged) | |
| Mouse look on/off | Tab | |
| Camera left / right | Z / C, keypad 4 / 6 | |
| Camera up / down (look up / down) | Page Up / Page Down, keypad 8 / 2 | |
| Zoom in / out | `=` / `-`, keypad + / -, mouse wheel | |
| Camera reset | middle mouse button, keypad 5 | |
| Area map (open/close) | M | |
| Minimap zoom in / out | `]` / `[` | |
| Map: battle zones (hold; the Map page's "Battle zones button" cheat on) | B | |
| Spell UI: fire / earth / water / wind crystal | 1 / 2 / 3 / 4 | (C up / left / down / right) |
| Quick bar: slots 1-6 of the page shown | 5 6 7 8 9 0 | |
| Quick bar: page before / after | `,` / `.` | |
| Spell UI: take the elements back | Esc | |

Fixed keys: F1 (or the controller's Back) the menu, F2 quick save, F4 quick load, F11 or Alt+Enter
fullscreen, F8 the flash recorder (debug). Alt+Enter only switches fullscreen, never presses Start: from
its press, Enter is not a game key until it is let go and 400 ms have passed (port/host/input.cpp).

In the game's menus the mouse points and clicks (below, "The mouse in menus"): left click A, right click B. Spells have
their own panel and quick bar (on by default; [SPELL_UI.md](SPELL_UI.md)): click a crystal or press 1-4, click the
plate to cast, right click to take an element back; the quick bar's slots cast with a click or 5-0.
While the full area map is open (docs/AUTOMAP.md) the mouse is the map's: the wheel zooms it, a left drag moves it,
a right click fits it again, pointing at a mark names it, and Esc closes it; the keyboard and the pad still drive the
game (unless the Map page's pause is on). With the map closed, the wheel over the minimap zooms the minimap instead of
the camera (not while mouse look has the cursor), as `]` / `[` do: the reach of the kind of place you are in, indoors
or outdoors (down to 30 units outdoors and 20 indoors). With the battle zones cheat on, the full map has a "Battle
zones" button at its bottom: hold it with the left button (or hold the `map_zones` key, B, also on the minimap) to
shade where random battles can start; let go and they hide. With fog of war (on by default, the Map page) the map shows only
what you have seen, kept with each save; the Cheats' "Show the whole map" turns it off.

## Rebinding

On the Controls page click a binding, then press the key or mouse button (or, in the Controller column, a
controller button or a stick or trigger push). Esc cancels, Backspace or Delete clears the slot. A key or
button does one thing: binding it takes it away from wherever it was. "Reset the controls to the defaults"
puts back the table above. The page also has the mouse camera's speed (10-400%) and an invert switch for
up and down, **Mouse in menus** (on by default; off: the mouse only turns the camera) and **Spell panel and quick
bar** (on by default; off: the game's own spell rows, and the keys 1-0, `,`, `.` and Esc do nothing:
[SPELL_UI.md](SPELL_UI.md)). The spell UI's keys were free before (1-0, `,`, `.`, Esc); Q, E, Enter, Space and the
minimap's `[` `]` and wheel are as they were.

## The mouse in menus

Point at an option, an item, a save or an answer and the game's own cursor goes there (its highlight, its
description); a left click picks it (A). A right click is B where the screen takes B (closes the Start and item
menus, takes the last element back from a spell, backs out of the save menu and the title's choices).

| Screen | Pointing | Left click |
| --- | --- | --- |
| Title | "Press Start": anywhere; the three choices; the confirmation's two rows | Start; A |
| Start menu | the arrows at the top | Z / R (the page before / after) |
| Start menu, map page | (while the marker moves, after A) the marker follows the pointer | puts the marker there (moving: sets it down, A) |
| Start menu, options | | the text speed's or the music's value under it |
| Item menu (R) | an item: the cursor | A (use it); the arrows under the row scroll by one |
| Text windows | an answer of a question: the cursor | A on the answer; a message: A (next page, close) |
| Save menu (save, load, delete) | the two saves shown: the cursor | A; the arrows page (a stick push); a message: A; a question's answers: the cursor, A |
| HUD crystals (field and battle) | the crystal glows | its C button: that element's spell list (or adds the element to the spell being made) |
| Spell list (the spell UI off) | a name: its element's gem glows at its ends | its C button: the element added (refused as with the controller when the spell is not known) |
| The spell being made (the middle name; the spell UI off) | its last element's gems glow | A: cast it |
| The spell panel and the quick bar (the spell UI on) | a plate, a slot: a yellow frame; a slot: its name box | [SPELL_UI.md](SPELL_UI.md) |
| Element allocation (a spirit found) | the cross's four elements | that element raised (its C button) |
| Controller Pak manager | the answers and the note rows: the selection steers there | A on it (after the selection got there) |

How it works (port/p0/mouse_menu.c, port/include/port/mouse_menu.h):

- Each frame's drawing names its **targets** (`Port_MouseTarget` / `Port_MouseTargetFn` at the draw sites,
  patches/p0.patch): a rectangle where the game draws the thing, in the 320x240 screen's units, with the anchor the
  native renderer places it by (CENTRE for menus and text, LEFT for the crystals: NATIVE_DRAW.md "Widescreen"), the
  cursor variable and the value pointing sets (or a function), and what a click presses. `Port_MouseBack` says the
  screen takes B. The targets are kept from the frame build (`nd_frame_begin` starts a new set) to the next read.
- The host hands the pointer to the game with the pad, once per controller read (`ElHost_GetPointer`,
  port/os/controller.c): in **picture units** (x across the 1x picture, 320 wide at 4:3 and for the CPU-drawn Pak
  manager, 426 at 16:9, the window's shape with "Match the window"; y down 240), from the window position through
  the presenter's fit (the picture's own shape, as large as fits, centred: pillarbox and letterbox). Resolution
  scale, MSAA and the border move nothing. `Port_MouseMenuRead` turns it into each anchor's units (CENTRE: x minus
  (width - 320) / 2, LEFT: as is, RIGHT: minus width - 320) and hit-tests the newest target first.
- A pointer that **moved** onto a target sets the cursor (a still pointer never fights the keyboard or the
  controller); a **click** sets it and presses the target's input for that one read: the game reads it exactly as
  the controller's button. Nothing pokes game state from the host's thread; it is all game input on the game's
  thread, so a run with the same pointer rows is the same run, and with no pointer every hash is as before.
- The Controller Pak manager draws with the CPU only when something changes, so its targets come from its state
  (`pak_menu_state` and its cursor places), and pointing steers its selection with one stick push every other read
  until it is there (its own redraw follows); a click then presses A.
- Hover highlights where the game has no cursor are drawn with the game's own pieces, only while the pointer is
  shown and on the target: the element glow on a crystal, the element's gems (and glow) at the ends of a spell name.

**Right button, mouse look and the cursor** (port/host/input.cpp): the right button turns the view only once it is
dragged (more than 3 counts; the motion so far is not lost), so a right click without a drag is B. Tab's mouse look
is as before. Clicks never count during mouse look. The OS cursor shows when the mouse moves or clicks and hides on
a key or controller press, during mouse look (captured) and after 3 seconds still; with the F1 menu open it is
ImGui's. "Mouse in menus" off: no pointer reaches the game.

**Focus and the pointer over the window** (port/host/host.cpp `focused`, input.cpp `input_pointer`): whether the
window has the focus and the mouse is over it come from SDL's window flags at the time they are needed
(`SDL_WINDOW_INPUT_FOCUS`, `SDL_WINDOW_MOUSE_FOCUS`), not only from the focus and enter/leave events: a fullscreen
switch can send a focus lost (or a mouse leave) with no focus gained (enter) after it, and a flag kept from the
events alone then stayed false: clicks, the wheel and mouse look dead while the keys worked. Entering the window
takes the pointer's place there at once (no motion needed). A hidden window (the tests', which the system never
focuses) goes by the events; tools/mouse_host_test.py's focus cases send a lost focus or a leave with no gain after
it, with `system_focus 1` / `system_mouse 1` rows (ELTALE_INPUT_TEST) standing in for the window flags.

`ELTALE_MOUSE_LOG=1` prints `P1_MOUSE_HOVER` (a target pointed at and the value set), `P1_MOUSE_MENU` (a click: the
pointer, the target, what was pressed) and `P1_MOUSE_STATE` (at the next read: the menus' state: cursors, spell,
save, choice, title, Pak manager); `=2` also `P1_MOUSE_TARGETS` (each new set of targets: anchor, tag,
rectangle), for writing tests.

## Mouse look and the game's screens

No mouse look while the game shows a screen the pointer is for: its pause and save menus, the title, the Controller
Pak manager, and in play the item menu, a text window, the spell panel (the spell input) and the element allocation.

- When one opens, mouse look turns off: the Tab toggle is switched off, a right drag does not start (a right click
  there is the screen's: B, or the spell panel's undo, and never turns the view), the camera keys still work. The
  pointer is shown and clicks reach the screen.
- When it closes, mouse look stays off: turn it on again with Tab, or let go of the right button (or the hold key) and
  hold it again. Tab while a screen is shown does nothing.
- How: each controller read the game tells the host whether such a screen is shown (`Port_MouseScreenOpen`,
  port/p0/mouse_menu.c; `ElHost_SetScreen`); the host (port/host/input.cpp `input_screen`) clears the toggle and
  blocks mouse look from that moment until the screen is closed and every mouse-look-hold key and button is up. Only
  the host's camera input changes; the game is the same. `ELTALE_MOUSE_LOG=1` prints `P1_HOST_LOOK on|off` when mouse
  look starts or stops (`screen=1`: a screen is shown). `tools/spell_ui_test.py` (window) checks it: Tab on, the
  spell panel opened (mouse look off), clicks and a right drag in it (undo, no turning), the panel closed, mouse
  motion: still off.

## The mouse camera

Hold the right mouse button and move the mouse (or press Tab for mouse look without holding a button, or
hold the camera keys) to turn the view around the player; the wheel zooms (0.35x to 2.5x the game's
distance); the middle button (or keypad 5) gives the camera back to the game.

- Once turned, the view's direction is the player's own until reset; the game's follow camera keeps
  following underneath. The stick moves the player relative to the view.
- It works in the field, indoors and in battles. It does nothing in the scenes the game films itself
  (camera mode 3), in a battle's or a conversation's focus view, or outside play (title, menus, ending).
- The eye stays above the floor; outdoors (maps with a sky) it stops in front of walls between the player
  and the eye, never nearer than 40 units. Indoors it may pass through walls (the halls are too tight for
  a colliding camera; Quest 64 Enhanced does the same).
- While the view is the player's, billboards (trees, bushes) are not culled by the camera's direction, so
  a view turned or tilted away from the game's never shows missing objects.

How it is built (port/p0/mouse_camera.c, port/include/port/mouse_camera.h):

- The host collects the motion while mouse look is on, the wheel, the reset button and the camera keys
  (port/host/input.cpp) and hands them to the game once per controller read (`ElHost_GetCamera`,
  port/os/controller.c). An input script's `mouse` rows do the same for headless tests.
- `Camera_Update` calls `Port_MouseCameraApply` after the camera mode's handler: the turned and zoomed eye
  (and the yaw it looks along) go into `camera_state`, so the tick's matrices, billboards, the compass and
  the native renderer's recording (`nd_camera`) all see the view. The end of `Gfx_BuildFrameDisplayList`
  calls `Port_MouseCameraRestore`, which puts the game's own eye and yaw back before any game logic runs.
  The game's camera, battles and quick saves (which keep no camera anyway) never see the turned eye.
- `Player_StateMove` adds the view's turn (`Port_MouseCameraMoveYaw`) to the camera yaw it rotates the
  stick by.
- The recording marks those ticks (`ElRdCamera.flags` `EL_RDCAM_ORBIT`, render.h v8): between two such
  ticks the native renderer's pictures between ticks move the eye round the point looked at (yaw, pitch
  and distance interpolated) and only a jump of that point or of the field of view is a cut, so mouse
  turns are as smooth as the game's own camera at 240 pictures a second. The tick after a reset goes back
  to the game's rules (a large jump back snaps, as a camera cut does).
- With no camera input nothing in the game changes: the hooks return before touching anything.

`ELTALE_MOUSE_LOG=1` prints `P1_MOUSE_CAMERA` (the view's yaw, pitch, zoom, the game's yaw, the stick, the
eye and the point looked at) for every tick the view is applied.

## Tests

- `tests/mouse-camera.input` (throne room after New Game: zoom, turns, look down, walking relative to the
  view, a reset, turns while the camera is low) and `tests/mouse-battle.input` (an outdoor battle from the
  `battle-stuck` quick save) are input scripts with `mouse VI DX DY WHEEL FLAGS [X Y]` rows (turn counts at
  0.006 rad across and 0.004 up and down, wheel steps with + zooming in, flags 1 = reset; once each). For the menus:
  X Y put the pointer at that place of the 1x picture (picture units, as above: a CENTRE menu's x is 53 more at
  16:9, the LEFT crystals' the same) and it stays there, shown; flags 2 a left click, 4 a right click, 8 the pointer
  gone; for the spell UI's drags 16 the left button held from then on (18 pressed and held) and 32 let go (a lone 2
  is pressed and let go at once). `spellkey VI MASK` rows press the spell UI's keys (port/spell_ui.h `PORT_SPELLKEY_*`:
  1 << element for 1-4, 0x10 << slot for 5-0, 0x400 / 0x800 the pages, 0x1000 Esc).
  `tools/interp_test.py` runs both (`mouse`, `mouse-battle`): ends exact, no flashes.
- `ELTALE_INPUT_TEST=FILE` (host, with `ELTALE_HIDDEN_WINDOW=1`): events put in SDL's own queue at pad
  read N, `N key_down NAME` / `key_up` (SDL scancode names: W, Space, Left Shift, PageUp, Keypad 4; an
  optional `| alt repeat` suffix sets the event's Alt modifier and repeat flag),
  `mouse_down left|middle|right|x1|x2` / `mouse_up`, `motion DX DY`, `wheel N`, `pointer X Y` (the mouse moved
  to X, Y in the window's coordinates: the menus' pointer; buttons and motion happen where it is). They go through the whole
  host path (bindings, mouse look, the camera keys). Nothing reaches the desktop, and a hidden window never
  takes the mouse (relative mode is skipped) and stays windowed when fullscreen is toggled.
  `tests/host-mouse.events` plays New Game with the keyboard, then the mouse camera.
  `tests/alt-enter.events` presses Alt+Enter in the intro and `tests/enter.events` Enter alone:
  `tests/host_checks.sh` prints `ALT_ENTER OK` when the first run ends as a run with no keys does and the
  second does not.
- `ELTALE_MENU_PAGE=controls` opens the menu on the Controls page (with `ELTALE_MENU_TEST` and
  `ELTALE_PRESENT_DUMP`, a picture of it).

- `tools/mouse_menu_test.py --exe EXE --rom ROM [--saves C:/code/eltale/out/bugsaves] [--pak PAK]`: the menus
  headless, each case in 4:3 and 16:9 (pictures of the hover highlights and the crystal's spell list in
  out/mouse_menu_test/<case>-mouse43|mouse169/pictures) and once more with the controller inputs the clicks stand
  for: items (point at the 4th then the 2nd item, click, right click closes), spell_field and spell_battle (click a
  crystal, point at and click a name, click the middle name to cast), load (the title's Press Start and Continue,
  the save), delete (the save menu's question: "No", then B), pak (the Controller Pak manager: list, note, Delete,
  confirm, B). It checks the states after the clicks, the same states and game sound at 4:3 and 16:9, and the same
  ending as the controller's (last_hash; the Pak manager's last screen). Without --pak it makes a Controller Pak
  with a save once (tests/tour-save.input, a few minutes). `MOUSE_MENU_TEST OK`.
- `tools/spell_ui_test.py --exe EXE --rom ROM [--saves ...]`: the spell panel and the quick bar (docs/SPELL_UI.md
  "Tests"), with the host's path and mouse look in a hidden window. `SPELL_UI_TEST OK`.
- `tools/mouse_host_test.py --exe EXE --rom ROM`: the host's window-to-picture mapping in a hidden window: 4:3,
  4:3 pillarboxed at scale 2, 16:9, 16:9 letterboxed at scale 3, "Match the window" narrow (letterboxed 4:3) and
  wide (686 wide): the same picture positions and the same pick ("Continue") in every case. `MOUSE_HOST_TEST OK`.

## settings.json

```json
"controls": {
  "mouse_sensitivity": 100,
  "invert_y": false,
  "mouse_menus": true,
  "spell_ui": true,
  "bindings": {
    "a": ["Space", "", "Pad a"],
    "c_up": ["Up", "", "Pad -righty"],
    ...
  }
}
```

Each action (input.cpp's names: `stick_up`, `walk`, `a`, `b`, `z`, `start`, `l`, `r`, `c_up`..., `d_up`...,
`look_hold`, `look_toggle`, `camera_left`, `camera_right`, `camera_up`, `camera_down`, `zoom_in`,
`zoom_out`, `camera_reset`, `map`, `map_zoom_in`, `map_zoom_out`, `map_zones`, `spell_fire`, `spell_earth`,
`spell_water`, `spell_wind`, `slot_1`..`slot_6`, `page_prev`, `page_next`, `spell_clear`) lists three bindings: SDL
scancode names, `Mouse Left|Middle|Right|X1|X2`,
`Pad <SDL button name>` or `Pad +<axis>` / `Pad -<axis>`; `""` is none. An action left out keeps its
defaults.

## Next steps

- The mouse in menus, not done: the wheel does not scroll the item and save lists (it zooms the camera, as
  before); the Start menu's options are set by a click, not by pointing at the rows in their edit mode.
- Click to interact and to aim spells: a click could be bound as A today (bind Mouse Left); aiming a
  spell where the cursor points needs the spell code's target direction (the player's facing when cast)
  turned toward the picked point. Not started.
- Quest 64 Enhanced's per-object cull opening is done here for billboards; effect emitters
  (`Effect_QueueMapObjEmitter`, +-90 degrees of the yaw) follow the view's yaw.
