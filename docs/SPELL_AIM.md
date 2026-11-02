# Click to aim spells

In a battle, with a spell put together and ready to cast, point at a monster or at the ground: the spell's area shows
on the battlefield and every monster it would hit gets a gold ring at its feet. A left click turns you to face that way
and casts. The spell keeps its own reach and rules from the game: the mouse only picks the direction. To get closer,
move inside your ring first, as always. Right click takes the last element back.

Code: `port/p0/spell_aim.c`, `port/include/port/spell_aim.h`. The hooks are in `patches/p0.patch` (one line in
`Player_StartCastSpell`, one in `Hud_Draw`), `port/os/controller.c`, `port/p0/mouse_camera.c` (the raised camera) and
`port/p0/spell_ui.c` (the folded panel). The research behind it is [SPELL_TARGETING.md](SPELL_TARGETING.md).

The F1 menu's Controls page has two switches:

- **Click to aim spells**: on by default (settings.json `controls.spell_aim`).
- **Raise the camera while aiming**: on by default (`controls.spell_aim_camera`).

Headless runs have aiming off unless `ELTALE_SPELL_AIM=1`. `ELTALE_SPELL_AIM_CAMERA=0` keeps the camera down.

## Using it

1. Put a spell together: the spell panel's crystals, the keys 1-4, the C buttons, or the HUD's crystals.
2. Move the pointer off the panel onto the battlefield.
   - **The panel folds** to the spell's plate (gems, name, MP) at the top right, with the hint *Click to cast ·
     R-click to undo*. The battlefield is clear.
   - **The camera rises** over the battlefield (below).
   - The preview follows the pointer:
     - the spell's area on the ground: for a spell that moves, its tunnel, the ground where it can touch a monster
       (below);
     - a white chevron at your feet for the facing the click would give;
     - a gold ring under each monster it would hit, one more ring for each extra bolt or arrow that hits the same
       monster.
3. Click a monster or the ground: you turn that way and cast.

What else works while aiming:

- **Click the plate**: casts as the game would, with the facing you have now. It is the only way for self spells.
- **More elements**: keys 1-4, the C buttons or the HUD's crystals. The plate stays folded.
- **Right click**: takes the last element back (the game's B). The full panel opens again and stays open until the
  pointer has moved onto it (to pick another crystal) and off it again.
- **Esc**: takes every element back.

The pointer aims anywhere on the battlefield except over the panel and over the game's own targets (the HUD's crystals).
While the panel is open in full, its area (the middle of the picture from y 86 down to the bottom: the panel, the quick
bar and the HUD) belongs to the panel, so going from a HUD crystal up to the panel never starts aiming on the way. Once
the panel has folded, the whole battlefield aims.

