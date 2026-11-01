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
| Mouse look (hold) | right mouse button | |
| Mouse look on/off | Tab | |
| Camera left / right | Z / C, keypad 4 / 6 | |
| Camera up / down (look up / down) | Page Up / Page Down, keypad 8 / 2 | |
| Zoom in / out | `=` / `-`, keypad + / -, mouse wheel | |
| Camera reset | middle mouse button, keypad 5 | |

Fixed keys: F1 (or the controller's Back) the menu, F2 quick save, F4 quick load, F11 or Alt+Enter
fullscreen, F8 the flash recorder (debug).

## Rebinding

On the Controls page click a binding, then press the key or mouse button (or, in the Controller column, a
controller button or a stick or trigger push). Esc cancels, Backspace or Delete clears the slot. A key or
button does one thing: binding it takes it away from wherever it was. "Reset the controls to the defaults"
puts back the table above. The page also has the mouse camera's speed (10-400%) and an invert switch for
up and down.

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
  `battle-stuck` quick save) are input scripts with `mouse VI DX DY WHEEL FLAGS` rows (turn counts at
  0.006 rad across and 0.004 up and down, wheel steps with + zooming in, flags 1 = reset; once each).
  `tools/interp_test.py` runs both (`mouse`, `mouse-battle`): ends exact, no flashes.
- `ELTALE_INPUT_TEST=FILE` (host, with `ELTALE_HIDDEN_WINDOW=1`): events put in SDL's own queue at pad
  read N, `N key_down NAME` / `key_up` (SDL scancode names: W, Space, Left Shift, PageUp, Keypad 4),
  `mouse_down left|middle|right|x1|x2` / `mouse_up`, `motion DX DY`, `wheel N`. They go through the whole
  host path (bindings, mouse look, the camera keys). Nothing reaches the desktop, and a hidden window never
  takes the mouse (relative mode is skipped). `tests/host-mouse.events` plays New Game with the keyboard,
  then the mouse camera.
- `ELTALE_MENU_PAGE=controls` opens the menu on the Controls page (with `ELTALE_MENU_TEST` and
  `ELTALE_PRESENT_DUMP`, a picture of it).

## settings.json

```json
"controls": {
  "mouse_sensitivity": 100,
  "invert_y": false,
  "bindings": {
    "a": ["Space", "", "Pad a"],
    "c_up": ["Up", "", "Pad -righty"],
    ...
  }
}
```

Each action (input.cpp's names: `stick_up`, `walk`, `a`, `b`, `z`, `start`, `l`, `r`, `c_up`..., `d_up`...,
`look_hold`, `look_toggle`, `camera_left`, `camera_right`, `camera_up`, `camera_down`, `zoom_in`,
`zoom_out`, `camera_reset`) lists three bindings: SDL scancode names, `Mouse Left|Middle|Right|X1|X2`,
`Pad <SDL button name>` or `Pad +<axis>` / `Pad -<axis>`; `""` is none. An action left out keeps its
defaults.

## Next steps

- Mouse in the game's menus (pointing at an item or option): the menus (game/ui/menu.c, the item and save
  menus, text choices) are cursor indices drawn at N64 screen positions; each needs its hit rectangles and
  a hook to set its cursor, and a click as A. Not started.
- Click to interact and to aim spells: a click could be bound as A today (bind Mouse Left); aiming a
  spell where the cursor points needs the spell code's target direction (the player's facing when cast)
  turned toward the picked point. Not started.
- Quest 64 Enhanced's per-object cull opening is done here for billboards; effect emitters
  (`Effect_QueueMapObjEmitter`, +-90 degrees of the yaw) follow the view's yaw.
