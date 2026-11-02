/* Click to aim spells (port/include/port/spell_aim.h, docs/SPELL_AIM.md, docs/SPELL_TARGETING.md).
 *
 * Three parts, all on the game's thread:
 * - Port_SpellAimRead, at each controller read (after the menus' pointer): with a spell ready in a battle, the pointer
 *   over the battlefield picks a monster (each one's body projected with the view the last picture was drawn with) or
 *   else the ground point under it (the pointer's ray on the plane of the player's feet). That gives a facing; the
 *   preview is the spell cast with that facing, worked out by a copy of the game's own code (below). A left click
 *   keeps the point and presses A for this read: the game casts the spell as the controller's A would.
 * - Port_SpellAimApply, in Player_StartCastSpell: the facing toward the kept point (Math_Atan2, the game's own), set
 *   on the caster before the cast animation; nothing else. Every rule of the spell (Magic_CreateCastEffect 18 ticks
 *   later: placement, launch, target choice, repeats) then reads that facing, as if the player had turned.
 * - Port_SpellAimDraw, in Hud_Draw: the preview as world-space overlays (the native renderer's overlay triangles,
 *   render.h v10): areas on the ground; for every moving spell the tunnel where its hit test can touch a monster, a
 *   thin arc where it flies out of reach, and a homing bolt's cone; a ring under each monster that would be hit.
 *
 * The copy of the spell code (simulate): Magic_CreateCastEffect, the place, select, move and find handlers and the
 * repeats of Magic_UpdateEffects, with the same expressions (the same float and double steps) on a local copy of the
 * effect. The real ones cannot be called for a preview: Magic_MoveHomingEffect shakes the camera and makes the
 * ground-hit effect when it lands, Magic_CreateCastEffect takes an effect slot and lights, and the floor query
 * (Coll_ResolveFloorAndWalls) writes coll_push_radius. Here the floor is the same walk over the grid's lists
 * (Coll_TestObjectFloor, which writes only its result). Math_Rand is never called: Rock Combo's random rocks are not
 * previewed (their ring of places is shown). Monsters are taken where they stand. */
#include "common.h"
#include "game/battle/battle.h"
#include "game/camera/camera.h"
#include "game/core/main.h"
#include "game/enemy/enemy.h"
#include "game/magic/magic.h"
#include "game/map/collision.h"
#include "game/map/map.h"
#include "game/math/math.h"
#include "game/player/player.h"
#include "game/native_draw/native_draw.h"
#include "game/native_draw/nd_2d.h"
#include "port/mouse_menu.h"
#include "port/os.h"
#include "port/spell_aim.h"
#include "port/spell_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI_F 3.14159265f
#define MAX_ENEMIES 6
#define MAX_CASTS 12  /* cast_count up to 6, doubled by the 50/50/50/50 bonus */
#define MAX_PATH 80   /* a moving effect's life (50, 1.5x with the bonus) and its delay */
#define FLOOR_FLAGS 0xA00 /* the spells' floor query (Magic_MoveHomingEffect) */

/* ---- the switch ------------------------------------------------------------------------------------------------ */

static int host_on = -1; /* the host's bits (4 aim, 8 camera), -1: none (headless) */

int Port_SpellAimEnabled(void) {
    static int env = -2;
    if (host_on >= 0) return (host_on & 4) != 0;
    if (env == -2) env = getenv("ELTALE_SPELL_AIM") ? atoi(getenv("ELTALE_SPELL_AIM")) != 0 : 0;
    return env;
}

static int camera_enabled(void) {
    static int env = -2;
    if (host_on >= 0) return (host_on & 8) != 0;
    if (env == -2) env = getenv("ELTALE_SPELL_AIM_CAMERA") ? atoi(getenv("ELTALE_SPELL_AIM_CAMERA")) != 0 : 1;
    return env;
}

void Port_SpellAimSwitch(int on) {
    if (on >= 0) host_on = on;
}

static int log_on = -1;
static int log_level(void) {
    if (log_on < 0) log_on = getenv("ELTALE_SPELL_AIM_LOG") ? atoi(getenv("ELTALE_SPELL_AIM_LOG")) : 0;
    return log_on;
}
static unsigned long long vi(void) { return (unsigned long long)Port_OsViCount(); }

/* ---- the spell's kind (from its SpellDef: docs/SPELL_AIM.md "Kinds") ---------------------------------------------- */

enum {
    K_NONE,     /* nothing to aim */
    K_SELF,     /* the caster (sel 2): the plate */
    K_ARENA,    /* every enemy (find 1 from sel 1, or an area of 500 and more): all glow */
    K_AROUND,   /* an area round the caster (static, at the feet): its circle */
    K_RANDOM,   /* random places round the caster (place 7): their ring */
    K_POINT,    /* a static area ahead (place 3/4/8/9): its circle, swung by the facing */
    K_TARGET,   /* on the enemy nearest the facing (place 6, or find 1): that enemy's ring */
    K_LINE,     /* a straight mover (homing 0, no landing): its tunnel */
    K_LOB,      /* a mover that lands (floor 1): its arc and its tunnel (where it is low enough, and after it lands) */
    K_FAN,      /* short homing bolts from ahead (place 3, homing): their cone and each one's tunnel */
    K_HOMING,   /* homing movers: each one's tunnel */
};
static const char *const kind_names[] = { "none", "self", "arena", "around", "random", "point", "target", "line",
                                          "lob", "fan", "homing" };

static const SpellDef *spell_def(u32 id) { return &MAGIC_SPELLS((id >> 8) & 0xF)[id & 0xFF]; }

static int spell_kind(u32 id) {
    const SpellDef *d = spell_def(id);
    if (d->target_select_handler_index == 2) return K_SELF;
    if (d->place_handler_index == 7) return K_RANDOM;
    if ((d->find_target_handler_index == 1 && d->target_select_handler_index == 1) || d->effect_extent >= 500.0f)
        return K_ARENA;
    if (d->move_handler_index != 0) {
        if (d->place_handler_index <= 2 && d->target_select_handler_index == 1) return K_AROUND;
        if (d->place_handler_index == 6) return K_TARGET;
        return K_POINT;
    }
    if (d->find_target_handler_index == 1 || d->place_handler_index == 6) return K_TARGET;
    if (d->floor_collision_mode == 1) return K_LOB;
    if (d->homing_strength_index == 0) return K_LINE;
    if (d->place_handler_index == 3) return K_FAN;
    return K_HOMING;
}

/* the facing changes where it goes */
static int directional(int k) { return k >= K_POINT; }

/* ---- the floor (Coll_ResolveFloorAndWalls without its walls and without writing coll_push_radius) ------------------- */

static int floor_y(float x, float y, float z, u16 flags, float *out_y) {
    MapGrid *g = map_subarea_grid;
    CollResult r;
    u16 *list;
    s32 ix = 0, iz = 0, n;
    *out_y = y;
    if (!g) return 0;
    memset(&r, 0, sizeof r);
    r.x = x, r.y = y, r.z = z, r.flags = 0x8000, r.ny = 1.0f;
    if (g->searchPattern == 0) {
        list = g->lists;
    } else {
        ix = (x - g->originX) / g->cellW;
        iz = (z - g->originZ) / g->cellD;
        list = NULL;
        if (ix >= 0 && ix < g->w && iz >= 0 && iz < g->h) list = &g->lists[g->cells[g->w * iz + ix]];
    }
    if (ix >= 0 && ix < g->w && iz >= 0 && iz < g->h && list) {
        for (n = *list++; n != 0; n--)
            if (Coll_TestObjectFloor(x, z, flags, *list++, &r)) goto found;
    }
    list = g->alwaysList;
    for (n = g->alwaysCount; n != 0; n--)
        if (Coll_TestObjectFloor(x, z, flags, *list++, &r)) goto found;
    return 0;
found:
    *out_y = r.y;
    return !(r.flags & 0x8000);
}

/* ---- the game's actors -------------------------------------------------------------------------------------------- */

static Actor *enemy(int i) { return &enemy_slots[i].actor; }
static int alive(int i) { return enemy_slots[i].actor.animSetIndex != -1; }

/* the middle the hit test measures to (Magic_FindTargetInRange's dy), and the radius it adds */
static float middle_y(const Actor *a) {
    switch (a->stats->unk0) {
    case 0: return a->y + a->stats->body_height * a->scale * 0.5f;
    case 1: return a->battle->unk94;
    default: return a->y;
    }
}
static float radius_of(const Actor *a) { return a->stats->body_radius * a->scale; }

/* ---- the copy of the spell code ----------------------------------------------------------------------------------- */

typedef struct SimFx {
    u16 frames_left, start_delay, flags;
    f32 x, y, z, vx, vy, vz;
    Actor *caster, *target;
    const SpellDef *cfg;
    u16 hit_bits;            /* enemies this effect touched (its slot's magic_hit_slot_bits) */
} SimFx;

typedef struct Cast {
    int made;                /* a target was found: the effect exists */
    int target;              /* the enemy it chose (-1: none, or the caster) */
    int path_n;
    float path[MAX_PATH][3]; /* where it was each tick (its place first) */
    /* The tunnel: at each place after a tick, whether the hit test ran there (live) and how far past that place, on
     * the ground, a monster's edge can be and still be touched (w: the hit test's reach, extent + radius, less the
     * halved height difference, less the radius; the most over the battle's monsters, battle_models). A monster
     * whose centre is nearer than w + its radius to a live place is hit there. */
    u8 live[MAX_PATH];
    float w[MAX_PATH];
    int landed, land_k;      /* (land_k: the place where it landed) */
    float land[3];
    float reach;             /* the length of its path */
    u16 hits;
} Cast;

/* A homing bolt's cone: the ground its tunnel can cover whatever monster it homes on (each tunnel of the bolt toward a
 * monster put at every place round the caster, every 5 degrees and 5 units), as the nearest and furthest covered
 * distance from the bolt's first place along each bearing (CONE_BINS, world bearings, sin/cos as Math_Atan2) */
#define CONE_BINS 240
typedef struct Cone {
    int ok;
    float sx, sz;
    float rmin[CONE_BINS], rmax[CONE_BINS];
} Cone;

typedef struct Preview {
    int valid;
    u32 spell;
    int kind;
    float yaw;               /* the facing it was worked out for */
    float px, py, pz;        /* the caster */
    int casts;
    Cast cast[MAX_CASTS];
    u8 hit_count[MAX_ENEMIES]; /* effects that touch each enemy */
    u16 hits;                /* enemies touched */
    float margin[MAX_ENEMIES]; /* the nearest each came to the hit test's reach (its distance less extent + radius) */
    Cone cone[MAX_CASTS];    /* (homing bolts: cone_wanted) */
    float body_r;            /* the battle's smallest monster radius (the tunnel's sides: tunnel_run) */
} Preview;