With the spell UI off (the game's own spell panel) aiming works the same way: nothing folds, and the camera rises as
soon as the pointer is on the battlefield.

## What each spell shows

The kind comes from the spell's own record (`SpellDef`). `ELTALE_SPELL_AIM_LOG=2` prints every spell's kind
(`P1_AIM_SPELLDEF`).

| Kind | Spells | The click | What is drawn |
| --- | --- | --- | --- |
| self | Power Rod, Spirit Armor, Healing, Vampire Touch, Avoidance, Wind Walk, Confusion Magic, Magic Barrier, Invalidity, Escape, Exit, Return | no aiming: click the plate | nothing; the panel never folds |
| whole arena | Rock Shower (area 1000), Wide Weakness, Soul Search Lv2, Restriction Lv2, Silence Lv2, Ultimate Wind | casts; no facing is set | a ring under every monster |
| round you | Hot Steam Lv1/Lv2 (15/30), Ice Gas (15), Wind Bomb (15) | casts; no facing is set | the circle round you; the monsters it reaches are ringed |
| random round you | Rock Combo | casts; no facing is set | the band 20..39 from you where the rocks fall. Where they fall is random (`Math_Rand`), so no monster is ringed |
| a point ahead | Water Pillar Lv1/2/3, Fire Pillar | faces the point | the circle where it stands, at its fixed distance |
| on a monster | Compression, Extinction, Weakness Lv1/Lv2, Magnet Rock, Soul Search Lv1, Ice Knife, Drain Magic, Restriction Lv1, Silence Lv1, Slow Enemy | faces the point; the game picks the monster nearest that bearing | its ring |
| a line | Walking Water, Cyclone, Large Cutter, Magma Ball (three, at 0 and 45 degrees each side), Rolling Rock Lv1/Lv2 (across the front) | faces the point | each one's tunnel |
| a lob | Fire Bomb, Rock Lv1/2/3 | faces the point | a thin arc for its flight, and its tunnel where it can touch a monster: Rock's lower flight and where it lands, Fire Bomb's blast only |
| a fan of bolts | Fire Ball Lv1/2/3 | faces the point | the bolts' cone (how far they can reach, turning), each bolt's tunnel inside it |
| homing | Homing Arrow Lv1/Lv2, Wind Cutter Lv1/2/3 | faces the point | each arrow's or cutter's tunnel |

An area or tunnel that would hit nothing is grey; one that would hit is light blue (each cast's tunnel on its own).

### Tunnels

A moving spell's hit test (`Magic_FindTargetInRange`) runs once a tick, where the effect is then. The tunnel is the
ground it can touch: round each place where the test runs, a disc whose radius `w` is how far past that place a
monster's edge can be and still be touched,

```
w = sqrt((extent + r)^2 - dy^2) - r     dy = (the monster's middle - the effect's height) / 2
```

for a monster of the battle standing on the ground there (its radius r, its middle's height above its feet; with
several kinds of monster, the widest). So **a monster whose edge reaches the tunnel is hit, and one outside it is
not**:

- **Width.** On the ground (the lines, Fire Ball at chest height) `w` is about the spell's extent. High up it narrows,
  and above `extent + r` from the monster's middle (twice that in height, as dy is halved) it is nothing: there only a
  thin arc is drawn, at the effect's height.
- **Ticks.** The test runs at each tick's place only, so a fast, thin effect (a Homing Arrow, 12 a tick, extent 2) is
  a string of beads: a monster between two of them is missed. The band's sides follow that exactly: along the path its
  half-width is where a monster beside it is touched (the discs of radius `w + r` round the places, less r); past its
  ends it is the end's own disc.
- **When.** Only where the test runs: not during a spell's start delay (Rock's wait over the caster's head), for Fire
  Bomb (`Magic_FindTargetInRangeIfLanded`) only after it lands, and a spell that is not piercing ends at its first hit
  (the path ends there).
- **Each cast** has its tunnel (Magma Ball's three, Wind Cutter's six), blue when that cast hits.

**Rock's flight does hit monsters on the ground**, once it is low enough. It waits over the caster's head untested,
then falls for about 15 ticks, testing every tick, and stays 8 more where it lands:

| Spell | Extent | Where its flight can touch a monster on the ground (r 5.6, middle 5.6 up) |
| --- | --- | --- |
| Rock Lv1 | 5 | from about 24 above the ground down: the last 9 of its 15 flying ticks, about the last 30 of its 50 |
| Rock Lv2 | 10 | its whole flight (from 32 up), 2 wide at first, 10 as it lands |
| Rock Lv3 | 15 | its whole flight (from 37 up), 7.4 wide at first, 15 as it lands |

In play, Rock Lv3 aimed between the two monsters hit the one it was not aimed at in the air, 20 above the ground
(`rock3_flight`), and Rock Lv1 the same way passed over it (`rock1_flight`), each as its tunnel showed. **Fire Bomb
hits nothing in flight**: its tunnel is only its blast, 18 round where it lands (49 ahead), for 9 ticks.

### Cones

Fire Ball's bolts home (strength 2, acceleration 1, the top speed 10): a bolt flies at the monster nearest its own
heading (no range limit). Its cone is the ground it can reach and touch whatever monster that is: each bolt, from
its real launch, run toward a monster of the battle's kind put at every place round the caster (every 5 degrees,
every 5 units out to 30 past its straight reach), and every tunnel disc of every run put together. It is kept as
the nearest and furthest covered distance along 240 bearings from the bolt's first place.

The bolts start slow (2.2 a tick) and turn fast, so they can turn right round within their 9-11 ticks: **the cone is
a lopsided disc, not a narrow wedge**. Measured from where a bolt starts (9.5 ahead of the caster) to the edge of a
monster it can touch:

| | ahead | 45 degrees off | 90 off | behind |
| --- | --- | --- | --- | --- |
| Lv1 (9 ticks) | 51 | 47 | 39 | 27 |
| Lv3 (11 ticks) | 66 | 61 | 51 | 38 |

The bolts of Lv2 and Lv3 start at the same place and their cones nearly coincide: they are drawn as one area (their union), faintly, and each bolt's predicted tunnel
inside it, stronger.

- A monster outside the cone is never hit by the bolt. One inside it is hit when it is the bolt's monster (nearest
  its heading) or in the way of the bolt's path, except within about a unit of the far edge (the cone is every
  tunnel of every run; a monster just inside may need a path to some other place to be touched).
