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

enum { EL_AM_VERSION = 2 };

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
};
enum {
    EL_AM_MARK_DONE = 1,      /* opened, taken (the game's chest_opened_flags / spirit_collected_flags) */
    EL_AM_MARK_LOCKED = 2,    /* an exit that needs an item the player has not got (MapExit flags 0x10) */
    EL_AM_MARK_SHUT = 4,      /* an exit shut because the player holds its item (MapExit flags 0x20) */
    EL_AM_MARK_AWAY = 8,      /* an exit to another area: another map, with a field or a dungeon (an enemy area) on
                               * one side or both (a town gate, a road, a cave mouth), not a house or room of the
                               * same town or castle */
};

typedef struct ElAmMarker {
    uint32_t id;
    uint16_t kind, flags;     /* EL_AM_MARK_* */
    float x, y, z, angle;     /* angle: the way through a door or exit (it faces (sin a, cos a) in (x, z)) */
    float half_w, half_d;     /* an exit's box (half width across, half depth along the way through) */
    float radius;             /* a boss's: how near starts the fight */
    uint16_t item;            /* a chest's item, a locked exit's key item (the game's item ids) */
    int16_t to_map, to_sub;   /* an exit's destination */
    uint16_t pad;
    char label[96];           /* what it is, in English (port text: text/en.tsv), for hover */
} ElAmMarker;

enum {
    EL_AM_AREA_SKY = 1,     /* the sub-area has a sky (an outdoor field: map_env_flags & 4) */
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
} ElAmArea;

enum {
    EL_AM_PLAYER_PLAY = 1,   /* in play (gameplay mode, no area transition): the map can open */
    EL_AM_PLAYER_BATTLE = 2, /* a battle is on */
    EL_AM_PLAYER_TRUTH = 4,  /* (a test) this tick is drawn from straight above (ELTALE_AUTOMAP_TRUTH) */
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
} ElAmPlayer;

#ifdef __cplusplus
}
#endif

#endif
