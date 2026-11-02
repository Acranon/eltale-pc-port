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
 *   render.h v10): areas on the ground, lines, a fan, landing circles, a ring under each monster that would be hit.
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

#define PI_F 3.14159265f
#define MAX_ENEMIES 6
#define MAX_CASTS 12  /* cast_count up to 6, doubled by the 50/50/50/50 bonus */
#define MAX_PATH 80   /* a moving effect's life (50, 1.5x with the bonus) and its delay */
#define FLOOR_FLAGS 0xA00 /* the spells' floor query (Magic_MoveHomingEffect) */

/* ---- the switch ------------------------------------------------------------------------------------------------ */

static int host_on = -1; /* the host's bits (2 aim, 4 camera), -1: none (headless) */

int Port_SpellAimEnabled(void) {
    static int env = -2;
    if (host_on >= 0) return (host_on & 2) != 0;
    if (env == -2) env = getenv("ELTALE_SPELL_AIM") ? atoi(getenv("ELTALE_SPELL_AIM")) != 0 : 0;
    return env;
}

static int camera_enabled(void) {
    static int env = -2;
    if (host_on >= 0) return (host_on & 4) != 0;
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
    K_LINE,     /* a straight mover (homing 0, no landing): a band along its path */
    K_LOB,      /* a mover that lands (floor 1): its landing circle */
    K_FAN,      /* short homing bolts from ahead (place 3, homing): a fan to their reach */
    K_HOMING,   /* homing movers: the rings of their targets */
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
    int landed;
    float land[3];
    float reach;             /* the length of its path */
    u16 hits;
} Cast;

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

static void simulate(u32 id, float yaw, Preview *out) {
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
                c->land[0] = p.x, c->land[1] = p.y, c->land[2] = p.z;
            }
            c->reach += sqrtf((p.x - ox) * (p.x - ox) + (p.z - oz) * (p.z - oz));
            if (c->path_n < MAX_PATH) {
                c->path[c->path_n][0] = p.x, c->path[c->path_n][1] = p.y, c->path[c->path_n][2] = p.z;
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
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (p.hit_bits & (1u << i)) out->hit_count[i]++;
    }
}

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

/* the monster under the pointer (CENTRE units): its body from its feet to its top, its radius wide, a few units round */
static int enemy_at(float sx, float sy) {
    int best = -1;
    float best_d = 1e9f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!alive(i)) continue;
        const Actor *a = enemy(i);
        const float r = radius_of(a), mid = middle_y(a);
        const float feet = a->stats->unk0 == 1 ? mid - r : a->y;
        const float top = a->stats->unk0 == 1 ? mid + r : a->y + a->stats->body_height * a->scale;
        float fx, fy, tx, ty, mx, my, rx, ry;
        if (!project(a->x, feet, a->z, &fx, &fy) || !project(a->x, top, a->z, &tx, &ty) ||
            !project(a->x, mid, a->z, &mx, &my) ||
            !project(a->x + view.right[0] * r, mid + view.right[1] * r, a->z + view.right[2] * r, &rx, &ry))
            continue;
        const float rpx = sqrtf((rx - mx) * (rx - mx) + (ry - my) * (ry - my)) + 3.0f;
        /* the distance to the segment feet..top */
        const float ex = tx - fx, ey = ty - fy, len2 = ex * ex + ey * ey;
        float t = len2 > 1e-6f ? ((sx - fx) * ex + (sy - fy) * ey) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        const float qx = fx + ex * t - sx, qy = fy + ey * t - sy, d = sqrtf(qx * qx + qy * qy);
        if (d <= rpx && d - rpx < best_d) best_d = d - rpx, best = i;
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
            if (enemy_slots[i].battle.magic_hit_slot_bits & magic_slot_bits[s]) A.track_actual |= (u16)(1u << i);
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
    if (A.active) {
        A.target = enemy_at(x, y);
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
    simulate((u32)id, yaw, &A.preview);

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
    Preview cast;
    simulate(A.pending_spell, a->rot, &cast);
    A.track = 1;
    A.track_spell = A.pending_spell;
    A.track_predicted = cast.hits;
    A.track_actual = 0;
    A.track_slots = 0;
    A.track_ticks = 0;
    A.track_seen = 0;
    if (log_level()) {
        printf("P1_AIM_APPLY vi=%llu spell=%d.%d rot=%.3f>%.3f\n", vi(), (int)(A.pending_spell >> 8) & 0xF,
               (int)A.pending_spell & 0xFF, before, a->rot);
        log_preview("P1_AIM_CAST", &cast);
        log_enemies();
        if (log_level() >= 3)
            for (int n = 0; n < cast.casts; n++)
                for (int k = 0; k < cast.cast[n].path_n; k++)
                    printf("P1_AIM_SIMPATH cast=%d k=%d pos=%.2f,%.2f,%.2f\n", n, k, cast.cast[n].path[k][0], cast.cast[n].path[k][1],
                           cast.cast[n].path[k][2]);
    }
}