- So with one monster left, Fire Ball within its cone hits it whichever way it is pointed.
- Building a cone is about 1400 runs of a bolt (8 ms for Lv3's three): it is kept for each cast and turned with the
  facing about the caster, and built again only when the spell, the caster's place or the monsters' kinds change
  (one bolt a read, the others follow at the next reads). The kept cone turned differs from one built for that facing
  by the ground's slope under it only (0.55 at most on battle-stuck's field). The cast's logged cone is always built.

Wind Cutter and Homing Arrows home far harder (strength 10) for 25-30 ticks and reach 90 to 160 (SPELL_TARGETING.md):
their cones would cover most of the arena round the caster and say nothing. They show each cutter's and arrow's tunnel only.

## How it works

### The read (`Port_SpellAimRead`, each controller read)

It runs after the menus' pointer (`port/os/controller.c`: the spell UI, then `Port_MouseMenuRead`, then this).

- **When.** A spell must be ready:
  - in a battle (`battle_flags & 1`), in play, with no menu or text;
  - the spell input open with 1-3 elements in;
  - `Magic_GetSpellRestrictionFlags` 0 (learned, MP, a battle spell);
  - not a self spell.
- **The pointer.** It must be over the picture and not over the game's targets (`Port_MouseOverTarget`, new in
  mouse_menu.c) or the spell UI (`Port_SpellUiPointerOver`: its pieces, and the open panel's area).
- **What it points at.**
  - **A monster:** the pointer is on a live monster's body on the picture (`body_on_screen`): a column from its feet to
    its top, 0.6 of its radius wide each side (its model is slimmer than the radius the hit test adds), its feet end
    taken down to the near edge of its footprint (where its legs are drawn). It is picked within 1 unit of that box
    (about 4 pixels at 1080p) and, once picked, stays picked to 3 (no flicker at the edge). Just off its body, or above
    its head, is the ground: pointing past a monster aims at the ground there freely. (The pick used to take the
    radius and 3 more round the whole column: 12 units each side for a Were-Rabbit, where now 6.4 picks and 8.4
    keeps.) The bodies are projected with the view the last picture was drawn with. That view is kept by `Port_SpellAimViewDrawn` at the end of the frame
    build, before the mouse camera puts the game's eye back, so it includes the turned and raised eye and the
    widescreen perspective (`camera_view_mtx` x `camera_proj_mtx`, `ND_ASPECT`). The pointer is in CENTRE units, which
    are the same at every aspect.
  - **Else the ground:** the pointer's ray on the plane of the player's feet.
- **The facing.** `Math_Atan2` of the point from the player, for the directional kinds. The preview is that spell cast
  with that facing (below). Without an aim the preview is drawn for the facing the player has.
- **A left click** keeps the point (a monster's slot or a ground x, z), takes the click from the menus' pointer and
  presses A for this read. The game's own `Player_HandleSpellInput` then casts as the controller's A would, with its
  checks and MP. A kept point that was not cast at this read (`P1_AIM_DROPPED`) is forgotten at the next.

### The cast (`Port_SpellAimApply`, in `Player_StartCastSpell`)

If this read's click kept a point and the spell being cast is the one that was aimed, the caster's facing becomes
`Math_Atan2(point - caster)`. For a monster the point is where it is at that moment. Nothing else is set:

- no target index, no distance, no position;
- the place, launch, target choice and repeats in `Magic_CreateCastEffect`, 18 ticks later, read that facing;
- Homing Arrow and Fire Ball's side bolts still choose their own targets;
- with no click, `rot` and every hash are as before.

The player could have turned there with the stick before building the spell. Turning is free on the player's turn (see
SPELL_TARGETING.md, "Fairness").

### The preview: a copy of the game's spell code

`simulate()` runs the spell on a local copy of the effect, with the same expressions as the decomp, float and double
steps alike:

- `Magic_CreateCastEffect`'s launch, with the 50/50/50/50 bonus's extra casts and longer life;
- the target choice: `Magic_SelectClosestAimTarget` (its bearing test, by the launch velocity for the V spells),
  `Magic_SelectFirstActiveTarget` and `Magic_TargetCaster`;
- the place handlers;
- `Magic_MoveHomingEffect`, `Magic_MoveVerticalEffect`;
- `Magic_FindTargetInRange` (the ellipsoid: dy to the monster's middle, halved, against `extent + radius x scale`),
  `Magic_FindEligibleTarget` and `Magic_FindTargetInRangeIfLanded`, each effect with its own hits as its slot's
  `magic_hit_slot_bits`.

It runs every cast of the spell (`cast_count`, `repeat_interval`, the launch direction stepping) for its delay and life.
The result: each effect's path, where it lands, what it touched, and how near each monster came (`near=`: the hit
test's distance less the reach; under 0 is a hit).

The real functions can't be called for a preview:

- `Magic_MoveHomingEffect` shakes the camera and makes the ground-hit effect when it lands;
- `Magic_CreateCastEffect` takes an effect slot and changes the lights;
- `Coll_ResolveFloorAndWalls` writes `coll_push_radius` (and reads a grid cell before it checks the bounds).

The floor is the same walk over the grid's lists, with the spells' flags 0xA00, calling `Coll_TestObjectFloor`, which
writes only its result. `Math_Rand` is never called.

The preview assumes the monsters stay where they are and alive. It differs from the game only when an earlier bolt
of the same spell kills or knocks back a monster that a later one would have met. A Wind Cutter that kills its target,
for example, makes the next cutter fly on.

### Drawing (`Port_SpellAimDraw`, in `Hud_Draw`)

The preview is world-space triangles, drawn through the native renderer's new overlay (render.h v10,
`ElRenderApi.overlay`, `nd_2d_world_tris`):

- **Recorded with the tick.** Each item is a run of vertices in world units with their colours, in the recording's
  order. Here that is after the 3D, before the HUD's 2D.
- **Drawn through each picture's camera.** At 240 pictures a second the areas stay on the ground while the camera moves
  between ticks. The overlay is the earlier tick's, as a 2D mesh is. They are drawn at any resolution, MSAA and aspect
  as the 3D is.
- **One material.** The game's ZB_XLU_SURF blending (depth-tested, not written) with the shade as the colour. It uses
  the program and pipeline state the game's own translucent effects use: no new shader. `warm_test --check` checks the
  pipeline list.
- **Shapes follow the ground.** Circles, tunnels, cones and lines are cut into small pieces whose corners sit 0.45 above
  the floor under them: the same floor query, kept on a 2-unit grid while a spell is being aimed (a corner's height is
  between its four nearest), so the drawing costs about 0.1 ms a tick.

Monsters glow with a gold ring at their feet, a pulsing fill inside it, and one more ring for each extra hit.

### The folded panel and the raised camera

- **The panel.** `Port_SpellAimCollapsed()` is true while aiming. The spell UI then draws only the spell's plate (184 x
  14 at x 122, y 9, CENTRE: right of the HP block, which ends at x 116 at 4:3) and its hint. The plate is its only
  target there. The enemy's info panel (x 181 on, y 26 on) shows only on the enemy's turn. The quick bar stays.