static void sim_select(SimFx *p) {
    const Actor *caster = p->caster;
    f32 angle, best, d;
    Vec2f v;
    s32 i;
    if (p->cfg->target_select_handler_index == 2) {
        p->target = p->caster;
        return;
    }
    if (p->cfg->target_select_handler_index == 1) { /* Magic_SelectFirstActiveTarget */
        for (i = 0; i < 6 && enemy_slots[i].actor.animSetIndex == -1; i++) {}
        p->target = i < 6 ? enemy(i) : NULL;
        return;
    }
    /* Magic_SelectClosestAimTarget */
    if ((p->cfg->flags & 8) && (p->vx != 0.0f || p->vz != 0.0f)) angle = Math_Atan2(p->vx, p->vz);
    else angle = caster->rot;
    best = 9.424778f;
    s32 n = enemy_count;
    for (i = 0; n != 0 && i < 6; i++) {
        if (!alive(i)) continue;
        v.x = enemy(i)->x - caster->x;
        v.y = enemy(i)->z - caster->z;
        Math_RotateVec2(angle, &v);
        d = Math_Atan2(v.x, v.y);
        if (d > 3.141592653589793) d -= 6.283185307179586;
        else if (d < -3.141592653589793) d += 6.283185307179586;
        if (d < 0.0f) d = -d;
        if (d < best) {
            best = d;
            p->target = enemy(i);
        }
        n--;
    }
}

static void sim_place(SimFx *p, Actor *a) {
    const SpellDef *c = p->cfg;
    Vec2f v;
    switch (c->place_handler_index) {
    case 0: /* at the feet */
        p->x = a->x, p->z = a->z;
        p->y = a->stats->unk0 == 1 ? a->battle->unk94 - a->stats->body_radius * a->scale : a->y;
        break;
    case 1: /* the middle */
        p->x = a->x, p->z = a->z;
        p->y = a->stats->unk0 == 1 ? a->battle->unk94 : a->y + a->stats->body_height * 0.5 * a->scale;
        break;
    case 2: /* above */
        p->x = a->x, p->z = a->z;
        p->y = a->stats->unk0 == 1 ? a->battle->unk94 + a->stats->body_radius * a->scale + c->effect_extent
                                   : a->y + 2.0 * a->stats->body_height * a->scale + c->effect_extent;
        break;
    case 3: /* ahead, the middle */
    case 4: /* ahead, high */
    case 5: /* behind, high */
        v.x = 0.0f;
        v.y = (c->place_handler_index == 5 ? -a->stats->body_radius * a->scale : a->stats->body_radius * a->scale) +
              c->effect_extent;
        Math_RotateVec2(a->rot, &v);
        p->x = a->x + v.x;
        p->z = a->z + v.y;
        if (c->place_handler_index == 3)
            p->y = a->stats->unk0 == 1 ? a->battle->unk94 : a->y + a->stats->body_height * 0.5 * a->scale;
        else
            p->y = a->stats->unk0 == 1 ? a->battle->unk94 + a->stats->body_radius * a->scale + c->effect_extent
                                       : a->y + 2.0 * a->stats->body_height * a->scale + c->effect_extent;
        break;
    case 6: { /* the target */
        Actor *t = p->target ? p->target : a;
        p->x = t->x, p->z = t->z;
        if (t->stats->unk0 == 1) p->y = t->battle->unk94 - t->stats->body_radius * t->scale;
        else if (t->stats->unk0 == 2) p->y = t->y + 100.0f * t->scale;
        else p->y = t->y;
        break;
    }
    case 8: { /* the diagonal point */
        f32 dist, dz;
        if (a->typeFlags & 1) {
            dist = p->target->x - a->x;
            dz = p->target->z - a->z;
            dist = sqrtf(dist * dist + dz * dz);
        } else {
            dist = a->stats->body_radius * 10.0 * a->scale;
        }
        v.y = dist;
        v.x = dist * 1.5;
        Math_RotateVec2(a->rot, &v);
        p->x = a->x + v.x;
        p->z = a->z + v.y;
        p->y = a->stats->unk0 == 1 ? a->battle->unk94 - a->stats->body_radius * a->scale : a->y;
        break;
    }
    case 9:
        v.x = 0.0f;
        v.y = 380.0f;
        Math_RotateVec2(a->rot, &v);
        p->x = a->x + v.x * a->scale;
        p->y = a->y + 380.0f * a->scale;
        p->z = a->z + v.y * a->scale;
        break;
    default: /* 7, random: not previewed */
        p->x = a->x, p->y = a->y, p->z = a->z;
        break;
    }
}

/* Magic_MoveHomingEffect without its camera shake and ground-hit effect; returns 1 when it landed this tick */
static int sim_move_homing(SimFx *p) {
    const MagicSpeedTable speeds = magic_homing_strengths, turns = magic_homing_accels;
    f32 dx, dy, dz, vx, vy, vz, d;
    Actor *t = p->target;
    if (p->flags & 1) return 0;
    if (t != NULL) {
        if ((p->flags & 2) || t->animSetIndex == -1) {
            dz = 0.0f;
            dy = 0.0f;
            dx = 0.0f;
        } else {
            dx = t->x - p->x;
            if (t->stats->unk0 == 1) dy = t->battle->unk94 - p->y;
            else dy = t->y + t->stats->body_height * t->scale * 0.5 - p->y;
            dz = t->z - p->z;
            d = sqrtf(dx * dx + dy * dy + dz * dz);
            dx = speeds.v[p->cfg->homing_strength_index] * dx / d;
            dy = speeds.v[p->cfg->homing_strength_index] * dy / d;
            dz = speeds.v[p->cfg->homing_strength_index] * dz / d;
        }
        vx = p->vx;
        vy = p->vy;
        vz = p->vz;
        d = sqrtf(vx * vx + vy * vy + vz * vz);
        if (d != 0.0f) {
            vx /= (f64)d;
            vy /= (f64)d;
            vz /= (f64)d;
            dx += vx;
            dy += vy;
            dz += vz;
        }
        d = sqrtf(dx * dx + dy * dy + dz * dz);
        dx = turns.v[p->cfg->homing_accel_index] * dx / d;
        dy = turns.v[p->cfg->homing_accel_index] * dy / d;
        dz = turns.v[p->cfg->homing_accel_index] * dz / d;
        p->vx = p->vx * 0.9 + dx;
        p->vy = p->vy * 0.9 + dy;
        p->vz = p->vz * 0.9 + dz;
    }
    if (p->cfg->launch_flags & 1) p->vy -= 0.3;
    p->x += p->vx;
    p->y += p->vy;
    p->z += p->vz;
    if (p->cfg->floor_collision_mode != 0) {
        f32 fy;
        floor_y(p->x, p->y, p->z, FLOOR_FLAGS, &fy);
        if (p->cfg->floor_collision_mode == 1) {
            if (p->y < fy) {
                p->flags |= 1;
                p->frames_left = 8;
                p->vz = 0.0f;
                p->vy = 0.0f;
                p->vx = 0.0f;
                return 1;
            }
        } else if (p->cfg->floor_collision_mode == 2) {
            p->y = fy;
        }
    }
    return 0;
}

/* Magic_MoveVerticalEffect without its camera shake and ground-hit effect */
static int sim_move_vertical(SimFx *p) {
    int landed = 0;
    if (!(p->flags & 1)) {
        if (p->cfg->launch_flags & 1) p->vy -= 0.3;
        p->y += p->vy;
        if (p->cfg->floor_collision_mode == 1) {
            f32 fy;
            floor_y(p->x, p->y, p->z, FLOOR_FLAGS, &fy);
            if (p->y < fy) {
                p->y = fy;
                p->flags |= 1;
                p->frames_left = 8;
                p->vz = 0.0f;
                p->vy = 0.0f;
                p->vx = 0.0f;
                landed = 1;
            }
        }
        if (p->cfg->target_select_handler_index == 0 && !(p->target->animFlags & 1)) {
            p->x = p->target->x;
            p->z = p->target->z;
        }
    }
    return landed;
}

/* the hit test's distance from an effect to an enemy (Magic_FindTargetInRange's: dy to its middle, halved) */
static float sim_distance(const SimFx *p, const Actor *a) {
    f32 dx = a->x - p->x, dz = a->z - p->z, dy;
    switch (a->stats->unk0) {
    default: dy = 0.0f; break;
    case 0: dy = (a->y + a->stats->body_height * a->scale * 0.5 - p->y) * 0.5; break;
    case 1: dy = (a->battle->unk94 - p->y) * 0.5; break;
    }
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Magic_FindTargetInRange (an enemy's target: the enemies), against this effect's own hits */
static int sim_find_range(SimFx *p) {
    if (p->target == NULL || !(p->target->typeFlags & 1)) return -1;
    s32 n = enemy_count;
    for (int i = 0; n != 0 && i < 6; i++) {
        if (!alive(i)) continue;
        Actor *a = enemy(i);
        if (!(p->hit_bits & (1u << i))) {
            f32 dx = a->x - p->x, dz = a->z - p->z, dy, dist;
            switch (a->stats->unk0) {
            default: dy = 0.0f; break;
            case 0: dy = (a->y + a->stats->body_height * a->scale * 0.5 - p->y) * 0.5; break;
            case 1: dy = (a->battle->unk94 - p->y) * 0.5; break;
            }
            dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < p->cfg->effect_extent + a->stats->body_radius * a->scale) return i;
        }
        n--;
    }
    return -1;
}

/* Magic_FindEligibleTarget */
static int sim_find_eligible(SimFx *p) {
    int t = -1;
    for (int i = 0; i < 6; i++)
        if (p->target == enemy(i)) t = i;
    if (p->target == NULL || !(p->target->typeFlags & 1)) return -1;
    if (t >= 0 && !(p->hit_bits & (1u << t))) return t;
    int ret = -1;
    s32 n = enemy_count;
    for (int i = 0; n != 0 && i < 6; i++) {
        if (!alive(i)) continue;
        if (!(p->hit_bits & (1u << i))) ret = i;
        n--;
    }
    return ret;
}

/* ---- the tunnel's width: the battle's monsters as the hit test sees them ------------------------------------------- */

/* A monster as Magic_FindTargetInRange and Magic_MoveHomingEffect see it, wherever it stands: its radius, its middle's
 * height above the ground under it (the hit test's dy and the homing's aim), and whether dy is 0 (kind 2) */
typedef struct Model {
    float r, off;
    int flat;
} Model;
#define MAX_MODELS MAX_ENEMIES

/* the ground under (x, z) near the height ref (the floor query from 40 above it), else ref */
static float ground_y(float x, float z, float ref) {
    float fy;
    if (!floor_y(x, ref + 40.0f, z, FLOOR_FLAGS, &fy) || fy < ref - 60.0f) fy = ref;
    return fy;
}

/* the live monsters' kinds of body (alike ones once); none: a Were-Rabbit's (r 5.6, its middle 5.6 up) */
static int battle_models(Model *m) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!alive(i)) continue;
        const Actor *a = enemy(i);
        Model x;
        x.r = radius_of(a);
        x.flat = a->stats->unk0 == 2;
        if (a->stats->unk0 == 1) x.off = a->battle->unk94 - ground_y(a->x, a->z, a->battle->unk94 - x.r);
        else x.off = (float)(a->stats->body_height * a->scale * 0.5);
        int same = 0;
        for (int k = 0; k < n; k++)
            if (fabsf(m[k].r - x.r) < 0.01f && fabsf(m[k].off - x.off) < 0.01f && m[k].flat == x.flat) same = 1;
        if (!same && n < MAX_MODELS) m[n++] = x;
    }
    if (!n) m[n++] = (Model){ 5.6f, 5.6f, 0 };
    return n;
}

/* How far past an effect at height y (the ground gy under it), on the ground, a monster's edge can be and be touched:
 * for each kind of monster standing there, sqrt((extent + r)^2 - dy^2) - r, dy half the height from the effect to its
 * middle; the most of them. Out of reach of all: -1e9. (Between -r and 0: only a monster right under it.) */
