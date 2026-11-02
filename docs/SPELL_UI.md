# The spell UI: the spell panel and the quick bar

A mouse-friendly face on the game's own spell input, after the owner's spec (`eltale-battle-ui-spec.md`, its mockup,
and the owner's change to the crystals: the next-spell plates under each crystal). Code: `port/p0/spell_ui.c`,
`port/include/port/spell_ui.h`; the hooks in `patches/p0.patch` (hud.c, save_menu.c); the keys in
`port/host/input.cpp`. On by default in a window (F1, Controls: **Spell panel and quick bar**); off, the game draws
and behaves as the original.

## Using it

- **The panel** opens when a spell is begun: a click on an element crystal at the bottom left (the HUD's), the keys
  **1-4** (fire, earth, water, wind: the crystals' order on screen) or the controller's C buttons. From the top:
  - a hint, *Click plate to cast · R-click to undo*;
  - **the spell's plate**: the game's chevron plate with the elements put in (the game's gems, an outlined place for
    each one left), the spell's name in the game's plate font and colours, and its MP. **Click it to cast** (the
    game's A: its own aim and target rules). A **right click** anywhere takes the last element back (the game's B);
    with nothing in it does nothing. **Esc** takes them all back. There is no cancel button.
  - **the four crystals**, the HUD's own, twice the size and spaced out, with their key (1-4) and spirit level;
  - **under each crystal, the spell that element would make** (the game's plate, the name over one or two lines).
    A click on the plate is the same as a click on its crystal. An element the game would refuse (not learned at
    your spirit levels, or not enough MP, or three elements in already) shows a dull crystal and an empty, faint plate
    and does nothing. A name in the game's grey is a spell that is not usable here (a battle spell in the field): the
    game takes the element (it leads on to others) but would not cast it.
- **The quick bar** (bottom, after the HUD's crystals): 3 pages of 6 slots, keys **5 6 7 8 9 0**.
  - **Keep a spell**: with a spell on the plate, click an empty slot (it shows +), or drag the plate onto any slot
    (it replaces what was there). Drag a slot onto another to swap them; drag one off the bar to empty it.
  - **Cast**: click a slot or press its key: the game is given the slot's elements one at a time and then A, exactly
    as the controller would (a spell half made is taken back first). A slot the game would not cast now (MP, a spell
    not learned, a battle spell in the field) has a red-brown frame and faint gems and does nothing.
  - **Pages**: the wheel over the bar, the page dots (16:9) or **,** and **.**. In 4:3 the "Spells" label and the dots
    are left out; paging works the same.
  - **Hover** a slot for a moment: its name, gems and MP (and why it cannot be cast: *Not enough MP*, *Not learned
    yet*, *Not in battle*, *Only in battle*); over an empty slot with a spell on the plate: *Keep it on key N*.
  - The bar is **kept with each save** (below) and starts empty for a new game.
- **When**: the panel and the bar show in play (the field and battles) and hide in menus, text windows, cutscenes
  and the element allocation. In a battle the bar fades out on the enemy's turn and while a spell plays; in the field
  it stays (the field's spells are Healing Lv1 and Lv2, Exit and Return; the others are battle spells, shown greyed).
- **The controller** drives the same thing: the panel shows the game's own spell input, so C buttons, A and B work
  as always and the panel follows them.

**No mouse look while the panel is open** (as over the game's menus and text windows: docs/CONTROLS.md "Mouse look
and the game's screens"): opening it turns mouse look off (Tab's too), a right drag there is the undo and never turns
the view, and closing it does not turn mouse look back on (Tab, or hold the right button again).

All keys are on the F1 Controls page and can be rebound (`spell_fire` ... `spell_wind`, `slot_1` ... `slot_6`,
`page_prev`, `page_next`, `spell_clear`). The keys taken before were checked: 1-0, `,`, `.` and Esc were free
(Q is Z, E is L, Enter is Start, Space is A, `[`/`]` and the wheel over the minimap zoom the minimap).

## Layout (game pixels: 240 high, 320 wide at 4:3, 426 at 16:9)

The panel is placed by the CENTRE anchor (in the 4:3 middle, as the game's own spell rows), the bar and the HUD's
key digits by LEFT (as the HUD crystals).

| Piece | Where (x in the 320 middle) | Size |
|---|---|---|
| Hint | y 92, centred on 160 | the HUD font, 9 high |
| Spell plate | x 68, y 103 | 184 x 14 (the game's 110 x 14 plate, its ends kept and its middle repeated) |
| Gems in it | x 81 + 12 i, y 104 | 11 x 11 (the game's) |
| MP | right of the name, before the right chevron | the HUD digits and "MP" |
| Crystal i | middle x 49 + 74 i (49, 123, 197, 271), y 121 | 24 x 24 (the HUD's 12 x 12 at 2x) |
| Its key digit | crystal's x - 4, y 119 | 5 x 7 (ours) |
| Its spirit level | crystal's middle + 1, y 135 | the HUD digits |
| Next-spell plate i | x 49 + 74 i - 35, y 149 | 70 x 22 (two lines of 6-pixel letters: "Restriction" is 66) |
| Quick bar | x 124, y 194 | 175 x 21 at 16:9 (label, slots, dots), 123 x 21 at 4:3 |
| Slots | x 127 (+38 at 16:9) + 20 i, y 197 | 17 x 15 |
| Name box | x 124, bottom at y 190 | 100 or more wide, 29 high |

Room checked: the panel spans y 92-171 and x 14-306 (4:3; 67-359 at 16:9). The HP/MP block ends at y 62 and the
enemy panel (battle, top right) at y 78: above it. The minimap (ImGui, 24 % of the height at the bottom right: a
57.6-unit square 6 from the edges) starts at y 176 and x 256 (4:3) or 362 (16:9): the next-spell plates end 5 above
it, the bar ends at x 247 (4:3) and 299 (16:9). With the minimap moved to a top corner in 4:3 it covers the right
end of the panel; at 16:9 it never does. The HUD crystals (y 198-210) and the bar keep their places when the panel
opens. Taller than the spec's column (image 22 put the plates under the crystals): at 240 lines the names need the
74-pixel pitch, so the panel is wider than the HUD row and sits over the middle of the picture, where the game's own
spell rows are.

## The game's art (read from the ROM at run time; nothing of it is in the repository)

| Piece | From |
|---|---|
| Plates (spell and next spell) | the battle HUD atlas `D_803A8718` (CI8, palette `D_8039EC90`): the chevron plate at (100, 14) and the plain plate at (100, 0), 110 x 14 each, the game's middle-name and spell-row plates |
| Gems in the plate and the name box | the same atlas, (64, 36 + 11 e), 11 x 11: the game's spell-input gems |
| Crystals | the main HUD atlas `D_803A86F8` (CI8, palette `D_8039ADB0`): (32 + 12 e, 16), 12 x 12, as the HUD draws them; a dull crystal is the same texture with a grey silhouette over it |
| Digits (spirit levels, MP) | the same atlas's italic outlined digits at (32, 29) through `Hud_DrawNumber` |
| Parchment (the bar, the name box) | the same atlas's 16 x 16 parchment swatch at (0, 0), repeated |
| Names, hint, labels | the spell-name font `D_803A8748` (8 x 9 glyphs; English: the port's Latin letters, 6 apart) through `Hud_DrawGlyphString` with the game's palettes `D_803A8778` (0 normal, 1 short of MP, 2 not usable or not learned: `hud_spell_label_palettes`); the hint and "Spells" use the same glyphs with a white palette of ours |
| The HUD set-up | `D_803A8788`, the HUD's own state list, at the start |
| Element colours (the bar's small gems) | `hud_element_colors` |

Ours (made in code at the start, no files): the tiny key digits (3 x 5 with an outline, and plain ones for the
slots), the outlined empty gem, the bar's small gems (5 x 5, in the game's element colours), the "+", the slot and
box frames, the shadows and the page dots.

## The game's rules it uses (spec Q2 and Q10, from the decomp)

- **Every sequence is a spell** (Q2): each element has a table of 15 spells (`D_800C185C[element]`, `SpellDef`), one
  for each sequence that starts with it: the element alone, the 4 two-element ones and the 10 three-element ones.
  `Magic_LookupSpellSequence` sorts the second and third elements of a three-element sequence, so their order does
  not matter (Earth, Water, Fire is Earth, Fire, Water: Confusion Magic), and every prefix of a spell is itself a
  spell. The mockup's missing steps (Confusion, Magic Barrier) are Rolling Rock Lv1 (Earth, Fire) and Spirit Armor Lv1
  (Earth, Water) etc. A sequence the table lacks would fall back to the element's first spell, but none does.
- **Learning** (Q10): `Magic_GetSpellRestrictionFlags(id, mp)` decides, and the UI calls it: 4 when the spirit level
  of the first element is below the spell's `required_level`; 2 when the spell is not for here (`SpellDef.flags` bit
  0 battle, bit 1 field); 1 when MP is short. `Player_HandleSpellInput` refuses an element whose sequence has 4 or 1
  (the press buzzes and the element is not added) and casts on A or Z only when the flags are 0. The UI greys a
  crystal for 4 or 1 (or three elements in), shows a 2 in the game's grey, and keeps a slot castable only when every
  step would be taken and the last has no flag.
- **MP** is by the number of elements, `D_800C186E[length]`: **1, 2, 3** (read from the ROM: 1 1, 2 2, 3 3). The
  spell's own record has no cost; `Player_StartCastSpell` takes `D_800C186E[spell_input_count]`.
- **Field spells**: Healing Lv1 (Water, Earth), Healing Lv2 (Water, Earth, Earth), Exit (Water, Wind), Return (Water,
  Wind, Wind). Escape (Water, Earth, Wind) is battle-only.
- **When an element is taken** (`Player_Update`): to begin, no spell input, item menu, text or allocation
  (`core_flags & 0xF`), the player can act (`Player_CanAct`: in a battle, the player's turn with no spell playing) and
  not silenced (status 0x40); while a spell is being made (`core_flags & 4`), always.
- `ELTALE_SPELLUI_LOG=2` prints the table as the game has it (`P1_SPELLUI_SPELL element index len seq level flags
  mp name`), every sequence's lookup (`P1_SPELLUI_SEQ`), the MP table and the plate font's palettes.

## How it works

### Inputs, not state

Everything the UI does to the game is a controller input at a controller read, on the game's thread
(`osContGetReadData`, port/os/controller.c: `Port_SpellUiRead` before `Port_MouseMenuRead`):

- The host sends the keys pressed since the last read (`ElHost_GetSpellKeys`: `PORT_SPELLKEY_*`, and the switch) with
  the pointer (`ElHost_GetPointer`, which now also says the left button is held, 16, and was let go, 32). An input
  script gives the same with `spellkey VI MASK` rows and `mouse` rows (flags 16 hold, 32 let go).
- A click on a crystal or a next-spell plate presses its C button (on the press); the plate and the slots act when the
  button is let go on them (so they can be dragged); a right click is B. The pointer's clicks on the panel and the bar
  are taken from the menus' pointer (`Port_MousePointerTake`), so nothing under them presses anything too.
- One press a read, then a read without it (the game sees a press when the button goes down). A quick-bar spell is
  a small machine (`macro_step`): B while what is in is not the start of the slot's spell, then the next element,
  then A, each checked against the game's state at the next read (a refused element or A stops it).
- **The one cast** is `cast_press`: A. The plate's click, the bar's last step and `Port_SpellUiCast` all end there.
- What the UI shows is the game's own state: the panel is `player_state.spell_input_count/dirs` (the game's spell
  input), its crystals are what `Player_HandleSpellInput` would take. It never writes game memory.
- With the switch off nothing is read or pressed; with it on and no mouse or keys used nothing is pressed. The
  headless runs have it off unless `ELTALE_SPELL_UI=1`, so `tests/p1_checks.sh`, the picture baselines and the
  mouse-in-menus test are the original game's. `tools/spell_ui_test.py` checks that a case played with the mouse and
  with the controller presses alone ends the same.

### Drawing: native 2D, from the game's own drawing code

The panel has to look like the game (its plates, gems, crystals, digits and font), so it is drawn the way the game's
HUD is: `Port_SpellUiDraw`, last in `Hud_Draw`, calls the game's own `Hud_DrawTextureTile`, `Hud_DrawGlyphString`,
`Hud_DrawNumber` and the gbi macros with the game's textures in RDRAM, inside the HUD's 2D section, into a display
list of its own that is thrown away (`dl_scratch`): the native 2D core (`nd_2d.c`) records the rectangles as they are
made, as it does for every HUD draw. The frame's display list is never touched (the headless frame hashes are the
same), `gfx_mtx_index` is not used, and `nd_2d_push`/`nd_2d_pop` keep the RDP state, element and anchor around it so
the game's draws after it see what they always did. The ImGui overlay (as the minimap) was the other choice; it
would have needed the game's textures copied into the host and drawn at another scale and filter than the game's 2D,
so it would not match the plates and fonts the game shows next to it.

- The game's spell rows (`Hud_DrawSpellInput`) still run every frame they always did (their timers and side effects
  are game state); `Port_SpellUiReplaceBegin/End` mute their native items (`nd_2d_mute`) and their mouse targets
  (`Port_MouseMute`).
- Anchors: the panel CENTRE, the bar and the HUD's key digits LEFT (`nd_2d_element` ids 0x20-0x3C; a name's id carries
  a hash of the text so a changed name never slides into the old one in the pictures between ticks).
- Every piece uses one blended render mode (0x504240, the game's own spell-panel fade) and one combiner (texture x
  ENV, or ENV with the texture's alpha for flat colours and the dull crystal): no new GPU pipelines
  (`tools/warm_test.py --check`).
- Animations, per tick (30 a second): the panel fades in over 4 ticks while it slides up 2 pixels and fades out over
  3; the bar fades over 5 on the enemy's turn; the name box shows after 7 ticks of hovering and fades in over 3; a
  held crystal or slot moves down a pixel. The cast confirmation is the game's own spell name plate.

### The quick bar's file (per save)

As the area map's fog of war (C:\code\el-map2, docs/AUTOMAP.md, same idea, separate code): the save's record is the
key. `SaveMenu_CaptureGameState` (the save menu's save, an F2 quick save) and `SaveMenu_RestoreGameState` (load,
Continue, F4) call `Port_SpellUiSaveEvent`, which hashes the record (FNV-1a 64 of the play time, day, map and
sub-area as 32-bit numbers, then the SaveGameStateBlock) and writes or reads `spellbar/save_<key>.bar` in the data
folder (text: `ELTSPELLBAR 1`, `page N`, `slot I E [E [E]]`; written to a .new file and renamed). A change to the bar
is written at once to the current save's file; a new game keeps it in memory until its first save; the title and
the intro forget it. A save copied to another Pak slot keeps its bar. A load with no file (every older save) starts
empty. The save formats are unchanged.

## Aiming (the hook for later)

- `Port_SpellUiReady()`: a spell is ready on the plate (elements in and the game would cast it now): its id, else -1.
- `Port_SpellUiCast()`: cast it (A at the next read; the same path as the plate's click).
- `battlefield_click(x, y)` in spell_ui.c is called for a left click that hits none of the UI while a spell is
  ready; it takes nothing today (a click on the battlefield does not cast: spec Q7). Click-to-aim fills it in: turn
  the point into the spell's direction (docs/SPELL_TARGETING.md), then `Port_SpellUiCast()`.

## Answers to the spec's open questions

1. The plates under each crystal (the owner's image 22), with the crystals spaced 74 apart while the panel is open;
   the HUD row stays where it is.
2. Every prefix is a spell (above).
3. Shown in the field with the field spells usable and the rest greyed.
4. Slots start empty.
5. Our own 3 x 5 digits (the ROM has none that small).
6. The game's textures (above).
7. No click on the battlefield to cast; casting is the plate, a slot, or the game's A. The hook is left.
8. Kept clear of the 24 % minimap (above).
9. No descriptions yet: the name box shows the name, the gems and the MP; `spell_description()` is where they go (a
   line is added to the box when it returns one).
10. `Magic_GetSpellRestrictionFlags` (above), called, not copied.

## Tests

`python tools/spell_ui_test.py --exe out/p1-windows/eltale-p1.exe --rom ROM [--saves C:/code/eltale/out/bugsaves]`:
cases from battle-stuck's quick save with more spirit levels (`spells`: 12/10/25/8, MP 30) or little MP (`lowmp`: 2),
each at 4:3 and 16:9 with pictures (`out/spell_ui_test/<case>-43|169/pictures`), once more without pictures, and once
with the controller's presses alone (the UI off): the same lines, the same sound, the same last frame list.

- `field`: the HUD's water crystal clicked (the panel opens), earth's plate clicked (Healing), a right click (taken
  back), earth again, kept in a slot, the plate clicked (Healing cast in the field), the slot clicked and its key
  pressed (cast twice more: MP 30, 28, 26, 24).
- `battle`: a battle; the panel opened from the HUD crystal, Water twice, a right click, kept in slot 5, the plate
  dragged onto slot 2, cast; next turn a slot dragged onto another (swapped), the wheel over the bar and the page key,
  a slot clicked (Water Pillar cast from the bar).
- `lowmp`: MP 2, a bar file made for the save beforehand: the slots' states (`xcmxc`), the crystals with one element
  in (all taken) and with two (none: 3 MP), Esc taking both back, a slot short of MP refused.
- `persist`: a spell kept, an F2 quick save, then a new process: the F4 load brings the bar back and its key casts it.
- `window`: the host's path in a hidden window (ELTALE_INPUT_TEST), with mouse look: Tab on, the key 3 opens the
  panel (mouse look goes off: while it is on the pointer is the camera's, so a key or a C button opens the panel), a
  click on a next-spell plate, a right drag (undo, the view does not turn), a click on the plate (cast on the
  release), then mouse motion with the panel closed: mouse look is still off.

`ELTALE_SPELLUI_LOG=1` prints `P1_SPELLUI_PRESS` (what was pressed and why), `P1_SPELLUI_STATE` (the spell input
after it), `P1_SPELLUI_VIEW` (shown/live/building, the crystals the game would take, each slot: c castable, m MP or
not learned, x not here, + keep here, - empty), `P1_SPELLUI_SLOTS`, `P1_SPELLUI_SAVE`, `P1_SPELLUI_CLICK`,
`P1_SPELLUI_KEYS`, `P1_SPELLUI_MACRO`, `P1_SPELLUI_SLOT_REFUSED`.

## Not done

- Click-to-aim (the hook is above).
- Descriptions for the name box.
- The minimap moved to a top corner can cover the panel's right end at 4:3.
- In Japanese the hint and the labels are left out (the HUD font has no Latin letters there); the names are the
  game's own.