/* ---- drawing ------------------------------------------------------------------------------------------------------ */

enum { MAX_VERTS = 12288 };
static ElRdVertex verts[MAX_VERTS];
static int nverts;

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
    float fy;
    if (!floor_y(x, y + 40.0f, z, FLOOR_FLAGS, &fy) || fy < y - 60.0f) fy = y;
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

void Port_SpellAimDraw(void) {
    if (!Port_SpellAimEnabled() || !ND2D_TRACK()) return;
    const s32 id = ready_spell();
    const Preview *p = &A.preview;
    if (id < 0 || !p->valid || p->spell != (u32)id) return;
    nverts = 0;
    const Actor *pl = &player_state.actor;
    const int any = p->hits != 0;
    u8 fill[4], edge[4], faint[4], none[4];
    /* an area that would hit: the game's water-blue; one that would not: grey */
    if (any) rgba(fill, 120, 200, 255, 52), rgba(edge, 170, 225, 255, 210);
    else rgba(fill, 210, 210, 210, 34), rgba(edge, 230, 230, 230, 150);
    rgba(faint, 255, 244, 214, 90);
    rgba(none, 255, 244, 214, 0);
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
        for (int n = 0; n < p->casts; n++) {
            const Cast *c = &p->cast[n];
            if (c->made) band((const float (*)[3])c->path, c->path_n, p->py, spell_def(p->spell)->effect_extent, fill, edge, 0.8f);
        }
        break;
    case K_LOB:
        for (int n = 0; n < p->casts; n++) {
            const Cast *c = &p->cast[n];
            if (!c->made) continue;
            const float *e = c->landed ? c->land : c->path[c->path_n - 1];
            line(p->px, p->pz, e[0], e[2], p->py, 0.8f, none, faint);
            disc(e[0], e[2], p->py, spell_def(p->spell)->effect_extent, fill, edge, 0.9f);
        }
        break;
    case K_FAN:
        /* each bolt's heading (0, then 45 to each side) to its reach: how far it flies in its life */
        for (int n = 0; n < p->casts; n++) {
            const Cast *c = &p->cast[n];
            if (!c->made) continue;
            const Vec3f *dir = &D_800C1878[(spell_def(p->spell)->launch_direction_index + n) & 7];
            Vec2f v = { dir->x, dir->z };
            Math_RotateVec2(p->yaw, &v);
            const float len = sqrtf(v.x * v.x + v.y * v.y);
            if (len < 1e-4f) continue;
            const float sx = c->path[0][0], sz = c->path[0][2];
            line(sx, sz, sx + v.x / len * c->reach, sz + v.y / len * c->reach, p->py, 1.0f, faint, none);
        }
        break;
    default:
        break; /* targets and homing: the rings say it */
    }
    /* the rings: every monster it would hit (all of them for the whole arena) */
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (alive(i) && (p->hits & (1u << i))) glow(i, p->kind == K_HOMING || p->kind == K_FAN ? p->hit_count[i] : 1);
    if (A.active && directional(p->kind)) facing_arrow(p->yaw);
    (void)pl;
    if (nverts) nd_2d_world_tris(verts, (uint32_t)nverts);
}
