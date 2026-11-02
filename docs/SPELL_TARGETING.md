# Spell targeting: how every spell aims and hits, and how to aim it with the mouse

This is the research for "click to aim spells", which is now implemented: see [SPELL_AIM.md](SPELL_AIM.md). It covers
every spell the player can cast, in battle and on the field, for all four elements and all levels. The rules come from the decomp
(`game/magic/magic.c`, `game/player/player.c`, `game/battle/battle.c`). The numbers come from the spell tables in
the ROM's magic block. The ranges were worked out with a Python copy of the game's movement code (see
"How the numbers were found"). They have since been checked in play against the game's own effects, every tick, for
every aiming spell (SPELL_AIM.md, "Verified in play"). The corrections are marked **(verified)** below.

Units are world units. For scale:

- The player's radius is 3.5 (`body_radius` 50 × `scale` 0.07) and their height is 11.2 (160 × 0.07).
- A Were-Rabbit's radius is 5.6 (80 × 0.07), from `ELTALE_BATTLE_LOG`.
- A random battle's arena radius is 100 (`battle_arena_radius`). Bosses use `battle_boss_arena_radii`, 85 to 190.
- The player's move ring each turn is `agility × 0.2857 + 17` from where the turn started: 18.4 at agility 5 and
  31 at agility 50. Wind Walk doubles it and Slow Enemy halves it (`ActorBattle.unk90`).

## 1. The spell machinery

### The spell record

A spell id is `element << 8 | index`:

- element 0 is Fire (C up), 1 Earth (C left), 2 Water (C down), 3 Wind (C right);
- index runs 0..14 (`Magic_LookupSpellSequence`);
- element 4 holds the enemies' spells.

Each spell is a `SpellDef` row (`D_800C185C[element][index]`). These fields decide where it goes:

| Field | Use |
| --- | --- |
| `flags` | bit 0 battle, bit 1 field. Bit 2 (4) *piercing*: the effect lives on after a hit and can hit other actors (each actor once per effect). Bit 3 (8): the target is chosen along the effect's launch velocity, not the caster's facing. |
| `target_select_handler_index` | Who the spell is "for", decided once at cast: 0 `Magic_SelectClosestAimTarget`, 1 `Magic_SelectFirstActiveTarget`, 2 `Magic_TargetCaster`. |
| `place_handler_index` | Where the effect starts (`magic_place_handlers`, 10 functions, below). |
| `move_handler_index` | 0 `Magic_MoveHomingEffect` (a moving, possibly homing, projectile). 1 and 2 `Magic_MoveVerticalEffect` (stays where placed; only y moves). |
| `find_target_handler_index` | What it hits each frame: 0 `Magic_FindTargetInRange` (a volume test), 1 `Magic_FindEligibleTarget` (the chosen target, guaranteed), 2 `Magic_FindTargetInRangeIfLanded` (the volume test, only after landing). |
| `effect_extent` | The hit radius added to the victim's radius. Also the placement offset. |
| `effect_lifetime`, `start_delay` | Frames the effect lives; frames it waits first. |
| `cast_count`, `repeat_interval` | Extra casts (`magic_cast_repeats`), every `repeat_interval + 1` frames. Cast *n* uses launch direction `(launch_direction_index + n) & 7`. |
| `launch_direction_index`, `launch_speed_index` | Initial velocity, for move handler 0 only: `D_800C1878[dir] × magic_launch_speeds[i]` ({2, 3, 4, 6, 7}), rotated by the caster's `rot`. |
| `homing_strength_index`, `homing_accel_index` | `magic_homing_strengths` {0, 1, 2, 10, 10} and `magic_homing_accels` {0.4, 0.5, 0.7, 1.0, 1.2}. |
| `launch_flags` | Bit 0: gravity, `vy -= 0.3` each frame. Bit 1: launched flat (`vy = 0`). |
| `floor_collision_mode` | 0 no floor. 1 stops (lands) when below the floor, then lives 8 more frames. 2 stuck to the floor (y = floor). |

`D_800C1878`, the 8 launch directions, as (x, y, z) in the caster's frame. Forward is +z. `Math_RotateVec2`
mirrors x, so cast 1 and cast 2 go 45° to opposite sides. **(verified)** The caster's +x is their right: direction 1
and the diagonal place (8) are on the caster's right, direction 2 on their left:

| Index | Direction |
| --- | --- |
| 0 | (0, .5, 1): forward and up |
| 1 | (.7, .5, .7): 45° to the right |
| 2 | (-.7, .5, .7): 45° to the left |
| 3 | (1, .5, 0): sideways |
| 4 | (-1, .5, 0): sideways, the other way |
| 5 | (.7, .5, -.7): back diagonal |
| 6 | (-.7, .5, -.7): back diagonal, the other way |
| 7 | (0, .5, -1): back |

### Placement (`magic_place_handlers`)

The second argument is always the caster:

- 0 at the caster's feet;
- 1 at the caster's middle;
- 2 above the caster;
- 3 **ahead of the caster** by `radius + extent`, at middle height;
- 4 ahead by `radius + extent`, high (`2 × height + extent` up);
- 5 behind and high;
- 6 **at the chosen target**;
- 7 a **random** point 20..39 from the caster in one of 16 directions, high (uses `Math_Rand`);
- 8 a **diagonal** point: d = `body_radius × 10 × scale` = 35 for the player, at (1.5d to the right, d ahead), and so
  63 away;
- 9 380 × scale forward (no spell uses it).

### Hit test (`Magic_FindTargetInRange`)

A victim is hit when this holds:

```
sqrt(dx² + (dy/2)² + dz²) < extent + victim.body_radius × victim.scale
```

- dy is measured to the victim's middle (`y + height × scale / 2`), or to `battle->unk94` for kind-1 actors, or is 0
  for kind 2.
- The test is an ellipsoid, squashed vertically by 2.
- It hits the first live enemy, in slot order, that passes and that this effect has not hit before
  (`magic_hit_slot_bits`).
- Without the piercing flag, the effect then ends. With it, the effect carries on and can hit another enemy on a
  later frame.

`Magic_FindEligibleTarget` tests no volume. It returns the chosen target. With the piercing flag and a lifetime over
1, it then returns every other live enemy, one a frame. That is how the "all enemies" spells work.

### Choosing the target (`Magic_SelectClosestAimTarget`)

This has **no range limit and no line of sight**. It picks the live enemy whose bearing from the caster is closest
to an angle: the caster's `rot`, or, with flags bit 3, the effect's launch velocity. An enemy caster always targets
the player.

### Movement (`Magic_MoveHomingEffect`)

Each frame:

```
dir = (homing_strength × unit(to the target's middle)) + unit(v)
v   = 0.9 × v + homing_accel × unit(dir)
```

- The top speed is `10 × homing_accel`: 4 to 12 units a frame.
- Homing strength 0 means a straight line.
- After a piercing hit (`MagicEffect.flags & 2`), or when the target is dead, it flies on straight.

### Walls and floors

**No spell collides with walls.** `Coll_ResolveFloorAndWalls` is called with flags 0xA00, which has neither wall
bit, so spells pass through walls. Only y is ever taken from the result.

### Timing

`Player_StartCastSpell` puts the player in state 0xE. 18 frames later (`unk124 = 0x12`),
`Player_StateCastSpell` calls `Magic_CreateCastEffect(actor, spell_id, 0, flags)`. Every placement, launch velocity
and target choice reads `actor->rot` then. In state 0xE nothing changes `rot`, and repeats read it again.

### The 50/50/50/50 bonus

With all four elements at 50, `player_state.flags & 0x10000` is set and the cast gets flag 0x10:

- a multi-cast spell casts `2 × cast_count − 1` times, so the extra casts go on round the launch directions;
- a moving effect lives 1.5× as long.

## 2. Every player spell

Abbreviations in the Code column:

- `sel` target select, `place` placement, `move` move handler, `find` find handler;
- P piercing; V velocity aim (flags bit 3);
- "r" is the victim's radius (5.6 for a Were-Rabbit).

Ranges are worked out for flat ground and a target 6 high, using the game's own formulas.

### Fire