static float bead_w(float y, float gy, float extent, const Model *m, int nm) {
    float best = -1e9f;
    for (int k = 0; k < nm; k++) {
        const float reach = extent + m[k].r;
        float w;
        if (m[k].flat) {
            w = extent;
        } else {
            const float dy = (gy + m[k].off - y) * 0.5f, r2 = reach * reach - dy * dy;
            if (r2 <= 0.0f) continue;
            w = sqrtf(r2) - m[k].r;
        }
        if (w > best) best = w;
    }
    return best;
}

/* the ground near the height ref for the cones' many places and the drawing's corners: on a GSTEP grid, kept while
 * the map and ref stay (and dropped when the spell input closes) */
#define GCACHE 65536
#define GSTEP 2.0f
static struct { int x, z, ok; float y; } gcache[GCACHE];
static float gcache_ref;
static const void *gcache_map;
static void gcache_use(float ref) {
    if (gcache_map == (const void *)map_subarea_grid && fabsf(gcache_ref - ref) < 0.5f) return;
    for (int i = 0; i < GCACHE; i++) gcache[i].ok = 0;
    gcache_ref = ref;
    gcache_map = map_subarea_grid;
}
static float ground_cached(float x, float z) {
    const int ix = (int)floorf(x / GSTEP + 0.5f), iz = (int)floorf(z / GSTEP + 0.5f);
    const unsigned h = ((unsigned)ix * 73856093u ^ (unsigned)iz * 19349663u) & (GCACHE - 1);
    if (!gcache[h].ok || gcache[h].x != ix || gcache[h].z != iz) {
        gcache[h].ok = 1, gcache[h].x = ix, gcache[h].z = iz;
        gcache[h].y = ground_y(ix * GSTEP, iz * GSTEP, gcache_ref);
    }
    return gcache[h].y;
}

/* ---- the cones of homing bolts ------------------------------------------------------------------------------------ */

/* (a preview is worked out at every read while aiming: a cone, many runs of the bolt, is kept per cast and, while only
 * the facing changes, turned with it about the caster; cone_get) */

/* which casts get a cone: Fire Ball's bolts (the fan) */
static int cone_wanted(int kind) { return kind == K_FAN; }

/* Magic_MoveHomingEffect toward a monster whose middle is at (tx, tmid, tz): the same steps as sim_move_homing */
static int hypo_move(SimFx *p, float tx, float tmid, float tz) {
    const MagicSpeedTable speeds = magic_homing_strengths, turns = magic_homing_accels;
    f32 dx, dy, dz, vx, vy, vz, d;
    if (p->flags & 1) return 0;
    if (p->flags & 2) {
        dz = 0.0f;
        dy = 0.0f;
        dx = 0.0f;
    } else {
        dx = tx - p->x;
        dy = tmid - p->y;
        dz = tz - p->z;
        d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d == 0.0f) d = 1e-6f;
        dx = speeds.v[p->cfg->homing_strength_index] * dx / d;
        dy = speeds.v[p->cfg->homing_strength_index] * dy / d;
        dz = speeds.v[p->cfg->homing_strength_index] * dz / d;
    }
    vx = p->vx;
    vy = p->vy;
    vz = p->vz;
    d = sqrtf(vx * vx + vy * vy + vz * vz);
    if (d != 0.0f) {
        vx /= (f64)d;
        vy /= (f64)d;
        vz /= (f64)d;
        dx += vx;
        dy += vy;
        dz += vz;
    }
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d == 0.0f) d = 1e-6f;
    dx = turns.v[p->cfg->homing_accel_index] * dx / d;
    dy = turns.v[p->cfg->homing_accel_index] * dy / d;
    dz = turns.v[p->cfg->homing_accel_index] * dz / d;
    p->vx = p->vx * 0.9 + dx;
    p->vy = p->vy * 0.9 + dy;
    p->vz = p->vz * 0.9 + dz;
    if (p->cfg->launch_flags & 1) p->vy -= 0.3;
    p->x += p->vx;
    p->y += p->vy;
    p->z += p->vz;
    if (p->cfg->floor_collision_mode != 0) {
        const f32 fy = ground_cached(p->x, p->z);
        if (p->cfg->floor_collision_mode == 1) {
            if (p->y < fy) {
                p->flags |= 1;
                p->frames_left = 8;
                p->vz = 0.0f;
                p->vy = 0.0f;
                p->vx = 0.0f;
                return 1;
            }
        } else if (p->cfg->floor_collision_mode == 2) {
            p->y = fy;
        }
    }
    return 0;
}

/* the bins' bearings (sin, cos) */
static float bin_dir[CONE_BINS][2];
static void bins_init(void) {
    if (bin_dir[0][1] == 1.0f) return;
    for (int i = 0; i < CONE_BINS; i++) bin_dir[i][0] = sinf(i * 2.0f * PI_F / CONE_BINS), bin_dir[i][1] = cosf(i * 2.0f * PI_F / CONE_BINS);
}

/* a live place of a tunnel into the cone: the bearings it covers from (sx, sz), nearest and furthest */
static void cone_add(Cone *c, float bx, float bz, float w) {
    if (w <= 0.0f) return;
    const float ox = bx - c->sx, oz = bz - c->sz, d = sqrtf(ox * ox + oz * oz);
    int b0 = 0, b1 = CONE_BINS - 1;
    const float step = 2.0f * PI_F / CONE_BINS;
    if (d > w) {
        const float beta = atan2f(ox, oz), alpha = asinf(w / d);
        b0 = (int)floorf((beta - alpha) / step);
        b1 = (int)ceilf((beta + alpha) / step);
    }
    for (int b = b0; b <= b1; b++) {
        const int i = ((b % CONE_BINS) + CONE_BINS) % CONE_BINS;
        const float ux = bin_dir[i][0], uz = bin_dir[i][1];
        const float along = ox * ux + oz * uz, perp2 = d * d - along * along;
        if (perp2 > w * w) continue;
        const float half = sqrtf(w * w - perp2);
        float t0 = along - half, t1 = along + half;
        if (t1 < 0.0f) continue;
        if (t0 < 0.0f) t0 = 0.0f;
        if (t0 < c->rmin[i]) c->rmin[i] = t0;
        if (t1 > c->rmax[i]) c->rmax[i] = t1;
    }
}

/* The cone of a homing cast (its effect as placed, before its first tick): its tunnel toward a monster of each kind put
 * at every place round the caster (every 5 degrees, every 5 units out to the bolt's reach and 30 more), each run as
 * the game would (its delay, its life, the hit on that monster ending it or, piercing, flying on straight). */
static void cone_build(Cone *c, const SimFx *init, const Actor *caster, const Model *m, int nm) {
    const SpellDef *def = init->cfg;
    bins_init();
    c->ok = 1;
    c->sx = init->x, c->sz = init->z;
    for (int i = 0; i < CONE_BINS; i++) c->rmin[i] = 1e9f, c->rmax[i] = -1.0f;
    /* how far it goes after a monster far ahead (along its launch), and 30 more: the furthest monster to try */
    float far = 0.0f;
    {
        SimFx p = *init;
        const float hl = sqrtf(p.vx * p.vx + p.vz * p.vz);
        const float ux = hl > 1e-4f ? p.vx / hl : sinf(caster->rot), uz = hl > 1e-4f ? p.vz / hl : cosf(caster->rot);
        int guard = 0;
        while (p.frames_left != 0 && guard++ < 400) {
            if (p.start_delay != 0) {
                p.start_delay--;
                continue;
            }
            p.frames_left--;
            hypo_move(&p, init->x + ux * 1000.0f, init->y, init->z + uz * 1000.0f);
            const float dx = p.x - caster->x, dz = p.z - caster->z, d = sqrtf(dx * dx + dz * dz);
            if (d > far) far = d;
        }
        far += 30.0f;
    }
    for (int k = 0; k < nm; k++)
        for (int a = 0; a < 72; a++) {
            const float th = a * (2.0f * PI_F / 72.0f), ux = sinf(th), uz = cosf(th);
            for (float dist = 5.0f; dist <= far; dist += 5.0f) {
                const float tx = caster->x + ux * dist, tz = caster->z + uz * dist;
                const float tmid = ground_cached(tx, tz) + m[k].off;
                SimFx p = *init;
                int guard = 0;
                while (p.frames_left != 0 && guard++ < 400) {
                    if (p.start_delay != 0) {
                        p.start_delay--;
                        continue;
                    }
                    p.frames_left--;
                    hypo_move(&p, tx, tmid, tz);
                    const int live = def->find_target_handler_index == 0 || (def->find_target_handler_index == 2 && (p.flags & 1));
                    if (!live) continue;
                    cone_add(c, p.x, p.z, bead_w(p.y, ground_cached(p.x, p.z), def->effect_extent, m, nm));
                    if (p.flags & 2) continue; /* (piercing, after its hit: on straight, no other hit on it) */
                    const float ddx = tx - p.x, ddz = tz - p.z, ddy = m[k].flat ? 0.0f : (tmid - p.y) * 0.5f;
                    if (sqrtf(ddx * ddx + ddy * ddy + ddz * ddz) < def->effect_extent + m[k].r) {
                        if (!(def->flags & 4)) p.frames_left = 0;
                        else p.flags |= 2;
                    }
                }
            }
        }
}

/* The cone of cast n, from the kept one when only the facing changed since (turned by the change: the cones are the
 * same round the caster whichever way it faces, but for the ground's slope under them), else built. fresh: built. */
static struct {
    int ok;
    u32 spell;
    u16 flags;
    float x, y, z, yaw;
    Model m[MAX_MODELS];
    int nm;
    Cone cone;
} cone_kept[MAX_CASTS];

static void cone_turn(Cone *dst, const Cone *src, float d_yaw, float sx, float sz) {
    const float step = 2.0f * PI_F / CONE_BINS, shift = d_yaw / step;
    dst->ok = 1;
    dst->sx = sx, dst->sz = sz;
    for (int i = 0; i < CONE_BINS; i++) {
        /* the bearing it came from, between two kept bins */
        const float f = i - shift;
        const int a = ((int)floorf(f) % CONE_BINS + CONE_BINS) % CONE_BINS, b = (a + 1) % CONE_BINS;
        const float t = f - floorf(f);
        if (src->rmax[a] < 0.0f || src->rmax[b] < 0.0f) {
            const int k = t < 0.5f ? a : b;
            dst->rmin[i] = src->rmin[k], dst->rmax[i] = src->rmax[k];
            continue;
        }
        dst->rmin[i] = src->rmin[a] + (src->rmin[b] - src->rmin[a]) * t;
        dst->rmax[i] = src->rmax[a] + (src->rmax[b] - src->rmax[a]) * t;
    }
}

static int cone_builds; /* (cones built at this preview: one a read while aiming, the others then follow, a read each) */
static void cone_get(Cone *c, int n, int fresh, u32 spell, u16 flags, const SimFx *init, const Actor *caster, const Model *m, int nm) {
    typeof(cone_kept[0]) *k = &cone_kept[n];
    int same = k->ok && k->spell == spell && k->flags == flags && k->x == caster->x && k->y == caster->y &&
                     k->z == caster->z && k->nm == nm;
    for (int i = 0; same && i < nm; i++) /* (a flying monster's height bobs: within half a unit, the same) */
        same = k->m[i].flat == m[i].flat && fabsf(k->m[i].r - m[i].r) < 0.01f && fabsf(k->m[i].off - m[i].off) < 0.5f;
    if (same && !fresh) {
        float d = caster->rot - k->yaw;
        cone_turn(c, &k->cone, d, init->x, init->z);
        return;
    }
    if (!fresh && cone_builds++) return; /* (c->ok 0: not drawn yet) */
    cone_build(c, init, caster, m, nm);
    k->ok = 1, k->spell = spell, k->flags = flags;
    k->x = caster->x, k->y = caster->y, k->z = caster->z, k->yaw = caster->rot;
    memcpy(k->m, m, sizeof(Model) * (size_t)nm);
    k->nm = nm;
    k->cone = *c;
}