- **Aiming starts** when the pointer is on the battlefield outside the open panel's area.
- **It stops** at:
  - the cast;
  - an undo (the panel stays open until the pointer has moved on it and leaves it: `P1_AIM_PANEL collapsed=0
    why=undo`);
  - a self spell;
  - the spell input closing.
- **The camera** (`Port_SpellAimLiftWanted`, `aim_lift` in mouse_camera.c):
  - Over 8 ticks it eases to a pitch of 47 degrees outdoors (38 indoors, for the ceilings).
  - The point looked at moves 32 units ahead along the view, so the monsters ahead stay in the picture.
  - It comes back down the same way after an undo. At the cast the game's focus view (`Camera_StartBattleFocus`)
    takes the camera at once and the raise goes with it, a cut: easing down over a camera the game is moving itself
    blended badly in the pictures between ticks (`interp_test.py aim` flagged it).

### Why the raised camera is clean

The camera is drawing only, and the raise keeps the eye's x and z, the yaw and the stick's `followYaw` as they are. It
changes only the eye's height and the x, z of the point looked at. These are the reads of `camera_state` in a tick's
game logic (the grep in game/), and how each sees the raise:

- **The stick.** Player_StateMove turns the stick by `followYaw` (the mouse camera adds its own turn). The raise does
  not touch it.
