/* The area map's feed (docs/AUTOMAP.md): what the game side (game/native_draw/nd_automap.c) reads from the game's
 * own state and hands to the host (port/host/automap.cpp), which draws the full map and the minimap. Read-only:
 * the feed never writes game state.
 *
 * World units are the game's: x and z on the ground (+z is north: the camera's yaw 0, the compass's N), y up.
 * Facing: the player's actor.rot in radians; it faces (sin rot, cos rot) in (x, z).
 *
 * Plain C, shared by the exe and eltale_host (no pointers kept by the host past the call: it copies). */
#ifndef PORT_AUTOMAP_H
#define PORT_AUTOMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { EL_AM_VERSION = 3 };

/* A floor triangle the player walks on: one of the sub-area's collision triangles (CollTri) with an actor flag
 * (0x200 or 0x800: the player's query is 0xA01), in world space. */
typedef struct ElAmTri {
    float v[3][3];  /* x, y, z per corner */
    float ny;       /* the normal's y (1: flat; small: a steep slope) */
    uint16_t flags; /* CollTri.flags: 0x800/0x200 actors, 0x400/0x200 the camera; bits 1-3 the camera preset;
                     * bits 6-8 the floor's effect (1: hurts, 2: slides) */
    uint16_t obj;   /* the map object it belongs to (map_objs index): the id for hover and marks later */
} ElAmTri;

/* A wall segment the player is pushed out of (map_actor_walls of the sub-area). */
typedef struct ElAmWall {
    float x0, z0, x1, z1;
    uint16_t run;   /* the polyline (CollWallRange) it belongs to */
    uint16_t pad;
} ElAmWall;

/* A way out of the sub-area (MapExit): the box the player walks into, and where it leads. */
typedef struct ElAmExit {
    float x, z, angle, half_w, half_d; /* angle in radians, as Exit_CheckEnter rotates by it */
    uint16_t flags;                    /* MapExit.flags (2: any direction; 0x10/0x20: an item needed / not held) */
    uint16_t item;                     /* MapExit.id: the item flags 0x10/0x20 test */
    int16_t to_map, to_sub, to_entry;
    uint16_t pad;
} ElAmExit;

/* The marks (v2): what the map draws over the floor, sent with the player every tick (people move, a chest opens).
 * Each has a stable id (kind << 24 | the game's own index) for hover and for following a person between ticks. */
enum {
    EL_AM_MARK_DOOR = 1,   /* an exit through a door the game swings or slides open (MapExit kind 2-4, its door
                            * object); x, z the middle of the doorway, angle the way through, half_w its half width */
    EL_AM_MARK_EXIT = 2,   /* a way out with no door (an opening, a road's end, stairs): the exit's box */
    EL_AM_MARK_PERSON = 3, /* someone to talk to (npc_slots) */
    EL_AM_MARK_CHEST = 4,  /* a chest (chest_objects): item = what it holds (ChestDef.animSetIndex) */
    EL_AM_MARK_SPIRIT = 5, /* a spirit (spirit_objects): an element point to raise */
    EL_AM_MARK_BOSS = 6,   /* a boss standing on the field (boss_field_record); radius = how near starts the fight */
    EL_AM_MARK_SIGN = 7,   /* something to read (textspot_list: a signpost, a door shut fast, a spirit stone); radius =
                            * how near the player must stand */
};
/* What a person offers (v3): ElAmMarker.service of a person, and as bits (EL_AM_SVC_BIT) in a door's `services`
 * (what the building or room behind it holds, a couple of doors deep). A small enum: shops could join it. */
enum {
    EL_AM_SVC_NONE = 0,
    EL_AM_SVC_INN = 1,   /* an innkeeper (NpcDef.talk_kind 6: the rest choice; resting is where the game saves) */
    EL_AM_SVC_WINGS = 2, /* gives Traveler's Wings when you have none (talk kind 2, an item whose use is a warp) */
    EL_AM_SVC_FOOD = 3,  /* gives bread when you have none (talk kind 2, an item whose use restores HP) */
    EL_AM_SVC_MP = 4,    /* gives an MP remedy when you have none (talk kind 2, an item whose use restores MP only) */
    EL_AM_SVC_COUNT
};
#define EL_AM_SVC_BIT(s) (1u << (s))
enum {
    EL_AM_MARK_DONE = 1,      /* opened, taken (the game's chest_opened_flags / spirit_collected_flags) */
    EL_AM_MARK_LOCKED = 2,    /* an exit that needs an item the player has not got (MapExit flags 0x10) */
    EL_AM_MARK_SHUT = 4,      /* an exit shut because the player holds its item (MapExit flags 0x20) */
    EL_AM_MARK_AWAY = 8,      /* an exit to another area: another map, with a field or a dungeon (an enemy area) on
                               * one side or both (a town gate, a road, a cave mouth), not a house or room of the
                               * same town or castle */
    EL_AM_MARK_KEY_GIVER = 0x10, /* a person who gives a story item (talk kind 2, an item with no use): a cheat */
    EL_AM_MARK_STORY = 0x20,     /* a person with a story or boss line (talk kind 5) */
};

typedef struct ElAmMarker {
    uint32_t id;
    uint16_t kind, flags;     /* EL_AM_MARK_* */
    float x, y, z, angle;     /* angle: the way through a door or exit (it faces (sin a, cos a) in (x, z)) */
    float half_w, half_d;     /* an exit's box (half width across, half depth along the way through) */
    float radius;             /* a boss's: how near starts the fight */
    uint16_t item;            /* a chest's item, a locked exit's key item (the game's item ids) */
    int16_t to_map, to_sub;   /* an exit's destination */
    uint8_t service;          /* a person's EL_AM_SVC_* */
    uint8_t services;         /* a door's or exit's: EL_AM_SVC_BIT()s of what lies behind it */
    char label[96];           /* what it is, in English (port text: text/en.tsv), for hover */
    char detail[96];          /* more for hover (a service, the item a person gives: shown when its mark is) */
} ElAmMarker;
/* A mark's stable key, for fog of war's "discovered" bits kept per save: (map, sub-area, id). The id is the kind and
 * the game's own index (an exit's in MapSubareaDef.exits, a person's NpcDef order in the sub-area, a chest's or
 * spirit's slot, a sign's textspot_table index), the same every visit. */