static void simulate(u32 id, float yaw, Preview *out, int fresh) {
    const SpellDef *def = spell_def(id);
    const MagicSpeedTable sizes = magic_launch_speeds;
    Actor caster = player_state.actor; /* (a copy: its facing is the aim's) */
    const u16 cast_flags = (player_state.flags & 0x10000) ? 0x10 : 0;
    int casts = 1;
    memset(out, 0, sizeof *out);
    caster.rot = yaw;
    out->valid = 1;
    out->spell = id;
    out->kind = spell_kind(id);
    out->yaw = yaw;
    out->px = caster.x, out->py = caster.y, out->pz = caster.z;
    if (def->cast_count >= 2) casts = 1 + (def->cast_count - 1) * ((cast_flags & 0x10) ? 2 : 1);
    if (casts > MAX_CASTS) casts = MAX_CASTS;
    out->casts = casts;
    for (int i = 0; i < MAX_ENEMIES; i++) out->margin[i] = 9999.0f;
    if (out->kind == K_SELF || out->kind == K_RANDOM) return;
    Model models[MAX_MODELS];
    const int nm = battle_models(models);
    out->body_r = models[0].r;
    for (int k = 1; k < nm; k++)
        if (models[k].r < out->body_r) out->body_r = models[k].r;
    const int cones = cone_wanted(out->kind);
    if (cones) gcache_use(caster.y);
    cone_builds = 0;
    for (int n = 0; n < casts; n++) {
        Cast *c = &out->cast[n];
        SimFx p;
        memset(&p, 0, sizeof p);
        c->target = -1;
        p.caster = &caster;
        p.cfg = def;
        p.frames_left = def->effect_lifetime;
        p.start_delay = def->start_delay;
        p.flags = cast_flags;
        if (def->move_handler_index == 0) {
            Vec2f v;
            const int dir = (def->launch_direction_index + n) & 7;
            v.x = D_800C1878[dir].x * sizes.v[def->launch_speed_index];
            v.y = D_800C1878[dir].z * sizes.v[def->launch_speed_index];
            Math_RotateVec2(caster.rot, &v);
            p.vx = v.x;
            p.vz = v.y;
            p.vy = (def->launch_flags & 2) ? 0.0f : D_800C1878[dir].y * sizes.v[def->launch_speed_index];
            if (cast_flags & 0x10) p.frames_left += p.frames_left >> 1;
        }
        sim_select(&p);
        if (p.target == NULL) continue;
        for (int i = 0; i < 6; i++)
            if (p.target == enemy(i)) c->target = i;
        sim_place(&p, &caster);
        if (def->floor_collision_mode == 2) floor_y(p.x, p.y, p.z, FLOOR_FLAGS, &p.y);
        c->made = 1;
        c->path[0][0] = p.x, c->path[0][1] = p.y, c->path[0][2] = p.z;
        c->path_n = 1;
        const SimFx init = p;
        int guard = 0;
        while (p.frames_left != 0 && guard++ < 400) {
            if (p.start_delay != 0) {
                p.start_delay--;
                continue;
            }
            p.frames_left--;
            const float ox = p.x, oz = p.z;
            int landed = def->move_handler_index == 0 ? sim_move_homing(&p) : sim_move_vertical(&p);
            if (landed && !c->landed) {
                c->landed = 1;
                c->land_k = c->path_n;
                c->land[0] = p.x, c->land[1] = p.y, c->land[2] = p.z;
            }
            c->reach += sqrtf((p.x - ox) * (p.x - ox) + (p.z - oz) * (p.z - oz));
            if (c->path_n < MAX_PATH) {
                const int live = def->find_target_handler_index == 0 || (def->find_target_handler_index == 2 && (p.flags & 1));
                c->path[c->path_n][0] = p.x, c->path[c->path_n][1] = p.y, c->path[c->path_n][2] = p.z;
                c->live[c->path_n] = (u8)live;
                c->w[c->path_n] = live ? bead_w(p.y, ground_y(p.x, p.z, caster.y), def->effect_extent, models, nm) : -1e9f;
                c->path_n++;
            }
            if (def->find_target_handler_index == 0 || (def->find_target_handler_index == 2 && (p.flags & 1)))
                for (int i = 0; i < MAX_ENEMIES; i++) {
                    if (!alive(i) || (p.hit_bits & (1u << i))) continue;
                    const float m = sim_distance(&p, enemy(i)) - (def->effect_extent + radius_of(enemy(i)));
                    if (m < out->margin[i]) out->margin[i] = m;
                }
            int hit = -1;
            switch (def->find_target_handler_index) {
            case 0: hit = sim_find_range(&p); break;
            case 1: hit = sim_find_eligible(&p); break;
            case 2: hit = (p.flags & 1) ? sim_find_range(&p) : -1; break;
            }
            if (hit >= 0) {
                p.hit_bits |= (u16)(1u << hit);
                if (!(def->flags & 4)) p.frames_left = 0;
                else p.flags |= 2;
            }
        }
        c->hits = p.hit_bits;
        out->hits |= p.hit_bits;
        if (cones && def->move_handler_index == 0 && def->homing_strength_index != 0) {
            cone_get(&out->cone[n], n, fresh, id, cast_flags, &init, &caster, models, nm);
            /* (and the predicted tunnel itself) */
            for (int k = 1; k < c->path_n; k++)
                if (c->live[k]) cone_add(&out->cone[n], c->path[k][0], c->path[k][2], c->w[k]);
        }
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (p.hit_bits & (1u << i)) out->hit_count[i]++;
    }
}

static void build(const Preview *p);
static int nverts;

/* ---- the view the last picture was drawn with (for the pointer) -------------------------------------------------- */

static struct {
    int ok;
    float vp[16];   /* view x perspective (row vectors: clip = p * vp) */
    float inv[16];
    float aspect;
    float right[3]; /* the view's right, in the world */
} view;

static int invert4(const float *m, float *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return 0;
        for (int k = 0; k < 8; k++) {
            double t = a[c][k];
            a[c][k] = a[p][k];
            a[p][k] = t;
        }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)a[r][c + 4];
    return 1;
}

void Port_SpellAimViewDrawn(void) {
    if (!Port_SpellAimEnabled()) return;
    const float(*v)[4] = camera_view_mtx.mf, (*p)[4] = camera_proj_mtx.mf;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += v[r][k] * p[k][c];
            view.vp[r * 4 + c] = s;
        }
    view.aspect = ND_ASPECT();
    view.right[0] = v[0][0], view.right[1] = v[1][0], view.right[2] = v[2][0];
    view.ok = invert4(view.vp, view.inv);
}

/* a world point to CENTRE units (x 160 the picture's middle, y down 240); 0 behind the eye */
static int project(float x, float y, float z, float *sx, float *sy) {
    const float *m = view.vp;
    float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (cw <= 0.01f) return 0;
    *sx = 160.0f + cx / cw * 120.0f * view.aspect;
    *sy = 120.0f - cy / cw * 120.0f;
    return 1;
}

/* the pointer's ray (CENTRE units) on the plane y = py; 0 when it does not meet it ahead */
static int ground_point(float sx, float sy, float py, float *gx, float *gz) {
    const float nx = (sx - 160.0f) / (120.0f * view.aspect), ny = (120.0f - sy) / 120.0f;
    float p[2][3];
    for (int k = 0; k < 2; k++) {
        const float c[4] = { nx, ny, k ? 1.0f : -1.0f, 1.0f };
        float w[4];
        for (int j = 0; j < 4; j++) w[j] = c[0] * view.inv[j] + c[1] * view.inv[4 + j] + c[2] * view.inv[8 + j] + c[3] * view.inv[12 + j];
        if (fabsf(w[3]) < 1e-9f) return 0;
        for (int j = 0; j < 3; j++) p[k][j] = w[j] / w[3];
    }
    const float dy = p[1][1] - p[0][1];
    if (fabsf(dy) < 1e-6f) return 0;
    const float t = (py - p[0][1]) / dy;
    if (t <= 0.0f) return 0;
    *gx = p[0][0] + (p[1][0] - p[0][0]) * t;
    *gz = p[0][2] + (p[1][2] - p[0][2]) * t;
    return 1;
}

/* A monster's body on the picture (CENTRE units), as the pick takes it: a column from its feet (f) to its top (t),
 * half wide each side. The width is BODY_HALF of its radius (the models are slimmer than the radius the hit test adds;
 * a Were-Rabbit's cloak is about 0.6 of it); the feet end goes down to the near edge of its footprint (its radius round
 * its feet on the ground), where its legs are drawn toward the camera. 0 when it is not in front of the eye. */
#define BODY_HALF 0.6f
static int body_on_screen(const Actor *a, float *fx, float *fy, float *tx, float *ty, float *half) {
    const float r = radius_of(a), mid = middle_y(a);
    const float feet = a->stats->unk0 == 1 ? mid - r : a->y;
    const float top = a->stats->unk0 == 1 ? mid + r : a->y + a->stats->body_height * a->scale;
    float mx, my, rx, ry;
    if (!project(a->x, feet, a->z, fx, fy) || !project(a->x, top, a->z, tx, ty) || !project(a->x, mid, a->z, &mx, &my) ||
        !project(a->x + view.right[0] * r, mid + view.right[1] * r, a->z + view.right[2] * r, &rx, &ry))
        return 0;
    *half = sqrtf((rx - mx) * (rx - mx) + (ry - my) * (ry - my)) * BODY_HALF;
    /* the footprint's edge toward the camera and away (along the ground, square to the view's right): the lower one */
    const float gx = -view.right[2], gz = view.right[0], gl = sqrtf(gx * gx + gz * gz);
    if (gl > 1e-4f)
        for (int k = -1; k <= 1; k += 2) {
            float px, py;
            if (project(a->x + gx / gl * r * k, feet, a->z + gz / gl * r * k, &px, &py) && py > *fy) *fy = py;
        }
    return 1;
}

/* The monster under the pointer (CENTRE units): on its body (body_on_screen) or PICK_IN beyond it; the one picked already
 * stays picked to PICK_STAY beyond it (no flicker at the edge). Just off a body is the ground: pointing past a monster
 * aims there freely. */
#define PICK_IN 1.0f
#define PICK_STAY 3.0f

/* how far (CENTRE units) a point is outside a body (under 0: inside): past its sides or its ends */
static float body_distance(float sx, float sy, float fx, float fy, float tx, float ty, float half) {
    const float ex = tx - fx, ey = ty - fy, len = sqrtf(ex * ex + ey * ey);
    if (len < 1e-4f) return sqrtf((sx - fx) * (sx - fx) + (sy - fy) * (sy - fy)) - half;
    const float ux = ex / len, uy = ey / len;
    const float along = (sx - fx) * ux + (sy - fy) * uy, side = fabsf((sx - fx) * uy - (sy - fy) * ux);
    const float past = along < 0.0f ? -along : along > len ? along - len : 0.0f;
    const float out = side - half;
    return out > past ? out : past;
}