- **The emitters.** `Effect_QueueMapObjEmitter` takes Math_Rand when an emitter comes into view, so the game's random
  numbers depend on which ones are seen. It tests a half-plane from the eye's x, z and the yaw. Unchanged.
- **The map.** The map's grid search is round the eye's x, z. Unchanged.
- **The billboards' yaw.** Unchanged.
- **Drawing only.** `Camera_WorldToScreen` places the floating numbers and markers (the one game use, the NPC marker
  that places a text window, is in the field). The billboards' aim-at-the-eye and the sky are drawn.

Like the mouse camera, the raise is applied in `Camera_Update` after the mode's handler and undone at the end of the
frame build (`Port_MouseCameraRestore` puts back the eye and now also the point looked at). The game's camera never sees
it.

The ticks are marked `EL_RDCAM_ORBIT`, so the pictures between ticks move the eye round the point looked at. The raise
is smooth at 240.

`tools/spell_aim_test.py lift` casts the same aimed Fire Ball with the camera raised and not: the same aim lines, hits
and game sound.

## The game's hit rule, and the glow

A monster glows when the copy of the game's code says one of the spell's effects touches it. That is the very test the
game makes each tick, including each monster's radius and the halved height difference.

- **Hot Steam.** A monster half in and half out of it glows exactly when the game will hit it. The test checks it 1.3
  inside and 2.0 outside the reach.
- **Misses.** A touched monster can still dodge: the game's hit roll, `Magic_GetHitOutcome`, is not part of the preview.
- **Magic Barrier.** A monster under it is touched but takes nothing.

## Verified in play (tools/spell_aim_test.py --verify)

Every spell that aims (43 of the 60) was cast at a monster from battle-stuck's save (spirit levels 48). Each game effect
was checked every tick against the copy's path (`P1_AIM_FX` against `P1_AIM_SIMPATH`), with what it hit
(`P1_AIM_HITS`):

- **Paths.** All match to 0.00. The only exceptions are Wind Cutter Lv2/Lv3: its first cutter killed a monster that the
  copy had a later cutter meet.
- **Hits.** Predicted and actual are the same for every spell.

The numbers SPELL_TARGETING.md left unverified, measured (player at 398.3, -1629.4, facing a bat 72.6 away):