/* A random battle region (v3: EncounterRegion of battle_encounter_region_table): a battle can start while the
 * player is inside the rectangle (the first one listing them wins) and an encounter point lies 50..90 units away. */
typedef struct ElAmZone {
    float x0, z0, x1, z1;     /* the rectangle: x0 <= x <= x1, z0 <= z <= z1 */
    uint16_t group_count;     /* the region's group list (EncounterRegion.group_list) */
    uint16_t groups[7];
    char label[160];          /* its monsters in English, by group ("Slime x2-3 / Bat"), or the group ids */
} ElAmZone;

enum {
    EL_AM_AREA_SKY = 1,     /* the sub-area has a sky (an outdoor field: map_env_flags & 4) */
    EL_AM_AREA_ENEMY = 2,   /* random battles can start here (map_subarea_flags bit 0 and the area's tables) */
};

/* One sub-area's plan, sent when the player enters it (and again if its data moves). */
typedef struct ElAmArea {
    uint32_t version;       /* EL_AM_VERSION */
    uint32_t serial;        /* counts the plans sent */
    int32_t map, sub;       /* map_id, map_subarea */
    uint32_t flags;         /* EL_AM_AREA_* */
    float bounds[4];        /* min x, min z, max x, max z over the floor and the walls */
    float y_min, y_max;     /* the floor's height range */
    const ElAmTri *tris;
    uint32_t tri_count;
    const ElAmWall *walls;
    uint32_t wall_count;
    const ElAmExit *exits;
    uint32_t exit_count;
    /* water: the triangles of the sub-area's wave, translucent and scrolling-texture objects (what the renderer
     * draws for them), in world space; flags = the object's kind (1-3 waves, 0x7F/0x80 translucent, 0x40-0x44 a
     * scrolling texture), obj its index */
    const ElAmTri *water;
    uint32_t water_count;
    /* (v3) random battles: the regions and the encounter points (battle_encounter_group_table->points) */
    const ElAmZone *zones;
    uint32_t zone_count;
    const float *points;    /* x, z pairs */
    uint32_t point_count;
} ElAmArea;

enum {
    EL_AM_PLAYER_PLAY = 1,   /* in play (gameplay mode, no area transition): the map can open */
    EL_AM_PLAYER_BATTLE = 2, /* a battle is on */
    EL_AM_PLAYER_TRUTH = 4,  /* (a test) this tick is drawn from straight above (ELTALE_AUTOMAP_TRUTH) */
    EL_AM_PLAYER_RANDOM_BATTLE = 8, /* the battle on is a random one (battle_flags 0x100 clear: not a boss) */
    /* (a test, ELTALE_AUTOMAP_SHOTS) the host writes a picture: the truth view alone, the map's lines over it, the
     * full map as the player sees it */
    EL_AM_PLAYER_SHOT_TRUTH = 0x100,
    EL_AM_PLAYER_SHOT_OVER = 0x200,
    EL_AM_PLAYER_SHOT_MAP = 0x400,
};

/* The player, every tick: `frame` is the tick's graphics task number (ElRenderApi frame_end's), so the host draws
 * the arrow where the pictures between ticks put the player. */
typedef struct ElAmPlayer {
    uint32_t frame;
    uint32_t area_serial;   /* the plan it is on */
    uint32_t flags;         /* EL_AM_PLAYER_* */
    float x, y, z, rot;
    const ElAmMarker *markers; /* this tick's marks (v2) */
    uint32_t marker_count;
    /* (v3) the random battle roll, read-only (Battle_TryStartRandomEncounter): every 50 units walked
     * (battle_travel_accumulator) the limit grows by 50 (battle_encounter_roll_limit) and a battle starts when
     * Math_Rand(2000) < limit, the player is in a region and a point is 50..90 units away (then the limit is 0) */
    float travel;           /* battle_travel_accumulator: 0..50 */
    uint16_t roll_limit;    /* battle_encounter_roll_limit */
    uint16_t danger;        /* EL_AM_DANGER_* */
    int16_t zone;           /* the region the player is in by the game's rule (-1 none, or no point in reach) */
    uint16_t pad2;
} ElAmPlayer;
/* (fog of war) the game saved, loaded or went back to the title: ElHost_AutomapSave(kind, key), key a hash of the
 * saved record (nd_automap.c), so the host keeps what was explored with each save, in files of its own */
enum {
    EL_AM_SAVE_WRITE = 0, /* SaveMenu_CaptureGameState: the save menu's Save, an F2 quick save */
    EL_AM_SAVE_READ = 1,  /* SaveMenu_RestoreGameState: the save menu's Load (the title's Continue), an F4 quick load */
    EL_AM_SAVE_TITLE = 2, /* the intro or the title came: a reset, the game's end, before New Game */
};

enum {
    EL_AM_DANGER_ROLLS = 1,  /* the roll runs here: an enemy area, no battle, battle_flags 0x8000 clear, the player
                              * free to move (actor.animFlags bit 0 clear) */
    EL_AM_DANGER_ZONE = 2,   /* and a battle could start where the player stands (a region and a point 50..90 away) */
};

#ifdef __cplusplus
}
#endif

#endif