static int enemy_at(float sx, float sy, int current) {
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!alive(i)) continue;
        float fx, fy, tx, ty, half;
        if (!body_on_screen(enemy(i), &fx, &fy, &tx, &ty, &half)) continue;
        const float d = body_distance(sx, sy, fx, fy, tx, ty, half) - (i == current ? PICK_STAY : PICK_IN);
        if (d <= 0.0f && d < best_d) best_d = d, best = i;
    }
    return best;
}

/* ---- the state ---------------------------------------------------------------------------------------------------- */

static struct {
    int session;           /* aiming (sticky from the pointer's first move onto the battlefield until undone or cast) */
    int disarmed;          /* after an undo: not again until the pointer has moved on the panel */
    float disarm_x, disarm_y;
    int last_count;
    int active;            /* this read: the pointer aims */
    int target;            /* the monster under the pointer, -1 the ground */
    float gx, gz;          /* the point aimed at */
    int pending;           /* a click: the facing goes on at this read's cast */
    int pending_target;
    float pending_x, pending_z;
    u32 pending_spell;
    unsigned long long pending_vi;
    Preview preview;       /* what is drawn */
    char last_hover[160];
    /* the cast's hits, for the log (P1_AIM_HITS): its effect slots and what they touched */
    int track;
    u32 track_spell;
    u16 track_predicted, track_actual, track_slots;
    int track_ticks, track_seen;
    u16 track_seen_pairs[16]; /* (slot, enemy) hits logged */
    unsigned tick;
} A = { .target = -1 };

/* The spell input holds a spell the game would cast now, in a battle; its id or -1 */
static s32 ready_spell(void) {
    if (core_game_mode != 1 || !(battle_flags & 1) || !player_state.stats) return -1;
    if (!(core_flags & 4) || !player_state.spell_input_count || player_state.spell_input_count > 3) return -1;
    if (core_flags & (0x1 | 0x2 | 0x8 | 0x80 | 0x400 | 0x800 | 0x4000)) return -1;
    u8 d[3];
    memcpy(d, player_state.spell_input_dirs, 3);
    u32 id = (u32)Magic_LookupSpellSequence(player_state.spell_input_count, d);
    if (Magic_GetSpellRestrictionFlags(id, player_state.stats->mp) != 0) return -1;
    return (s32)id;
}

int Port_SpellAimCollapsed(void) { return A.session && Port_SpellAimEnabled() && ready_spell() >= 0; }

int Port_SpellAimLiftWanted(void) { return Port_SpellAimCollapsed() && camera_enabled(); }

void Port_SpellAimPlateRect(s32 *x, s32 *y, s32 *w, s32 *h) {
    /* the top, right of the HP block (it ends at x 116 at 4:3), its hint under it (the enemy's panel, x 181 and y 26 on,
     * shows on the enemy's turn only); the lower half is where the player and the areas round them are */
    *w = 184, *h = 14;
    *x = 122;
    *y = 9;
}

static void log_preview(const char *tag, const Preview *p) {
    printf("%s vi=%llu spell=%d.%d kind=%s yaw=%.3f hits=%x counts=%d%d%d%d%d%d", tag, vi(), (int)(p->spell >> 8) & 0xF,
           (int)p->spell & 0xFF, kind_names[p->kind], p->yaw, p->hits, p->hit_count[0], p->hit_count[1], p->hit_count[2],
           p->hit_count[3], p->hit_count[4], p->hit_count[5]);
    for (int n = 0; n < p->casts; n++) {
        const Cast *c = &p->cast[n];
        if (!c->made) {
            printf(" c%d=none", n);
            continue;
        }
        printf(" c%d=t%d,%.1f,%.1f>%.1f,%.1f", n, c->target, c->path[0][0], c->path[0][2], c->path[c->path_n - 1][0],
               c->path[c->path_n - 1][2]);
        if (c->landed) printf(",land%.1f,%.1f", c->land[0], c->land[2]);
        printf(",reach%.1f,hit%x", c->reach, c->hits);
    }
    /* how near each monster came to being hit (the hit test's distance less its reach; under 0: hit) */
    printf(" near=");
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (alive(i)) printf("e%d:%.2f,", i, p->margin[i]);
    printf("\n");
}

/* the tunnels and cones as drawn (the tests check the game's hits against them): each cast's live places with their
 * disc (x, z, w; w <= 0 not drawn), and each cone's covered bearings (bin:nearest:furthest) */
static void log_shapes(const Preview *p) {
    for (int n = 0; n < p->casts; n++) {
        const Cast *c = &p->cast[n];
        if (!c->made) continue;
        printf("P1_AIM_TUNNEL vi=%llu spell=%d.%d cast=%d land=%d beads=", vi(), (int)(p->spell >> 8) & 0xF, (int)p->spell & 0xFF, n,
               c->landed ? c->land_k : -1);
        for (int k = 1; k < c->path_n; k++)
            if (c->live[k] && c->w[k] > -1e8f) printf("%d:%.2f,%.2f,%.2f,%.2f;", k, c->path[k][0], c->path[k][1], c->path[k][2], c->w[k]);
        printf("\n");
        const Cone *cn = &p->cone[n];
        if (!cn->ok) continue;
        printf("P1_AIM_CONE vi=%llu spell=%d.%d cast=%d start=%.2f,%.2f bins=%d r=", vi(), (int)(p->spell >> 8) & 0xF,
               (int)p->spell & 0xFF, n, cn->sx, cn->sz, CONE_BINS);
        for (int i = 0; i < CONE_BINS; i++)
            if (cn->rmax[i] >= 0.0f) printf("%d:%.2f:%.2f;", i, cn->rmin[i], cn->rmax[i]);
        printf("\n");
    }
}

static void log_enemies(void) {
    printf("P1_AIM_ENEMIES vi=%llu player=%.2f,%.2f,%.2f rot=%.3f", vi(), player_state.actor.x, player_state.actor.y,
           player_state.actor.z, player_state.actor.rot);
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (alive(i)) {
            float sx = -1.0f, sy = -1.0f;
            if (view.ok) project(enemy(i)->x, middle_y(enemy(i)), enemy(i)->z, &sx, &sy);
            printf(" e%d=%.2f,%.2f,%.2f,r%.2f,k%d,s%.0f,%.0f", i, enemy(i)->x, enemy(i)->y, enemy(i)->z, radius_of(enemy(i)),
                   enemy(i)->stats->unk0, sx, sy);
        }
    printf("\n");
}

static void dump_spells(void) {
    static int done;
    if (done || log_level() < 2 || !player_state.stats) return;
    done = 1;
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < 15; i++) {
            const SpellDef *d = &MAGIC_SPELLS(g)[i];
            printf("P1_AIM_SPELLDEF %d.%d kind=%s flags=%x life=%d delay=%d casts=%d/%d sel=%d place=%d move=%d find=%d floor=%d "
                   "extent=%g home=%d acc=%d launch=%d dir=%d speed=%d level=%d seq=%d%d%d len=%d\n",
                   g, i, kind_names[spell_kind((u32)(g << 8 | i))], d->flags, d->effect_lifetime, d->start_delay, d->cast_count,
                   d->repeat_interval, d->target_select_handler_index, d->place_handler_index, d->move_handler_index,
                   d->find_target_handler_index, d->floor_collision_mode, d->effect_extent, d->homing_strength_index,
                   d->homing_accel_index, d->launch_flags, d->launch_direction_index, d->launch_speed_index, d->required_level,
                   d->input_sequence[0], d->input_sequence[1], d->input_sequence[2], d->input_length);
        }
}

/* the real effects (ELTALE_SPELL_AIM_LOG=2: their places each tick, to check the copy against) and the cast's hits */
static void watch_effects(void) {
    if (log_level() >= 2) {
        for (int s = 0; s < 16; s++) {
            const MagicEffect *e = &magic_effects[s];
            if (!e->frames_left) continue;
            printf("P1_AIM_FX vi=%llu slot=%d spell=%d.%d left=%d delay=%d flags=%x pos=%.2f,%.2f,%.2f v=%.2f,%.2f,%.2f\n", vi(), s,
                   e->spell_group, e->spell_index, e->frames_left, e->start_delay, e->flags, e->x, e->y, e->z, e->vx, e->vy, e->vz);
        }
    }
    if (!A.track) return;
    int alive_fx = 0;
    /* (the effect is made 18 ticks after the cast starts, Player_StateCastSpell: from then on every slot of this
     * spell and caster is this cast's, a one-tick effect's too, whose slot is free again by the next read) */
    const int made = !(player_state.state == 0xE && player_state.unk124 != 0);
    for (int s = 0; s < 16; s++) {
        const MagicEffect *e = &magic_effects[s];
        if (e->caster != &player_state.actor || (u32)(e->spell_group << 8 | e->spell_index) != A.track_spell) continue;
        if (e->frames_left) alive_fx = 1;
        if (made) A.track_slots |= (u16)(1u << s), A.track_seen = 1;
        if (!(A.track_slots & (1u << s))) continue;
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (enemy_slots[i].battle.magic_hit_slot_bits & magic_slot_bits[s]) {
                if (log_level() && !(A.track_seen_pairs[s] & (1u << i))) {
                    const Actor *a = enemy(i);
                    printf("P1_AIM_HITAT vi=%llu slot=%d enemy=%d fx=%.2f,%.2f,%.2f left=%d flags=%x at=%.2f,%.2f,%.2f r=%.2f\n", vi(), s,
                           i, e->x, e->y, e->z, e->frames_left, e->flags, a->x, a->y, a->z, radius_of(a));
                }
                A.track_seen_pairs[s] |= (u16)(1u << i);
                A.track_actual |= (u16)(1u << i);
            }
    }
    int repeats = 0;
    for (int r = 0; r < 4; r++)
        if (magic_cast_repeats[r].casts_left && magic_cast_repeats[r].caster == &player_state.actor) repeats = 1;
    if (alive_fx || repeats) A.track_seen = 1;
    A.track_ticks++;
    if ((A.track_seen && !alive_fx && !repeats && player_state.state != 0xE) || A.track_ticks > 400) {
        if (log_level())
            printf("P1_AIM_HITS vi=%llu spell=%d.%d predicted=%x actual=%x slots=%x %s\n", vi(), (int)(A.track_spell >> 8) & 0xF,
                   (int)A.track_spell & 0xFF, A.track_predicted, A.track_actual, A.track_slots,
                   spell_kind(A.track_spell) == K_RANDOM      ? "random" /* (Rock Combo's places: not previewed) */
                   : A.track_predicted == A.track_actual ? "match"
                                                         : "DIFFER");
        A.track = 0;
    }
}