| Spell | Measured | The research said |
| --- | --- | --- |
| Fire Ball Lv1 | the bolt starts 9.5 ahead; its last tested place is 55.2 from the caster after its 9 ticks, so it reaches a 5.6-radius monster up to ~66 straight ahead (the bat at 72.6: missed) | ~65: right |
| Fire Ball Lv2 | 62.4 after 10 ticks (~73) | ~70 |
| Fire Ball Lv3 | hit the bat at 72.6 | ~80 |
| Fire Ball's side bolts | Lv2's second bolt goes 45 degrees to the caster's right, Lv3's third 45 to the left | "to one side" |
| Rock Lv1 | appears 8.5 ahead, 27.4 up; aimed at the bat 72.6 away it lands 60.1 ahead (the pull toward the bat's middle brings it down early): missed by 1.8 | "lands about 72 ahead with no pull"; that never happens, it always has a target |
| Rock Lv2 / Lv3 | land 67.7 / 70.7 ahead at that bat: both hit | 10..85 / 10..100 |
| Rolling Rock | starts on the caster's **right** (52.5 right, 35 ahead) and rolls to the left; it rolls **~275** in its 50 ticks (top speed 7) | "one side"; ~220 |
| Walking Water | starts 13.5 ahead after 6 ticks, goes **~219** | ~233 |
| Fire Bomb | thrown from 21.5 ahead, lands **49.1** ahead after **18** ticks | ~52 after 11 |
| Water Pillar 1/2/3, Fire Pillar | 8.5 / 13.5 / 18.5 / 11.5 ahead | the same |
| Large Cutter, Cyclone, Magma Ball | 50.9, 106.2, 91.5 | 51, 106, 91 |
| Rock Combo | the rocks fall 20..39 from the caster | the same |

## Item spells

Items that cast spells (`Item_UseSpell`) call `Magic_CreateCastEffect(..., 0, 4)` with the same facing (`Actor.rot`).
They could take the same hook later, in `Player_StartUseItem`. Not done.

## Determinism and the hashes

- **Input on the game's thread.** Everything is controller input and drawing on the game's thread, from the pointer at
  the read.
- **Replays.** An input script's `mouse VI 0 0 0 0 X Y` and `mouse VI 0 0 0 2` rows replay an aim exactly.
- **What the game sees.** The aim sets one thing, the facing at the cast. Without a click nothing is pressed or set.
- **Read only.** The preview and the drawing only read game state. They call only `Math_Atan2`, `Math_RotateVec2`,
  `Coll_TestObjectFloor`, `Magic_LookupSpellSequence` and `Magic_GetSpellRestrictionFlags`, all of which only return
  values.
- **No mouse, no change.** With aiming on and no pointer, a run ends on the same frame list and sound as with it off
  (`idle` case).
- **The checks.** `tests/p1_checks.sh` and the picture baselines run with it off.

## Logs (`ELTALE_SPELL_AIM_LOG`)

| Level | Line | What |
| --- | --- | --- |
| 1 | `P1_AIM_HOVER` | the preview when what it hits or its facing changes: the kind, the facing, `hits=` (a bit per monster slot), `counts=`, each cast `cN=t<target>,start>end[,land],reach,hit` and `near=` |
| 1 | `P1_AIM_SCREEN` | the player and each monster on the picture (CENTRE units) |
| 1 | `P1_AIM_PANEL` | `collapsed=1 why=aim`, `collapsed=0 why=undo / self / not-ready` |
| 1 | `P1_AIM_CLICK` | the point, the facing, `predicted=` |
| 1 | `P1_AIM_APPLY` | `rot=before>after` |
| 1 | `P1_AIM_CAST` | the copy run again for the cast's facing |
| 1 | `P1_AIM_ENEMIES` | positions |
| 1 | `P1_AIM_HITS` | `predicted=` against `actual=` (from the effects' slots and the monsters' `magic_hit_slot_bits`): match, DIFFER, or random (Rock Combo) |
| 1 | `P1_AIM_TUNNEL` | at the cast, each cast's tunnel as drawn: `land=` (the place it landed) and each live place `k:x,y,z,w;` |
| 1 | `P1_AIM_CONE` | at the cast, each cone: its start and each covered bearing `bin:nearest:furthest;` (240 bins) |
| 1 | `P1_AIM_HITAT` | each hit as the game made it: the effect's slot and place, the monster and where it stood |
| 1 | `P1_AIM_COMPASS` | with `P1_AIM_SCREEN`: the ground 40 from the player every 5 degrees (`b<deg>=x,y`) and each monster's body as the pick takes it (`body<i>=feet x,y,top x,y,half-width`) |
| 2 | `P1_AIM_COST` | at the cast, what a preview costs: built (with its cones), turned (a kept cone), the drawing, and the kept cone's difference from one built |
| 2 | `P1_AIM_SPELLDEF` | the table |
| 2 | `P1_AIM_FX` | the game's live effects each tick |
| 3 | `P1_AIM_SIMPATH` | the copy's paths at the cast |

## Tests

`python tools/spell_aim_test.py --exe EXE --rom ROM [--saves C:/code/eltale/out/bugsaves]`.

The save is battle-stuck's quick save with spirit levels 48 and MP 60 (a copy). Each case:

1. A probe run of its script without the click finds where the target is on the raised view (`P1_AIM_SCREEN`).
2. The case then runs at 4:3 and 16:9 with pictures at 3x.
3. It must print the same aim lines and the same sound at both aspects, and the preview at the click must equal both the
   cast's copy and the game's hits.

| Case | What |
| --- | --- |
| pillar_at, pillar_away | Water Pillar Lv3 from the same place (turn 2), at a monster 19 away (hit) and away from it (missed). The game's pillar stands at the preview's circle (within 0.11); the two places differ |
| auto_far | Weakness Lv1 on the monster further away, which the player's facing would not pick: it is hit, the other not |
| steam_in, steam_out | Hot Steam Lv1 with the first monster 1.31 inside its reach (it glows and is hit) and, after a walk aside, 1.96 outside (it does not glow and is not hit) |
| line | Walking Water at the far monster: hit |
| fireball | Fire Ball Lv3: three bolts, both monsters ringed and hit |
| undo | Water Pillar Lv2 folded; a right click (Lv1, the panel opens); the pointer on the battlefield (it stays open), onto the panel and off (folds); cast |
| noui | the spell UI off: the game's C button, then the aim (nothing folds) |
| homing, lob, bomb, sweep, magma, random, arena, cutter | the other kinds' pictures and hits |
| lift | the same Fire Ball with the camera raised and not: the same game |
| idle | aim on with no pointer against aim off: the same frame list and sound |

`--verify [SPELL ...]` runs the check against the game described above, for every aiming spell, and for every moving
one the tunnel check below.

The tunnels and cones (`t_*`, `cone_*`, `*_flight`; all at 4:3 and 16:9): each moving spell cast at a monster and at a
bearing that passes one by (`b:DEG`: the pointer on the ground 40 from the player at that bearing, from
`P1_AIM_COMPASS`). The checks use the shapes the cast's copy logs (`P1_AIM_TUNNEL`, `P1_AIM_CONE`), the monsters at
the cast and the game's own hits (`P1_AIM_HITS`, `P1_AIM_HITAT`):

| Check | What |
| --- | --- |
| tunnel | every monster is hit exactly when its edge is inside the tunnel (`e0=-4.21 hit`: 4.21 inside), and each hit came from a live place of the tunnel, with the monster inside that place's disc. After a kill (Wind Cutter Lv2/Lv3, below) the hits up to it |
| cone | every monster hit is inside the cone |
| outside / off_line | the monster is outside the cone; or well off its bolt's straight line (by more than the bolt's reach and 2) |
| flight | the monster was hit while the rock was in the air exactly when the tunnel before the landing reaches it |