| # | Spell | Elements | Kind | How it moves and hits | Code | Aim UI |
| --- | --- | --- | --- | --- | --- | --- |
| 0.0 | Fire Ball Lv1 | Fire | **Facing cone, homing bolt** | One bolt starts 9.5 ahead at chest height, launched forward and up (v = (0, 1, 2)). It homes (strength 2, accel 1.0, top speed 10) on the enemy nearest its heading. It lives 9 frames and travels about 52, ignoring the floor. Hit: 6 + r, single hit. It reaches a target straight ahead up to about 65, 30° off up to 55, 60° off up to 30. **(verified)** Its last tested place is 55 from the caster, so about 66 for r 5.6; a bat 72.6 away was missed. | sel0 V, place3, move0, find0 | Cone of reach about 65 along the facing. The enemy the bolt will pick is ringed, greyed if out of reach. Click an enemy to face it. |
| 0.1 | Fire Ball Lv2 | Fire+Fire | Same, 2 bolts | Bolts 1 frame apart at 0° and 45° to the right **(verified)**. **Each picks the enemy nearest its own heading** (V). They live 10 frames, about 58 travel, reach about 70 ahead (**verified**: the first bolt's last place is 62.4 out, about 73). | as Lv1, cast_count 2 | Two arcs (0°, 45°), each with its target ringed. |
| 0.5 | Fire Ball Lv3 | Fire+Fire+Fire | Same, 3 bolts | Bolts at 0°, 45° right and 45° left, each with its own target. They live 11 frames, about 65 travel, reach about 80 ahead (**verified**: it hit a bat 72.6 away). | cast_count 3 | Three-armed fan, about 80 long, each arm's target ringed. |
| 0.2 | Power Rod Lv1 | Fire+Earth | Self | Attack ×1.5 (status 2, arg 1). | sel2, find1 | None. Click the plate. |
| 0.7 | Power Rod Lv2 | Fire+Earth+Earth | Self | Attack ×2. | sel2 | Click the plate. |
| 0.3 | Homing Arrow Lv1 | Fire+Wind | **Auto-target, homing, piercing** | 3 arrows, one every 4 frames. They start just behind and over the head and are launched backwards (directions 5, 6, 7), speed 6. Each picks the enemy nearest *its own backward heading* (V), so the arrows spread over the outermost enemies. They home hard (strength 10, accel 1.0) and live 25 frames. Hit: 2 + r, piercing; after a hit they fly on straight. They swing wide: an arrow that turns from 135° back reliably hits only up to about 30, and the straight-back one reaches about 110. | sel0 V, place5, move0, find0, P | Ring the enemy each arrow will pick, with a count badge. Draw the predicted curves (dry run, section 4). The click faces, but the arrows choose their own targets. |
| 0.9 | Homing Arrow Lv2 | Fire+Wind+Wind | Same | 5 arrows, one every 8 frames, from the feet, directions 4, 5, 6, 7, 0. Top speed 12, life 30. Reach about 30, 70, 70, then 160 or more. | place0, cast_count 5 | As Lv1. |
| 0.4 | Hot Steam Lv1 | Fire+Water | **Area round self** | Stays at the caster's feet on the floor for 8 frames. Hits every enemy within 15 + r, piercing. | sel1, place0, move1, find0, floor2, P | Circle r 15 round the player; enemies inside glow. No aim, so click the plate. |
| 0.14 | Hot Steam Lv2 | Fire+Water+Water | Area round self | Same, r 30. | as Lv1 | Circle r 30. |
| 0.6 | Compression | Fire+Fire+Wind | **Auto-target single** | Hits the enemy nearest the facing, any distance, 75%. Shrink. | sel0, place6, find1 | Click a monster. |
| 0.8 | Fire Pillar | Fire+Fire+Water | **Facing point (ground circle)** | A pillar on the floor 11.5 ahead (3.5 + 8), after a 3-frame delay, for 8 frames. Hits within 8 + r, piercing. | sel1, place3, move1, find0, floor2, P | Circle r 8 at 11.5 ahead, swung round the player by the mouse. The distance is fixed. |
| 0.10 | Fire Bomb | Fire+Fire+Earth | **Facing lob, ground circle** | Lobbed from 21.5 ahead, forward and up, with gravity, not homing. It lands **about 49 ahead after 18 frames (verified; the research said 52 after 11)**, then blasts within 18 + r for 8 frames, piercing. It hits nothing in flight (find2). | sel0, place3, move0 (strength 0), find2, floor1, gravity, P | Arc and landing circle r 18 at the predicted landing point (a dry run against the real floor). The mouse sets the direction only. |
| 0.11 | Vampire Touch | Fire+Earth+Water | Self | Status 1 (bit 2: heal on hit). | sel2 | Click the plate. |
| 0.12 | Magma Ball | Fire+Earth+Wind | **Facing fan of ground lines** | 3 balls one frame apart from the feet at 0°, +45° and −45°. They roll along the floor (floor2), not homing, at up to 7 a frame for 20 frames, about 91 each. Hit 7 + r, piercing. Damage also clears the victim's statuses (hit 9). | sel1, place0, move0 (strength 0), find0, floor2, P | Three tubes, 91 long, half-width 7 (+r), in a fan. Enemies touched glow. |
| 0.13 | Extinction | Fire+Water+Wind | Auto-target single | The enemy nearest the facing, 60%, after a 28-frame delay. Removes it (bosses: popup only). | sel0, place6, find1, hit 7 | Click a monster. |

### Earth

| # | Spell | Elements | Kind | How it moves and hits | Code | Aim UI |
| --- | --- | --- | --- | --- | --- | --- |
| 1.0 | Rock Lv1 | Earth | **Homing lob at the target nearest the facing** | Appears 8.5 ahead and 27 high and waits 10 frames. Then it falls forward with gravity, homing weakly (strength 1, accel 0.5) on the enemy nearest the facing, and lands (floor1, camera shake). It hits in flight and for 8 frames after landing, within 5 + r, piercing. Straight ahead it hits at 15..70; **30° off the facing it misses**. With no pull it lands about 72 ahead. **(verified)** It appears 8.5 ahead and 27.4 up, as said; but it always has a target to pull toward (the one nearest the facing), and the pull toward the target's middle brings it down early: aimed at a bat 72.6 away it landed 60.1 ahead and missed. Lv2 and Lv3 landed 67.7 and 70.7 ahead at the same bat and hit. | sel0, place4, move0, find0, floor1, gravity, P | Ring the target, and put a circle r 5 at the predicted landing point (dry run). Click a monster to face it. This is where aiming helps most. |
| 1.1 | Rock Lv2 | Earth+Earth | Same | Delay 16, starts 13.5 ahead and 32 high, hit 10 + r. Straight ahead it hits at 10..85; 30° off at 15..70. | extent 10 | As Lv1, circle r 10. |
| 1.5 | Rock Lv3 | Earth+Earth+Earth | Same | Delay 20, starts 18.5 ahead and 37 high, hit 15 + r. Straight ahead it hits at 10..100; 30° off at 10..80. | extent 15 | Circle r 15. |
| 1.2 | Spirit Armor Lv1 | Earth+Water | Self | Defence ×1.5. | sel2 | Click the plate. |
| 1.7 | Spirit Armor Lv2 | Earth+Water+Water | Self | Defence ×2. | sel2 | Click the plate. |
| 1.3 | Rolling Rock Lv1 | Earth+Fire | **Sweep line across the front** | A rock starts at the diagonal point (**52.5 to the right**, 35 ahead: verified) and rolls *sideways*, to the left, across the front along the floor. It does not home, reaches up to 7 a frame, and goes **about 275 in 50 frames (verified; the research said 220)**, so it crosses the arena. Hit 6 + r, piercing. | sel0, place8, move0 (strength 0, direction 4), find0, floor2, P | A tube perpendicular to the facing, 35 ahead, from 52.5 on the right to the arena's edge, half-width 6. Enemies on it glow. |
| 1.13 | Rolling Rock Lv2 | Earth+Fire+Fire | Same | Hit 12 + r. | extent 12 | Tube, half-width 12. |
| 1.4 | Weakness Lv1 | Earth+Wind | Auto-target single | The enemy nearest the facing, 90%. Defence −25%. | sel0, place6, find1 | Click a monster. |
| 1.10 | Weakness Lv2 | Earth+Water+Wind | Auto-target single | The same, defence −50%. P is set, but life 1, so only one enemy is hit. | sel0, find1 | Click a monster. |
| 1.6 | Magnet Rock | Earth+Earth+Water | **Auto-target (sits on the target)** | Placed at the target's feet (the enemy nearest the facing), it follows the target's x/z for 40 frames after an 18-frame delay. Hit 1 + r, piercing, so an enemy overlapping the target is hit too. | sel0, place6, move2, find0, P | Click a monster. |
| 1.8 | Rock Combo | Earth+Fire+Earth | **Random ring round self** | 10 rocks, one every 5 frames, each dropped from 32 high at a random point 20..39 from the player (`Math_Rand`, 16 directions). Each hits within 10 + r while falling and for 8 frames after landing. | sel1, place7, move1, find0, floor1, gravity, P | Ring 10..49 round the player (the outer edge plus r). It can't be aimed; click the plate. Never preview the random points: that would draw from the game's RNG. |
| 1.9 | Confusion Magic | Earth+Fire+Water | Self | Status 9: damage taken gives MP. | sel2 | Click the plate. |
| 1.11 | Rock Shower | Earth+Earth+Wind | **Whole arena** | Centred on the player with hit radius 1000. Hits every enemy, one a frame, over 20 frames. | sel1, place0, move1, find0, P | Full screen: all enemies glow. Click the plate. |
| 1.12 | Magic Barrier | Earth+Fire+Wind | Self | Status 8: immune to spell hits. | sel2 | Click the plate. |
| 1.14 | Wide Weakness | Earth+Wind+Wind | Whole arena | Every enemy, 90%, defence −25%. | sel1, find1, life 8, P | Full screen. |

### Water

| # | Spell | Elements | Kind | How it moves and hits | Code | Aim UI |
| --- | --- | --- | --- | --- | --- | --- |
| 2.0 | Water Pillar Lv1 | Water | **Facing point (ground circle)** | A pillar on the floor 8.5 ahead (3.5 + 5) for 30 frames. Hits within 5 + r, piercing. | sel1, place3, move1, find0, floor2, P | Circle r 5 at 8.5 ahead, swung by the mouse. |
| 2.1 | Water Pillar Lv2 | Water+Water | Same | 13.5 ahead, r 10. | extent 10 | Circle r 10. |
| 2.4 | Water Pillar Lv3 | Water+Water+Water | Same | 18.5 ahead, r 15. It is at chest height, not on the floor (floor mode 0); this barely matters given the halved dy. | extent 15, floor0 | Circle r 15. |
| 2.2 | Healing Lv1 | Water+Earth | Self (battle and field) | Heals 2% of max HP + level/2. | sel2, find1 | Click the plate. |
| 2.10 | Healing Lv2 | Water+Earth+Earth | Self (battle and field) | 16% + level/2. | sel2 | Click the plate. |
| 2.3 | Soul Search Lv1 | Water+Fire | Auto-target single | The enemy nearest the facing: shows its stats. | sel0, place6, find1 | Click a monster. |
| 2.11 | Soul Search Lv2 | Water+Fire+Fire | Whole arena | Every enemy. | sel1, find1, life 8, P | Full screen. |
| 2.5 | Ice Gas | Water+Earth+Water | **Area round self** | At the feet for 8 frames. It sinks (gravity, no floor), which barely matters. Hits within 15 + r, piercing, and freezes (status 3). | sel1, place0, move1, find0, gravity, P | Circle r 15 round the player. Click the plate. |
| 2.6 | Ice Knife | Water+Water+Wind | Auto-target single | The enemy nearest the facing, after a 20-frame delay. Freezes. | sel0, place6, find1 | Click a monster. |
| 2.7 | Exit | Water+Wind | Self, field only | Leaves the dungeon (hit 6, `Magic_TryStartFlag8ReturnTransition`). | sel2 | Click the plate. |
| 2.8 | Escape | Water+Earth+Wind | Self, battle | Ends a random battle, 55% (hit 3). | sel2 | Click the plate. |
| 2.9 | Return | Water+Wind+Wind | Self, field only | Warps back (hit 4). | sel2 | Click the plate. |
| 2.12 | Walking Water | Water+Fire+Water | **Facing line along the ground** | A wave starts 13.5 ahead after a 6-frame delay and runs straight forward along the floor. It does not home, has a top speed of 5, and goes **about 219 in 50 frames (verified; the research said 233)**, so it crosses the arena. Hit 10 + r, piercing. | sel1, place3, move0 (strength 0), find0, floor2, P | A tube from 13.5 ahead to the arena's edge, half-width 10. Enemies on it glow. |
| 2.13 | Drain Magic | Water+Fire+Earth | Auto-target single | The enemy nearest the facing, 70%. Takes 11/64 of its MP. | sel0, place6, find1 | Click a monster. |
| 2.14 | Invalidity | Water+Fire+Wind | Self | Clears the caster's statuses (hit 8). | sel2 | Click the plate. |

### Wind

| # | Spell | Elements | Kind | How it moves and hits | Code | Aim UI |
| --- | --- | --- | --- | --- | --- | --- |
| 3.0 | Wind Cutter Lv1 | Wind | **Auto-target, homing** | One cutter from the caster's middle, launched 45° to the side (direction 2), speed 6. It homes hard (strength 10, accel 1.2, top speed 12) on the enemy nearest the **facing** (not V), lives 30 frames, hits 5 + r. Reach about 130. | sel0, place1, move0, find0 | Click a monster (ring it). |
| 3.1 | Wind Cutter Lv2 | Wind+Wind | Same | 3 cutters, one every 4 frames, directions 2, 3, 4. All home on the same target (the facing doesn't change); if it dies, the next cutter retargets. Reach about 90..130. | cast_count 3 | Click a monster. |
| 3.5 | Wind Cutter Lv3 | Wind+Wind+Wind | Same | 6 cutters, directions 2..7, all at one target. The back-launched ones reach 160 or more. | cast_count 6 | Click a monster. |
| 3.6 | Large Cutter | Wind+Fire+Wind | **Facing line (short tube)** | A flat slash (vy = 0) from the caster's middle, straight forward. It does not home, starts at speed 7, and lives **6 frames**, so it reaches about 51. Hit 12 + r, piercing. | sel1, place1, move0 (strength 0, speed 7), find0, P | A tube 51 long, half-width 12, along the facing. |
| 3.2 | Restriction Lv1 | Wind+Fire | Auto-target single | The enemy nearest the facing, 80%, after a 45-frame delay. Status 3 (no action). | sel0, place6, find1 | Click a monster. |
| 3.7 | Restriction Lv2 | Wind+Fire+Fire | Whole arena | Every enemy. | sel1, find1, life 8, P | Full screen. |
| 3.3 | Avoidance Lv1 | Wind+Water | Self | Agility ×2. | sel2 | Click the plate. |
| 3.9 | Avoidance Lv2 | Wind+Water+Water | Self | Agility ×4. | sel2 | Click the plate. |
| 3.4 | Silence Lv1 | Wind+Earth | Auto-target single | The enemy nearest the facing, 80%. Status 6 (no spells). | sel0, place6, find1 | Click a monster. |
| 3.13 | Silence Lv2 | Wind+Earth+Earth | Whole arena | Every enemy. | sel1, find1, life 8, P | Full screen. |
| 3.8 | Wind Bomb | Wind+Water+Wind | **Area round self** | At the feet, on the floor, for 8 frames. Hits within 15 + r, piercing, and halves agility. | sel1, place0, move1, find0, floor2, P | Circle r 15 round the player. Click the plate. |
| 3.10 | Cyclone | Wind+Earth+Wind | **Facing line along the ground** | From the feet, straight forward along the floor, not homing, with gravity (which only tilts its heading). It goes about 106 in 45 frames. Hit 12 + r, piercing. | sel1, place0, move0 (strength 0), find0, floor2, P | A tube 106 long, half-width 12. |
| 3.11 | Slow Enemy | Wind+Fire+Earth | Auto-target single | The enemy nearest the facing, 90%. Its move ring ×0.5. | sel0, place6, find1 | Click a monster. |
| 3.12 | Wind Walk | Wind+Earth+Water | Self | The player's move ring ×2. | sel2 | Click the plate. |
| 3.14 | Ultimate Wind | Wind+Fire+Water | Whole arena | Placed on the first live enemy, after a 20-frame delay. Hits every enemy, one a frame, over 8 frames. | sel1, place6, find1, P | Full screen. |

### Summary by kind

| Kind | Spells | What the mouse changes | Visual |
| --- | --- | --- | --- |
| Self | Power Rod 1/2, Vampire Touch, Spirit Armor 1/2, Confusion Magic, Magic Barrier, Healing 1/2, Exit, Escape, Return, Invalidity, Avoidance 1/2, Wind Walk | nothing | Click the plate (the existing `PORT_TAG_SPELL_CAST`). Optionally a glow on the player. |
| Whole arena | Rock Shower, Wide Weakness, Soul Search Lv2, Restriction Lv2, Silence Lv2, Ultimate Wind | nothing | Click the plate; every enemy glows. |
| Area round self | Hot Steam 1/2 (r 15/30), Ice Gas (15), Wind Bomb (15) | nothing | A circle round the player; enemies inside glow; click the plate. |
| Random round self | Rock Combo (ring 20..39, rocks r 10) | nothing | A faint ring 10..49; click the plate. |
| Auto-target single (nearest the facing, any range) | Compression, Extinction, Weakness 1/2, Magnet Rock, Soul Search Lv1, Ice Knife, Drain Magic, Restriction Lv1, Silence Lv1, Slow Enemy | which enemy | Hovered enemy ringed; click a monster. |
| Auto-target homing projectile | Wind Cutter 1/2/3 (reach about 90..160), Homing Arrow 1/2 (V: the arrows spread over targets) | which enemy | Ring the target(s). For arrows, a badge per target and the predicted curves. |
| Facing cone (short homing fan) | Fire Ball 1/2/3 (reach about 65/70/80, 1/2/3 bolts at 0°, ±45°) | direction | A fan of arcs; each arm's chosen enemy ringed if in reach. |
| Facing point (fixed distance) | Fire Pillar (r 8 at 11.5), Water Pillar 1/2/3 (r 5/10/15 at 8.5/13.5/18.5) | direction | A ground circle at its fixed distance, swung round the player. |
| Facing lob | Fire Bomb (r 18, lands about 52 ahead); Rock 1/2/3 (homing lob; r 5/10/15 near the target) | direction, which also picks the target | The arc and the landing circle from a dry run. |
| Facing lines (tubes) | Walking Water (233, ±10), Cyclone (106, ±12), Large Cutter (51, ±12), Magma Ball (3 × 91, ±7, fan 0/±45°), Rolling Rock 1/2 (across the front at 35 ahead, ±6/±12) | direction | Tubes on the ground; enemies they touch glow. |

## 3. What aiming has to change, and the hook

### What the player can already do: turning is free in battle

On the player's turn the stick turns the player:

- `Player_StateMove` calls `Player_MoveWalk`, which sets `rot += d × 0.5` a frame, with d clamped to ±45°.
- Speed is scaled by `1 − |d| / 45°`, so **a turn of 45° or more is made on the spot without moving**. Only the last
  45° creeps forward a little.
- The move ring (`Player_MoveAndCollide`, radius `unkC × unk90` from `battle_turn_origin`) clamps *where* the player
  is, never their `rot`.

The player cannot turn in these cases:

- while a spell is being put together (`core_flags & 4` is in `core_flags & 0x300F`, which zeroes the stick);
- during `battle_turn_delay`;
- on the enemies' turn (`battle_flags & 2`);
- while frozen (`status_bits & 8` also sets `animFlags & 1`, so the player cannot act at all).

So in the original you turn first and then build the spell. With the cast plate you build it first and then aim.
The set of outcomes is the same.

### The hook: set the facing at the moment of casting

Every targeting rule above reads one thing from the player: `actor->rot`, at `Magic_CreateCastEffect`, 18 frames
after the cast starts. Nothing changes `rot` in between. The minimal hook is in `Player_StartCastSpell`
(`patches/p0.patch`, `game/player/player.c`):

```c
e = &D_800C185C[...][...];
#ifdef PORT
    if (e->target_select_handler_index != 2) Port_SpellAimApply(a); /* sets a->rot if a mouse aim was taken */
#endif
```

`Port_SpellAimApply` does the following:

- If this read committed an aim, it takes the aim's point: a clicked enemy slot's x/z, or a ground point.
- It sets `a->rot = Math_Atan2(px − a->x, pz − a->z)`, using the game's own atan2, on the game's thread.
- It clears the aim.
- With no aim it returns at once, so `rot` and every hash are unchanged.

The cast animation (`Anim_SetCurrentAndNext` just below) then plays facing the aim, as if the player had turned.

Why `rot` and not a target index:

- It is exactly "what the player could do by turning", so it gives no extra range or behaviour.
- It is right for every kind at once:
  - auto-target spells pick the clicked monster, because it is at bearing 0, the minimum;
  - the cones, points, lobs and tubes go where the overlay showed;
  - the velocity-aimed fans (Fire Ball Lv2/3, Homing Arrows) keep their real spread over targets.

  Overriding `Magic_SelectClosestAimTarget` instead would let Homing Arrows or Fire Ball's side bolts all go to one
  enemy. The original can't do that.
- One edge case: two enemies exactly in line. `d < best` keeps the lower slot. The stick has the same tie. The
  overlay should ring the enemy the rule will actually pick, using the same test.

For ground spells, the mouse chooses the **direction**, not the distance. Fire Pillar, Water Pillar, Fire Bomb and
the lines have a fixed reach from the code. The circle or tube slides round the player on its fixed radius,
following the pointer's bearing. Letting the click choose a distance would be a new mechanic (see the questions).

Items that cast spells (`Item_UseSpell` calls `Magic_CreateCastEffect(..., 0, 4)`) read the same `rot`. They could
take the same hook later, in `Player_StartUseItem`.

### Input path: the per-read controller path

As with `port/p0/mouse_menu.c` and `mouse_camera.c` (docs/CONTROLS.md):

1. **Frame build.** The view the picture was drawn with is kept until the next read, as the menu targets are
   (`Port_MouseFrameBegin`). That view includes the mouse camera's turned eye, *before* `Port_MouseCameraRestore`
   puts the game's back. Store the drawn view-projection, and the picture width for 16:9, at the end of
   `Gfx_BuildFrameDisplayList`.
2. **Each controller read** (`port/os/controller.c`, after `Port_MouseMenuRead`): `Port_SpellAimRead`.
   - It is active only when all of these hold:
     - `battle_flags & 1`;
     - `core_flags & 4`;
     - `player_state.spell_input_count > 0`;
     - the built spell passes `Magic_GetSpellRestrictionFlags`;
     - its `target_select_handler_index != 2`;
     - it is not self or whole-arena;
     - the pointer is `PORT_POINTER_IN`;
     - the pointer is **not** on a HUD target (the crystals, the spell list and the cast plate win).
   - **Hover:** project each live enemy's middle to picture units and pick the nearest within its projected radius
     plus a few pixels. If none is picked, cast the pointer's ray onto the plane y = player.y to get a ground point.
     Keep it as the preview aim (port state only).
   - **Left click:** commit that aim and `Port_MouseInject(PORT_MOUSE_A)`. The game's own A branch in
     `Player_HandleSpellInput` then casts it, with the restriction check and the MP, and `Player_StartCastSpell`
     applies the aim.
   - A right click is still B (take the last element back).
3. **Determinism.** Everything runs on the game's thread from the read's pointer, in picture units, and from game
   state. An input script's `mouse VI 0 0 0 2 X Y` rows replay an aim exactly. With no pointer, nothing is read or
   written.

### Fairness check

- The facing is the only thing set. Distance, ring position, MP and the hit rolls are untouched.
- The aim is applied only when the player could have turned: it is their turn, they can act, and they aren't frozen.
  `Player_CanAct` already gates the cast.
- One small difference: turning by mouse adds no move distance, so `Player_AddTurnMoveAgilityXP` gives no agility
  XP for the turn. It is negligible, and in the player's disfavour.

## 4. Drawing the preview

These rules keep the game's hashes unchanged:

- **Draw in the port, not with game effects.** `Effect_CreateTerrainRing` (the battle rings) or
  `Hud_DrawMeleeTargetMarker` would take game effect slots or display list words. Draw the circles, tubes and rings
  in the native renderer as an overlay, after the game's frame, like the minimap. Its look can copy the game's
  terrain ring: a strip sampled along the floor.
- **Floor heights** come from the collision data, as the automap does (docs/AUTOMAP.md).
  `Coll_ResolveFloorAndWalls` writes the global `coll_push_radius` (from `out->x`), so save and restore it. Better,
  query the floor the way the automap does. Never call `Math_Rand`.
- **Dry run for moving spells** (Fire Ball, Homing Arrow, Fire Bomb, Rock, Wind Cutter): integrate a copy of the
  `Magic_MoveHomingEffect` formula on a local struct for the spell's lifetime, using the same tables
  (`magic_launch_speeds`, `magic_homing_*`, `D_800C1878`) and the floor query. Run the hit test each frame against
  the live enemies. Two shortcuts would cause side effects:
  - calling `Magic_MoveHomingEffect` itself: on landing it calls `Camera_Shake` and
    `Magic_CreateGroundHitPresentation`;
  - calling `Magic_CreateCastEffect`.

  This gives the path, the landing point, and who would be touched, for the real geometry.
- **Who gets hit:** the same ellipsoid test, `extent + r`, with dy halved. Draw the ring's radius as `extent` and
  light up each enemy whose own circle overlaps it.
- **Facing preview:** while aiming, show the yaw by turning a ghost arrow at the player's feet, not the actor. The
  actor turns only at the cast.
- **Field:** no field spell aims. Healing Lv1/Lv2, Exit and Return are self spells. Every other spell has flags bit 1
  clear and cannot be cast on the field. Aim mode should stay off outside battle.

## 5. The enemies' spells (element 4)

The enemies cast these through `Enemy_StateCastSpell` → `Magic_CreateCastEffect`. For an enemy caster, both target
rules pick the player. They are not player spells and need no aim. Their reach (`effect_extent`) is also used by the
enemy AI (`enemy.c`, `enemy_temp_vec.z`).

## How the numbers were found

- **Tables.** The `SpellDef`, `SpellFxDef` and launch-direction tables were read from the ROM's magic block (ROM
  0xD490C0 → 0x800C0000; `D_800C185C`, `D_800C1878`), and the speed tables from `.data` (`magic.c`). The names come
  from `translations/en.v2.tsv` (0x2000000 + element × 16 + index).
- **Ranges.** A Python copy of `Magic_MoveHomingEffect` and `Magic_FindTargetInRange` gave the ranges, on flat
  ground, against a target with radius 5.6 at height 6 (a Were-Rabbit). The C copy the aim uses (port/p0/spell_aim.c)
  was then checked in play: `python tools/spell_aim_test.py --verify` casts every aiming spell and compares the game's
  effects (`P1_AIM_FX`) with the copy's paths every tick, and what they hit. Fire Ball's reach, Rock's landing point,
  Rolling Rock's side and distance, Walking Water's and Fire Bomb's numbers were corrected from it (SPELL_AIM.md,
  "Verified in play").
- **Not in the repo.** The scratch tools (`dump.py`, `sim.py`, `reach.py`) were not committed.