void Port_SpellAimRead(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    (void)stick_x;
    (void)stick_y;
    A.tick++;
    if (A.pending) {
        if (log_level()) printf("P1_AIM_DROPPED vi=%llu (no cast at the click's read)\n", vi());
        A.pending = 0;
    }
    if (!Port_SpellAimEnabled()) {
        A.session = A.active = 0;
        A.preview.valid = 0;
        A.track = 0;
        return;
    }
    dump_spells();
    watch_effects();
    const s32 id = ready_spell();
    const int count = player_state.stats ? player_state.spell_input_count : 0;
    const int kind = id >= 0 ? spell_kind((u32)id) : K_NONE;
    if (!(core_flags & 4) || !count) A.disarmed = 0; /* (a new spell input: armed) */
    const int undo = count < A.last_count && (core_flags & 4); /* an undo: the panel stays open (below) */
    if (id < 0 || kind == K_SELF || count < A.last_count) {
        if (A.session && log_level()) printf("P1_AIM_PANEL vi=%llu collapsed=0 why=%s\n", vi(), id < 0 ? "not-ready" : kind == K_SELF ? "self" : "undo");
        A.session = 0;
    }
    A.last_count = (core_flags & 4) ? count : 0;
    A.active = 0;
    if (id < 0 || kind == K_SELF) {
        A.preview.valid = 0;
        gcache_map = NULL; /* (the kept ground: again for the next spell, the map may have changed) */
        return;
    }

    /* the pointer: over the battlefield, not on the game's targets (the HUD's crystals) nor the spell UI's */
    float x, y;
    int flags;
    Port_MousePointerGet(&x, &y, &flags);
    Port_MouseToAnchor(PORT_MOUSE_CENTRE, &x, &y);
    const int in = (flags & PORT_POINTER_IN) != 0;
    const int over_panel = Port_SpellUiPointerOver();
    const int over_ui = Port_MouseOverTarget() || over_panel;
    A.active = in && !over_ui && view.ok;
    /* After an undo the panel stays open (the pointer may be on its way to another crystal) until the pointer has moved
     * on the panel's area and leaves it again; meanwhile the battlefield outside it still aims (and a click casts).
     * Without the spell UI there is no panel to keep: aiming goes on at once. */
    if (undo) A.disarmed = 1, A.disarm_x = x, A.disarm_y = y;
    else if ((in && over_panel && (x != A.disarm_x || y != A.disarm_y)) || !Port_SpellUiEnabled()) A.disarmed = 0;
    if (A.active && !A.session && !A.disarmed) {
        A.session = 1;
        if (log_level()) {
            printf("P1_AIM_PANEL vi=%llu collapsed=%d why=aim\n", vi(), Port_SpellUiEnabled());
            log_enemies();
        }
    }

    /* the facing: toward the monster or the ground point under the pointer; else the player's own */
    float yaw = player_state.actor.rot;
    if (!A.active) A.target = -1;
    if (A.active) {
        A.target = enemy_at(x, y, A.target);
        if (A.target >= 0) {
            A.gx = enemy(A.target)->x;
            A.gz = enemy(A.target)->z;
        } else if (!ground_point(x, y, player_state.actor.y, &A.gx, &A.gz)) {
            /* (above the horizon: the pointer's way on the ground, far off) */
            float fx, fz;
            if (ground_point(x, 240.0f, player_state.actor.y, &fx, &fz)) {
                A.gx = player_state.actor.x + (fx - player_state.actor.x) * 50.0f;
                A.gz = player_state.actor.z + (fz - player_state.actor.z) * 50.0f;
            } else {
                A.gx = player_state.actor.x + sinf(yaw), A.gz = player_state.actor.z + cosf(yaw);
            }
        }
        if (directional(kind) && (A.gx != player_state.actor.x || A.gz != player_state.actor.z))
            yaw = Math_Atan2(A.gx - player_state.actor.x, A.gz - player_state.actor.z);
    }
    simulate((u32)id, yaw, &A.preview, 0);

    if (log_level()) {
        char h[160];
        static float logged_yaw;
        snprintf(h, sizeof h, "%d %d %d %x", (int)id, A.active, A.target, A.preview.hits);
        if (strcmp(h, A.last_hover) || fabsf(A.preview.yaw - logged_yaw) > 0.05f) {
            strcpy(A.last_hover, h);
            logged_yaw = A.preview.yaw;
            printf("P1_AIM_HOVER vi=%llu active=%d pointer=%.1f,%.1f target=%d ground=%.2f,%.2f ", vi(), A.active, x, y, A.target, A.gx,
                   A.gz);
            log_preview("preview", &A.preview);
        }
        /* where the monsters and the player are on the picture (CENTRE units: the tests point at them) */
        static char last_screen[200];
        char sc[200];
        int n = 0;
        float sx, sy;
        sc[0] = 0;
        if (project(player_state.actor.x, player_state.actor.y, player_state.actor.z, &sx, &sy))
            n += snprintf(sc + n, sizeof sc - (size_t)n, " player=%.0f,%.0f", sx, sy);
        for (int i = 0; i < MAX_ENEMIES && n < (int)sizeof sc - 24; i++)
            if (alive(i) && project(enemy(i)->x, middle_y(enemy(i)), enemy(i)->z, &sx, &sy))
                n += snprintf(sc + n, sizeof sc - (size_t)n, " e%d=%.0f,%.0f", i, sx, sy);
        if (strcmp(sc, last_screen)) {
            strcpy(last_screen, sc);
            printf("P1_AIM_SCREEN vi=%llu%s\n", vi(), sc);
            /* (for the tests' aims: the ground 40 from the player every 5 degrees of bearing, and each monster's body
             * as the pick sees it: its feet and top on the picture and its half-width) */
            const Actor *pl = &player_state.actor;
            printf("P1_AIM_COMPASS vi=%llu", vi());
            for (int b = 0; b < 360; b += 5) {
                const float th = b * PI_F / 180.0f;
                if (project(pl->x + sinf(th) * 40.0f, pl->y, pl->z + cosf(th) * 40.0f, &sx, &sy)) printf(" b%d=%.2f,%.2f", b, sx, sy);
            }
            for (int i = 0; i < MAX_ENEMIES; i++) {
                float fx, fy, tx, ty, half;
                if (alive(i) && body_on_screen(enemy(i), &fx, &fy, &tx, &ty, &half))
                    printf(" body%d=%.2f,%.2f,%.2f,%.2f,%.2f", i, fx, fy, tx, ty, half);
            }
            printf("\n");
        }
    }

    /* a click: the aim kept for this read's cast, and A */
    if (A.active && (flags & PORT_POINTER_LEFT)) {
        A.pending = 1;
        A.pending_target = A.target;
        A.pending_x = A.gx;
        A.pending_z = A.gz;
        A.pending_spell = (u32)id;
        A.pending_vi = vi();
        Port_MousePointerTake(PORT_POINTER_LEFT);
        *buttons |= 0x8000;
        if (log_level()) {
            printf("P1_AIM_CLICK vi=%llu spell=%d.%d target=%d point=%.2f,%.2f yaw=%.3f predicted=%x\n", vi(), (int)(id >> 8) & 0xF,
                   (int)id & 0xFF, A.target, A.gx, A.gz, A.preview.yaw, A.preview.hits);
            log_enemies();
        }
    }
}

void Port_SpellAimApply(struct Player *p, struct Actor *a) {
    if (!A.pending) return;
    A.pending = 0;
    if (p->spell_id != A.pending_spell) return;
    const int kind = spell_kind(A.pending_spell);
    const float before = a->rot;
    if (directional(kind)) {
        float px = A.pending_x, pz = A.pending_z;
        if (A.pending_target >= 0 && alive(A.pending_target)) px = enemy(A.pending_target)->x, pz = enemy(A.pending_target)->z;
        if (px != a->x || pz != a->z) a->rot = Math_Atan2(px - a->x, pz - a->z);
    }
    A.session = 0;
    static Preview cast;
    simulate(A.pending_spell, a->rot, &cast, 1);
    A.track = 1;
    A.track_spell = A.pending_spell;
    A.track_predicted = cast.hits;
    A.track_actual = 0;
    A.track_slots = 0;
    A.track_ticks = 0;
    A.track_seen = 0;
    memset(A.track_seen_pairs, 0, sizeof A.track_seen_pairs);
    if (log_level()) {
        printf("P1_AIM_APPLY vi=%llu spell=%d.%d rot=%.3f>%.3f\n", vi(), (int)(A.pending_spell >> 8) & 0xF,
               (int)A.pending_spell & 0xFF, before, a->rot);
        log_preview("P1_AIM_CAST", &cast);
        if (log_level() >= 2) {
            /* what a preview costs (each read while aiming works one out) */
            static Preview t;
            const clock_t t0 = clock();
            for (int k = 0; k < 20; k++) simulate(A.pending_spell, a->rot, &t, 1);
            const clock_t t1 = clock();
            /* and a kept cone turned by a facing 0.7 off and back, against the cone built (the slope's difference) */
            simulate(A.pending_spell, a->rot + 0.7f, &t, 1);
            simulate(A.pending_spell, a->rot, &t, 0);
            float worst = 0.0f;
            for (int n = 0; n < cast.casts; n++)
                for (int i = 0; i < CONE_BINS; i++)
                    if (cast.cone[n].ok && cast.cone[n].rmax[i] >= 0.0f && t.cone[n].rmax[i] >= 0.0f) {
                        const float d = fabsf(cast.cone[n].rmax[i] - t.cone[n].rmax[i]);
                        if (d > worst) worst = d;
                    }
            const clock_t t2 = clock();
            for (int k = 0; k < 20; k++) simulate(A.pending_spell, a->rot + 0.01f * k, &t, 0);
            const clock_t t3 = clock();
            /* and the drawing's triangles for the preview the pointer had */
            for (int k = 0; k < 20; k++) build(&A.preview);
            printf("P1_AIM_COST spell=%d.%d built_us=%.0f turned_us=%.0f turned_off=%.2f draw_us=%.0f verts=%d\n",
                   (int)(A.pending_spell >> 8) & 0xF, (int)A.pending_spell & 0xFF, (double)(t1 - t0) * 1e6 / CLOCKS_PER_SEC / 20.0,
                   (double)(t3 - t2) * 1e6 / CLOCKS_PER_SEC / 20.0, worst, (double)(clock() - t3) * 1e6 / CLOCKS_PER_SEC / 20.0,
                   nverts);
        }
        log_shapes(&cast);
        log_enemies();
        if (log_level() >= 3)
            for (int n = 0; n < cast.casts; n++)
                for (int k = 0; k < cast.cast[n].path_n; k++)
                    printf("P1_AIM_SIMPATH cast=%d k=%d pos=%.2f,%.2f,%.2f\n", n, k, cast.cast[n].path[k][0], cast.cast[n].path[k][1],
                           cast.cast[n].path[k][2]);
    }
}

/* ---- drawing ------------------------------------------------------------------------------------------------------ */

enum { MAX_VERTS = 49152 };
static ElRdVertex verts[MAX_VERTS];

static void vert(float x, float y, float z, const u8 *c) {
    if (nverts >= MAX_VERTS) return;
    ElRdVertex *v = &verts[nverts++];
    memset(v, 0, sizeof *v);
    v->x = x, v->y = y, v->z = z;
    memcpy(v->c, c, 4);
    v->flags = EL_RDV_SHADE;
}

static void quad(const float *a, const float *b, const float *c, const float *d, const u8 *ca, const u8 *cb) {
    /* a b on one edge (colour ca), d c on the other (cb): a-b-c, a-c-d */
    vert(a[0], a[1], a[2], ca), vert(b[0], b[1], b[2], ca), vert(c[0], c[1], c[2], cb);
    vert(a[0], a[1], a[2], ca), vert(c[0], c[1], c[2], cb), vert(d[0], d[1], d[2], cb);
}