| Case | What |
| --- | --- |
| t_rock1, t_rock1_side, t_rock2, t_rock3_side, t_bomb, t_water, t_cyclone, t_cyclone_side, t_cutter, t_magma, t_roll1, t_roll2, t_fire1/2/3, t_wind1/2/3, t_arrow1/2 | every moving spell: tunnel (and cone for Fire Ball) |
| cone_in1, cone_in3 | Fire Ball Lv1 facing 75, Lv3 facing 90: the near monster, 18 and 33 degrees off a bolt's heading, is that bolt's: it turns and hits it, 19 and 34 off its straight line, inside the cone |
| cone_out1, cone_out3 | Fire Ball Lv1 facing 327 (its monster 68 degrees off, 70 from its start) and Lv3 facing 215 (both monsters behind, about 72): outside the cones, missed |
| rock3_flight, rock1_flight | Rock Lv3 and Lv1 facing 46, between the monsters (the far one its target): Lv3 hits the near one in the air, Lv1 passes over it |
| pick | Water Pillar Lv2 aimed with the pointer on the far monster's body (picked), 2 units off its side (kept), 4 off (the ground), 2 off again (still the ground), 2.5 above its head (the ground), on it again (picked) |

The pictures are in `OUT/pictures/<case>-<43|169>-<n>.png`: n 0 is the preview just before the click, n 1 the cast.
The undo case has four: folded, open after the undo, the pointer on the panel, folded again.

## Not done

- Item spells (above).
- Cones for Wind Cutter and Homing Arrows (they would cover most of the arena; their tunnels are drawn).
- Rock Combo's monsters are not ringed: where its rocks fall is random.
- The preview does not know about kills by an earlier bolt of the same spell (above). Wind Cutter Lv2 and Lv3 at
  battle-stuck's monsters show it: the first cutter kills the near monster, so the later cutters' real paths leave
  their tunnels (in the tests the monsters hit were still the ones predicted, by other cutters).
- 3D spell paths (tried and removed, 2026-11-02): the hit volumes drawn in the air as see-through tubes and domes
  (render.h v11 rim vertices, a "3D spell paths" Controls setting). In play the hit volumes looked far too big in 3D
  and covered the battle, so the owner chose the ground drawing alone; the branch history (57e8a1e, reverted) keeps
  the code.