/* a point on the ground (the floor under it, else at `y`), a little above it */
static void ground(float x, float z, float y, float *p) {
    /* (from the kept ground near its height: a corner's height between the four nearest of its grid, GSTEP apart) */
    if (gcache_map != (const void *)map_subarea_grid) gcache_use(y);
    if (fabsf(y - gcache_ref) > 10.0f) {
        float fy;
        if (!floor_y(x, y + 40.0f, z, FLOOR_FLAGS, &fy) || fy < y - 60.0f) fy = y;
        p[0] = x, p[1] = fy + 0.45f, p[2] = z;
        return;
    }
    const float gx = x / GSTEP, gz = z / GSTEP, x0 = floorf(gx), z0 = floorf(gz), tx = gx - x0, tz = gz - z0;
    const float a = ground_cached(x0 * GSTEP, z0 * GSTEP), b = ground_cached((x0 + 1) * GSTEP, z0 * GSTEP);
    const float c = ground_cached(x0 * GSTEP, (z0 + 1) * GSTEP), d = ground_cached((x0 + 1) * GSTEP, (z0 + 1) * GSTEP);
    const float fy = (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
    p[0] = x, p[1] = fy + 0.45f, p[2] = z;
}

static void rgba(u8 *c, int r, int g, int b, int a) { c[0] = (u8)r, c[1] = (u8)g, c[2] = (u8)b, c[3] = (u8)(a < 0 ? 0 : a > 255 ? 255 : a); }

/* a band from radius r0 to r1 round (cx, cz), on the ground; colours inside and outside */
static void annulus(float cx, float cz, float y, float r0, float r1, const u8 *c0, const u8 *c1) {
    int segs = (int)(r1 * 1.2f) + 24;
    if (segs > 96) segs = 96;
    float pi[3], po[3], qi[3], qo[3];
    for (int k = 0; k < segs; k++) {
        const float a0 = 2.0f * PI_F * k / segs, a1 = 2.0f * PI_F * (k + 1) / segs;
        ground(cx + sinf(a0) * r0, cz + cosf(a0) * r0, y, pi);
        ground(cx + sinf(a0) * r1, cz + cosf(a0) * r1, y, po);
        ground(cx + sinf(a1) * r0, cz + cosf(a1) * r0, y, qi);
        ground(cx + sinf(a1) * r1, cz + cosf(a1) * r1, y, qo);
        quad(pi, qi, qo, po, c0, c1);
    }
}

static void disc(float cx, float cz, float y, float r, const u8 *fill, const u8 *edge, float edge_w) {
    if (fill[3]) {
        /* the fill: rings out from the middle (so it follows the ground) */
        const int rings = r > 12.0f ? 3 : 1;
        for (int i = 0; i < rings; i++) annulus(cx, cz, y, r * i / rings, r * (i + 1) / rings - (i == rings - 1 ? edge_w * 0.5f : 0.0f), fill, fill);
    }
    u8 out[4];
    memcpy(out, edge, 4);
    out[3] = (u8)(edge[3] / 3);
    annulus(cx, cz, y, r - edge_w * 0.5f, r + edge_w * 0.5f, edge, edge);
    annulus(cx, cz, y, r + edge_w * 0.5f, r + edge_w * 1.5f, edge, out);
}

/* a band along a path (x, z pairs), half-width w, on the ground */
static void band(const float (*path)[3], int n, float y, float w, const u8 *fill, const u8 *edge, float edge_w) {
    for (int k = 0; k + 1 < n; k++) {
        const float *a = path[k], *b = path[k + 1];
        float dx = b[0] - a[0], dz = b[2] - a[2], len = sqrtf(dx * dx + dz * dz);
        if (len < 0.05f) continue;
        dx /= len, dz /= len;
        const float nx = dz, nz = -dx; /* to the side */
        /* split long steps so the band follows the ground */
        const int parts = (int)(len / 6.0f) + 1;
        for (int s = 0; s < parts; s++) {
            const float t0 = (float)s / parts, t1 = (float)(s + 1) / parts;
            const float x0 = a[0] + (b[0] - a[0]) * t0, z0 = a[2] + (b[2] - a[2]) * t0;
            const float x1 = a[0] + (b[0] - a[0]) * t1, z1 = a[2] + (b[2] - a[2]) * t1;
            float p[8][3];
            const float off[4] = { -w - edge_w * 0.5f, -w + edge_w * 0.5f, w - edge_w * 0.5f, w + edge_w * 0.5f };
            for (int e = 0; e < 4; e++) {
                ground(x0 + nx * off[e], z0 + nz * off[e], y, p[e]);
                ground(x1 + nx * off[e], z1 + nz * off[e], y, p[4 + e]);
            }
            quad(p[0], p[4], p[5], p[1], edge, edge);
            quad(p[1], p[5], p[6], p[2], fill, fill);
            quad(p[2], p[6], p[7], p[3], edge, edge);
        }
    }
}

/* a thin line on the ground from a to b */
static void line(float ax, float az, float bx, float bz, float y, float w, const u8 *c0, const u8 *c1) {
    const float dx = bx - ax, dz = bz - az, len = sqrtf(dx * dx + dz * dz);
    if (len < 0.05f) return;
    const float nx = dz / len * w * 0.5f, nz = -dx / len * w * 0.5f;
    const int parts = (int)(len / 5.0f) + 1;
    for (int s = 0; s < parts; s++) {
        const float t0 = (float)s / parts, t1 = (float)(s + 1) / parts;
        u8 ca[4], cb[4];
        for (int k = 0; k < 4; k++) ca[k] = (u8)(c0[k] + (c1[k] - c0[k]) * t0), cb[k] = (u8)(c0[k] + (c1[k] - c0[k]) * t1);
        float p[4][3];
        ground(ax + dx * t0 - nx, az + dz * t0 - nz, y, p[0]);
        ground(ax + dx * t0 + nx, az + dz * t0 + nz, y, p[1]);
        ground(ax + dx * t1 + nx, az + dz * t1 + nz, y, p[2]);
        ground(ax + dx * t1 - nx, az + dz * t1 - nz, y, p[3]);
        /* (a quad whose ends have the colours along it) */
        vert(p[0][0], p[0][1], p[0][2], ca), vert(p[1][0], p[1][1], p[1][2], ca), vert(p[2][0], p[2][1], p[2][2], cb);
        vert(p[0][0], p[0][1], p[0][2], ca), vert(p[2][0], p[2][1], p[2][2], cb), vert(p[3][0], p[3][1], p[3][2], cb);
    }
}

/* ---- the tunnels, the arcs and the cones ------------------------------------------------------------------------- */

/* A tunnel: the ground a cast's hit test can reach, the union of a disc of radius w round each live place (Cast.w),
 * drawn as a band along the path whose half-width at each point is the widest of those discs there (so it narrows
 * where the effect is high and breaks where it can touch nothing), with round ends. A run of places that do not move
 * (a landed rock, Fire Bomb's blast) is its disc. */
#define TUNNEL_STEP 1.5f
typedef struct Sample { float x, z, nx, nz, h; } Sample;
static Sample samples[1024];

static void strip(const Sample *sm, int n, float y, const u8 *fill, const u8 *edge, float ew) {
    for (int k = 0; k + 1 < n; k++) {
        const Sample *a = &sm[k], *b = &sm[k + 1];
        if (a->h <= 0.0f || b->h <= 0.0f) continue;
        float p[8][3];
        const float ha = a->h, hb = b->h;
        const float oa[4] = { -ha - ew * 0.5f, -ha + ew * 0.5f, ha - ew * 0.5f, ha + ew * 0.5f };
        const float ob[4] = { -hb - ew * 0.5f, -hb + ew * 0.5f, hb - ew * 0.5f, hb + ew * 0.5f };
        for (int e = 0; e < 4; e++) {
            ground(a->x + a->nx * oa[e], a->z + a->nz * oa[e], y, p[e]);
            ground(b->x + b->nx * ob[e], b->z + b->nz * ob[e], y, p[4 + e]);
        }
        quad(p[0], p[4], p[5], p[1], edge, edge);
        quad(p[1], p[5], p[6], p[2], fill, fill);
        quad(p[2], p[6], p[7], p[3], edge, edge);
    }
}

/* The drawn half-width at (x, z) on the path. Along the path: where a monster of radius R beside it is touched, less R
 * (the discs of radius w + R round the live places: a monster's middle within one is hit), so a monster whose edge
 * reaches the band's side is hit. Past the ends (R 0): the end's own disc of radius w, so one met head on is hit when
 * its edge reaches the round end. */
static float run_h(const Cast *c, int k0, int k1, float x, float z, float R) {
    float h = 0.0f;
    for (int k = k0; k <= k1; k++) {
        const float dx = x - c->path[k][0], dz = z - c->path[k][2], w = c->w[k] + R, q = w * w - dx * dx - dz * dz;
        if (q > (h + R) * (h + R)) h = sqrtf(q) - R;
    }
    return h;
}

static void tunnel_run(const Cast *c, int k0, int k1, float y, float R, const u8 *fill, const u8 *edge) {
    /* the run's line: its places that move; none: a disc */
    float wmax = 0.0f;
    for (int k = k0; k <= k1; k++)
        if (c->w[k] > wmax) wmax = c->w[k];
    int moved = 0;
    for (int k = k0; k < k1; k++)
        if (fabsf(c->path[k + 1][0] - c->path[k][0]) + fabsf(c->path[k + 1][2] - c->path[k][2]) > 0.05f) moved = 1;
    if (!moved) {
        disc(c->path[k0][0], c->path[k0][2], y, wmax, fill, edge, 0.8f);
        return;
    }
    int n = 0;
    float px = c->path[k0][0], pz = c->path[k0][2], dx0 = 0, dz0 = 0;
    /* (the first direction it moves: the start's cap goes back along it) */
    for (int k = k0; k < k1; k++) {
        const float dx = c->path[k + 1][0] - c->path[k][0], dz = c->path[k + 1][2] - c->path[k][2], l = sqrtf(dx * dx + dz * dz);
        if (l > 0.05f) {
            dx0 = dx / l, dz0 = dz / l;
            break;
        }
    }
    const int cap = (int)(wmax / (TUNNEL_STEP * 0.5f)) + 2;
    for (int s = cap; s > 0 && n < 1024; s--) {
        const float t = wmax * s / cap, x = px - dx0 * t, z = pz - dz0 * t;
        samples[n++] = (Sample){ x, z, dz0, -dx0, run_h(c, k0, k1, x, z, 0.0f) };
    }
    float dx = dx0, dz = dz0;
    for (int k = k0; k <= k1 && n < 1024; k++) {
        if (k < k1) {
            const float ex = c->path[k + 1][0] - c->path[k][0], ez = c->path[k + 1][2] - c->path[k][2], l = sqrtf(ex * ex + ez * ez);
            if (l <= 0.05f) continue;
            dx = ex / l, dz = ez / l;
            const int parts = (int)(l / TUNNEL_STEP) + 1;
            for (int s = 0; s < parts && n < 1024; s++) {
                const float x = c->path[k][0] + ex * s / parts, z = c->path[k][2] + ez * s / parts;
                samples[n++] = (Sample){ x, z, dz, -dx, run_h(c, k0, k1, x, z, R) };
            }
        } else {
            px = c->path[k][0], pz = c->path[k][2];
            samples[n++] = (Sample){ px, pz, dz, -dx, run_h(c, k0, k1, px, pz, R) };
        }
    }
    for (int s = 1; s <= cap && n < 1024; s++) {
        const float t = wmax * s / cap, x = px + dx * t, z = pz + dz * t;
        samples[n++] = (Sample){ x, z, dz, -dx, run_h(c, k0, k1, x, z, 0.0f) };
    }
    /* (the normals at a turn: halfway between the two ways) */
    for (int k = 1; k + 1 < n; k++) {
        float nx = samples[k - 1].nx + samples[k + 1].nx, nz = samples[k - 1].nz + samples[k + 1].nz, l = sqrtf(nx * nx + nz * nz);
        if (l > 0.1f && (samples[k].nx != samples[k + 1].nx || samples[k].nz != samples[k + 1].nz)) samples[k].nx = nx / l, samples[k].nz = nz / l;
    }
    strip(samples, n, y, fill, edge, 0.7f);
}

/* the drawn places (live, and their disc reaches past the place itself) */
static int drawn(const Cast *c, int k) { return k > 0 && k < c->path_n && c->live[k] && c->w[k] > 0.0f; }

static void tunnel(const Cast *c, float y, float R, const u8 *fill, const u8 *edge) {
    for (int k = 1; k < c->path_n; k++) {
        if (!drawn(c, k)) continue;
        int e = k;
        while (drawn(c, e + 1)) e++;
        tunnel_run(c, k, e, y, R, fill, edge);
        k = e;
    }
}

/* a thin line in the air along the path from place k0 to k1 (as it flies: where it is too high to touch anything) */
static void arc(const Cast *c, int k0, int k1, const u8 *col) {
    const float w = 0.35f;
    for (int k = k0; k < k1 && k + 1 < c->path_n; k++) {
        const float *a = c->path[k], *b = c->path[k + 1];
        float dx = b[0] - a[0], dz = b[2] - a[2];
        const float l = sqrtf(dx * dx + dz * dz);
        if (l < 0.05f) continue;
        dx /= l, dz /= l;
        const float nx = dz * w, nz = -dx * w;
        const float p0[3] = { a[0] - nx, a[1], a[2] - nz }, p1[3] = { a[0] + nx, a[1], a[2] + nz };
        const float p2[3] = { b[0] + nx, b[1], b[2] + nz }, p3[3] = { b[0] - nx, b[1], b[2] - nz };
        quad(p0, p1, p2, p3, col, col);
    }
}

/* the arc over each stretch the tunnel does not show (all of a lob's flight: whole) */
static void arcs(const Cast *c, int whole, const u8 *col) {
    for (int k = 0; k + 1 < c->path_n; k++)
        if (whole || !drawn(c, k) || !drawn(c, k + 1)) arc(c, k, k + 1, col);
}

/* a cone: the covered ground round the bolt's first place, bearing by bearing (every other bin), and its outline */
static void cone_draw(const Cone *cn, float y, const u8 *fill, const u8 *edge) {
    const float step = 2.0f * PI_F / CONE_BINS;
    const int stride = 2;
    for (int i = 0; i < CONE_BINS; i += stride) {
        const int j = (i + stride) % CONE_BINS;
        if (cn->rmax[i] < 0.0f || cn->rmax[j] < 0.0f) continue;
        const float ti = i * step, tj = j * step;
        const float si = sinf(ti), ci = cosf(ti), sj = sinf(tj), cj = cosf(tj);
        const float span = fmaxf(cn->rmax[i] - cn->rmin[i], cn->rmax[j] - cn->rmin[j]);
        const int parts = (int)(span / 8.0f) + 1;
        float a0[3], b0[3];
        ground(cn->sx + si * cn->rmin[i], cn->sz + ci * cn->rmin[i], y, a0);
        ground(cn->sx + sj * cn->rmin[j], cn->sz + cj * cn->rmin[j], y, b0);
        for (int s = 1; s <= parts; s++) {
            const float ri = cn->rmin[i] + (cn->rmax[i] - cn->rmin[i]) * s / parts;
            const float rj = cn->rmin[j] + (cn->rmax[j] - cn->rmin[j]) * s / parts;
            float a1[3], b1[3];
            ground(cn->sx + si * ri, cn->sz + ci * ri, y, a1);
            ground(cn->sx + sj * rj, cn->sz + cj * rj, y, b1);
            quad(a0, b0, b1, a1, fill, fill);
            memcpy(a0, a1, sizeof a0);
            memcpy(b0, b1, sizeof b0);
        }
        /* the outline: the far edge, the near one when it is off the start, and the sides where it ends */
        line(cn->sx + si * cn->rmax[i], cn->sz + ci * cn->rmax[i], cn->sx + sj * cn->rmax[j], cn->sz + cj * cn->rmax[j], y, 0.6f, edge, edge);
        if (cn->rmin[i] > 0.5f || cn->rmin[j] > 0.5f)
            line(cn->sx + si * cn->rmin[i], cn->sz + ci * cn->rmin[i], cn->sx + sj * cn->rmin[j], cn->sz + cj * cn->rmin[j], y, 0.6f, edge, edge);
        const int pi = (i - stride + CONE_BINS) % CONE_BINS, nj = (j + stride) % CONE_BINS;
        if (cn->rmax[pi] < 0.0f)
            line(cn->sx + si * cn->rmin[i], cn->sz + ci * cn->rmin[i], cn->sx + si * cn->rmax[i], cn->sz + ci * cn->rmax[i], y, 0.6f, edge, edge);
        if (cn->rmax[nj] < 0.0f)
            line(cn->sx + sj * cn->rmin[j], cn->sz + cj * cn->rmin[j], cn->sx + sj * cn->rmax[j], cn->sz + cj * cn->rmax[j], y, 0.6f, edge, edge);
    }
}

/* the facing: a chevron on the ground just past the player's ring */
static void facing_arrow(float yaw) {
    const Actor *a = &player_state.actor;
    const float r = radius_of(a), s = sinf(yaw), c = cosf(yaw), sx = c, sz = -s; /* forward (s, c), side (c, -s) */
    u8 col[4];
    rgba(col, 255, 255, 255, 200);
    const float d0 = r + 2.0f, d1 = r + 6.0f, half = 3.0f;
    const float tip[2] = { a->x + s * d1, a->z + c * d1 };
    const float l[2] = { a->x + s * d0 - sx * half, a->z + c * d0 - sz * half };
    const float rr[2] = { a->x + s * d0 + sx * half, a->z + c * d0 + sz * half };
    line(l[0], l[1], tip[0], tip[1], a->y, 1.0f, col, col);
    line(rr[0], rr[1], tip[0], tip[1], a->y, 1.0f, col, col);
}

/* the glow under a monster the spell would hit: a bright ring at its feet (one more for each further hit) */
static void glow(int i, int count) {
    const Actor *a = enemy(i);
    const float r = radius_of(a) + 1.2f;
    const float pulse = 0.5f + 0.5f * sinf((float)A.tick * 0.35f);
    u8 fill[4], edge[4];
    rgba(fill, 255, 196, 64, 60 + (int)(40 * pulse));
    rgba(edge, 255, 222, 110, 200 + (int)(55 * pulse));
    float fy = a->y;
    if (a->stats->unk0 == 1) floor_y(a->x, middle_y(a), a->z, FLOOR_FLAGS, &fy);
    disc(a->x, a->z, fy, r, fill, edge, 0.9f);
    for (int k = 1; k < count && k < 5; k++) {
        u8 e2[4];
        rgba(e2, 255, 222, 110, 170);
        annulus(a->x, a->z, fy, r + 1.0f + 1.4f * k, r + 1.7f + 1.4f * k, e2, e2);
    }
}

/* the preview's triangles (verts, nverts) */
static void build(const Preview *p) {
    nverts = 0;
    const int any = p->hits != 0;
    u8 fill[4], edge[4];
    /* an area that would hit: the game's water-blue; one that would not: grey */
    if (any) rgba(fill, 120, 200, 255, 52), rgba(edge, 170, 225, 255, 210);
    else rgba(fill, 210, 210, 210, 34), rgba(edge, 230, 230, 230, 150);
    switch (p->kind) {
    case K_AROUND:
        disc(p->px, p->pz, p->py, spell_def(p->spell)->effect_extent, fill, edge, 0.9f);
        break;
    case K_RANDOM: {
        /* Rock Combo: rocks fall 20..39 from the player (Math_Rand: where, not previewed) */
        u8 f2[4];
        rgba(f2, 170, 225, 255, 38);
        annulus(p->px, p->pz, p->py, 20.0f, 39.0f, f2, f2);
        annulus(p->px, p->pz, p->py, 19.5f, 20.3f, edge, edge);
        annulus(p->px, p->pz, p->py, 38.7f, 39.5f, edge, edge);
        break;
    }
    case K_POINT:
        for (int n = 0; n < p->casts; n++)
            if (p->cast[n].made)
                disc(p->cast[n].path[0][0], p->cast[n].path[0][2], p->py, spell_def(p->spell)->effect_extent, fill, edge, 0.9f);
        break;
    case K_LINE:
    case K_LOB:
    case K_FAN:
    case K_HOMING: {
        /* each cast's tunnel (blue where it touches a monster, grey where it touches none), the thin arc where it flies
         * out of reach (a lob's whole flight), and a homing bolt's cone round it */
        const int many = p->casts > 1;
        /* Several bolts (Fire Ball Lv2, Lv3): they all start at one place and can each turn right round, so their cones
         * are nearly one lopsided disc: drawn once, their union (each bolt's is in the log) */
        static Cone all;
        int cones = 0, cone_hits = 0;
        for (int n = 0; n < p->casts; n++) {
            const Cone *cn = &p->cone[n];
            if (!cn->ok || !p->cast[n].made) continue;
            if (!cones++) {
                all = *cn;
            } else {
                for (int i = 0; i < CONE_BINS; i++) {
                    if (cn->rmax[i] < 0.0f) continue;
                    if (cn->rmin[i] < all.rmin[i]) all.rmin[i] = cn->rmin[i];
                    if (cn->rmax[i] > all.rmax[i]) all.rmax[i] = cn->rmax[i];
                }
            }
            cone_hits |= p->cast[n].hits != 0;
        }
        if (cones) {
            u8 cf[4], ce[4];
            if (cone_hits) rgba(cf, 120, 200, 255, 20), rgba(ce, 170, 225, 255, 110);
            else rgba(cf, 210, 210, 210, 16), rgba(ce, 230, 230, 230, 90);
            cone_draw(&all, p->py, cf, ce);
        }
        for (int n = 0; n < p->casts; n++) {
            const Cast *c = &p->cast[n];
            if (!c->made) continue;
            u8 tf[4], te[4], arc_col[4];
            if (c->hits) rgba(tf, 120, 200, 255, many ? 40 : 52), rgba(te, 170, 225, 255, 210);
            else rgba(tf, 210, 210, 210, many ? 28 : 34), rgba(te, 230, 230, 230, 150);
            rgba(arc_col, 255, 244, 214, 110);
            tunnel(c, p->py, p->body_r, tf, te);
            arcs(c, p->kind == K_LOB, arc_col);
        }
        break;
    }
    default:
        break; /* targets: the rings say it */
    }
    /* the rings: every monster it would hit (all of them for the whole arena) */
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (alive(i) && (p->hits & (1u << i))) glow(i, p->kind == K_HOMING || p->kind == K_FAN ? p->hit_count[i] : 1);
    if (A.active && directional(p->kind)) facing_arrow(p->yaw);
}

void Port_SpellAimDraw(void) {
    if (!Port_SpellAimEnabled() || !ND2D_TRACK()) return;
    const s32 id = ready_spell();
    const Preview *p = &A.preview;
    if (id < 0 || !p->valid || p->spell != (u32)id) return;
    build(p);
    if (nverts) nd_2d_world_tris(verts, (uint32_t)nverts);
}
