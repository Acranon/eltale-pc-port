/* nd_automap.c: the area map's feed (port/include/port/automap.h, docs/AUTOMAP.md). PC-only.
 *
 * Read-only: at the end of each tick (the graphics task's start, after nd_frame_end) it reads what the game
 * keeps for the sub-area the player is in and hands it to the host, which draws the map:
 * - the plan, once per sub-area: the floor triangles the player's own floor query can land on (the collision
 *   meshes of the map objects in the sub-area's grid, CollTri flags 0x200/0x800 as the player's 0xA01 query
 *   takes them, in world space by Coll_TestObjectFloor's transform), the wall segments the player is pushed out
 *   of (map_actor_walls), and the exits (MapExit);
 * - the player, every tick (position and facing), numbered as the native renderer's recordings are, so the host
 *   draws the arrow where the pictures between ticks show the player.
 * Nothing here writes game state: the checks' hashes are the same with it on.
 *
 * The same file has the map's truth view (a test): ELTALE_AUTOMAP_TRUTH=1 shows the sub-area's real geometry from
 * straight above (the camera the renderer gets is replaced by a nearly orthographic one over the plan's bounds,
 * every map object of the sub-area is drawn, no fog, sky or 2D), so the map can be laid over the picture.
 * ELTALE_AUTOMAP_SHOTS=1 (with ELTALE_PRESENT_DUMP, in a hidden window) turns it on for a stretch of every
 * sub-area visited and asks the host for its pictures (port/host/automap.cpp). */
#include "game/native_draw/nd_internal.h"
#include "port/automap.h"

#include "game/core/main.h"
#include "game/battle/battle.h"
#include "game/map/map.h"
#include "game/player/player.h"
#include "game/map/chest.h"
#include "game/map/spirit.h"
#include "game/map/textspot.h"
#include "game/npc/npc.h"
#include "game/enemy/boss.h"
#include "game/item/item.h"
#include "port/data.h"
#include "port/text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*send_area)(const ElAmArea *);
static void (*send_player)(const ElAmPlayer *);

void nd_automap_connect(void *area_fn, void *player_fn) {
    send_area = (void (*)(const ElAmArea *))area_fn;
    send_player = (void (*)(const ElAmPlayer *))player_fn;
}

/* ---- the plan ------------------------------------------------------------------------------------------ */

static ElAmTri *tris;
static ElAmWall *walls;
static ElAmExit *exits;
static ElAmTri *water;
static uint32_t water_count, water_cap;
static uint32_t tri_count, tri_cap, wall_count, wall_cap, exit_count, exit_cap;
static ElAmArea area;
static uint32_t serial;
static int have_plan;
static s32 plan_map = -1, plan_sub = -1;
static const void *plan_grid, *plan_objs;
static uint32_t plan_objs_hash;
static uint32_t ticks_on_plan;

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t size) {
    if (need <= *cap) return p;
    uint32_t n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    void *q = realloc(p, (size_t)n * size);
    if (!q) return p;
    *cap = n;
    return q;
}

/* The objects the floor query can find: every cell's list and the list tested everywhere (bits by index) */
static u8 obj_seen[0x10000 / 8];

static void mark_list(const u16 *list) {
    for (u32 n = *list++; n; n--, list++) obj_seen[*list >> 3] |= (u8)(1 << (*list & 7));
}

static int collidable(const MapObj *o) {
    const MapModel *m = &map_obj_models[o->model];
    return (o->kind & 0xFF) < 0x10 && m->tris && m->ntris;
}

/* The collidable objects' places (a wave object moves its own: then the plan is made again) */
static uint32_t objs_hash(const MapGrid *g) {
    uint32_t h = 2166136261u;
    for (u32 i = 0; i < g->count; i++) {
        if (!(obj_seen[i >> 3] & (1 << (i & 7))) || !collidable(&map_objs[i])) continue;
        const MapObj *o = &map_objs[i];
        const float v[4] = { o->x, o->y, o->z, o->angle };
        const u8 *b = (const u8 *)v;
        for (size_t k = 0; k < sizeof v; k++) h = (h ^ b[k]) * 16777619u;
    }
    return h;
}

static void build_plan(void) {
    const MapGrid *g = map_subarea_grid;
    memset(obj_seen, 0, sizeof obj_seen);
    if (g->searchPattern == 0) {
        mark_list(g->lists);
    } else {
        for (s32 c = 0; c < g->w * g->h; c++) mark_list(&g->lists[g->cells[c]]);
    }
    {
        const u16 *list = g->alwaysList;
        for (u32 n = g->alwaysCount; n; n--, list++) obj_seen[*list >> 3] |= (u8)(1 << (*list & 7));
    }
    float bx0 = 1e30f, bz0 = 1e30f, bx1 = -1e30f, bz1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    tri_count = 0;
    for (u32 i = 0; i < g->count && i < 0x10000; i++) {
        if (!(obj_seen[i >> 3] & (1 << (i & 7)))) continue;
        const MapObj *o = &map_objs[i];
        if (!collidable(o)) continue;
        const MapModel *m = &map_obj_models[o->model];
        /* Coll_TestObjectFloor: local = R(world - o) with lx = x c - z s, lz = x s + z c (Math_RotateVec2, then
         * x negated); so world = o + R^T local. y = local y * scale + o->y. */
        const f32 ang = o->angle * 0.017453292519943295;
        const float c = ang != 0.0f ? cosf(ang) : 1.0f, s = ang != 0.0f ? sinf(ang) : 0.0f, k = o->scale;
        const CollTri *t = m->tris;
        for (u32 n = 0; n < m->ntris; n++, t++) {
            if (!(t->flags & 0xA00)) continue;
            tris = grow(tris, &tri_cap, tri_count + 1, sizeof *tris);
            if (tri_count >= tri_cap) break;
            ElAmTri *e = &tris[tri_count++];
            const s16 idx[3] = { t->v0, t->v1, t->v2 };
            for (int j = 0; j < 3; j++) {
                const CollVert *v = (const CollVert *)((const u8 *)m->verts + idx[j] * 8);
                const float lx = v->x * k, lz = v->z * k;
                e->v[j][0] = o->x + lx * c + lz * s;
                e->v[j][1] = o->y + v->y * k;
                e->v[j][2] = o->z - lx * s + lz * c;
                if (e->v[j][0] < bx0) bx0 = e->v[j][0];
                if (e->v[j][0] > bx1) bx1 = e->v[j][0];
                if (e->v[j][2] < bz0) bz0 = e->v[j][2];
                if (e->v[j][2] > bz1) bz1 = e->v[j][2];
                if (e->v[j][1] < y0) y0 = e->v[j][1];
                if (e->v[j][1] > y1) y1 = e->v[j][1];
            }
            e->ny = t->ny;
            e->flags = t->flags;
            e->obj = (uint16_t)i;
        }
    }
    wall_count = 0;
    if (map_actor_walls) {
        const CollWalls *w = &map_actor_walls[map_subarea];
        for (s32 r = 0; r < w->count; r++) {
            const CollWallRange *range = &w->ranges[r];
            const s32 *idx = &w->idx[range->start];
            for (s32 n = 0; n < range->count; n++, idx++) {
                walls = grow(walls, &wall_cap, wall_count + 1, sizeof *walls);
                if (wall_count >= wall_cap) break;
                ElAmWall *e = &walls[wall_count++];
                e->x0 = w->verts[idx[0]].x;
                e->z0 = w->verts[idx[0]].y;
                e->x1 = w->verts[idx[1]].x;
                e->z1 = w->verts[idx[1]].y;
                e->run = (uint16_t)r;
                e->pad = 0;
                const float xs[2] = { e->x0, e->x1 }, zs[2] = { e->z0, e->z1 };
                for (int j = 0; j < 2; j++) {
                    if (xs[j] < bx0) bx0 = xs[j];
                    if (xs[j] > bx1) bx1 = xs[j];
                    if (zs[j] < bz0) bz0 = zs[j];
                    if (zs[j] > bz1) bz1 = zs[j];
                }
            }
        }
    }
    /* water: the meshes the renderer draws for the wave, translucent and scrolling-texture objects (guPosition: a
     * turn about y by the angle, the scale, the place; the same transform as the floor's) */
    water_count = 0;
    for (u32 i = 0; nd_kinds && i < g->count && i < 0x10000; i++) {
        const MapObj *o = &map_objs[i];
        const u16 kind = (o->kind & 0x2000) ? 0 : (u8)o->kind;
        if (!((kind >= 1 && kind <= 3) || kind == 0x7F || kind == 0x80 || (kind >= 0x40 && kind < 0x45))) continue;
        uint32_t n = 0;
        const float *pos = nd_model_positions(nd_map_object_mesh(o), &n);
        if (!pos) continue;
        const f32 ang = o->angle * 0.017453292519943295;
        const float c = cosf(ang), s = sinf(ang), k = o->scale;
        for (uint32_t t = 0; t + 2 < n; t += 3) {
            water = grow(water, &water_cap, water_count + 1, sizeof *water);
            if (water_count >= water_cap) break;
            ElAmTri *e = &water[water_count++];
            for (int j = 0; j < 3; j++) {
                const float *v = &pos[(t + (uint32_t)j) * 3];
                const float lx = v[0] * k, lz = v[2] * k;
                e->v[j][0] = o->x + lx * c + lz * s;
                e->v[j][1] = o->y + v[1] * k;
                e->v[j][2] = o->z - lx * s + lz * c;
            }
            e->ny = 1.0f;
            e->flags = kind;
            e->obj = (uint16_t)i;
        }
    }
    exit_count = 0;
    for (s32 i = 0; i < map_subarea_def->exitCount; i++) {
        const MapExit *x = &map_subarea_def->exits[i];
        exits = grow(exits, &exit_cap, exit_count + 1, sizeof *exits);
        if (exit_count >= exit_cap) break;
        ElAmExit *e = &exits[exit_count++];
        e->x = x->x; e->z = x->z; e->angle = x->angle; e->half_w = x->halfW; e->half_d = x->halfD;
        e->flags = x->flags; e->item = x->id;
        e->to_map = (int16_t)x->dest[0]; e->to_sub = (int16_t)x->dest[1]; e->to_entry = (int16_t)x->dest[2];
        e->pad = 0;
    }
    if (bx0 > bx1) bx0 = bx1 = bz0 = bz1 = 0.0f;
    if (y0 > y1) y0 = y1 = 0.0f;
    memset(&area, 0, sizeof area);
    area.version = EL_AM_VERSION;
    area.serial = ++serial;
    area.map = map_id;
    area.sub = map_subarea;
    area.flags = (map_env_flags & 4) ? EL_AM_AREA_SKY : 0;
    area.bounds[0] = bx0; area.bounds[1] = bz0; area.bounds[2] = bx1; area.bounds[3] = bz1;
    area.y_min = y0; area.y_max = y1;
    area.tris = tris; area.tri_count = tri_count;
    area.walls = walls; area.wall_count = wall_count;
    area.exits = exits; area.exit_count = exit_count;
    area.water = water; area.water_count = water_count;
    plan_objs_hash = objs_hash(g);

    static int log = -1;
    if (log < 0) log = getenv("ELTALE_AUTOMAP_LOG") != NULL;
    if (log) {
        /* the floor's flags: how many triangles of each (actor/camera bits, effect) */
        u32 fx[8] = { 0 }, steep = 0, only800 = 0, objs = 0;
        for (u32 i = 0; i < tri_count; i++) {
            fx[(tris[i].flags >> 6) & 7]++;
            steep += tris[i].ny < 0.5f;
            only800 += !(tris[i].flags & 0x200);
        }
        for (u32 i = 0; i < g->count; i++) objs += (obj_seen[i >> 3] >> (i & 7)) & 1;
        char kinds[512] = "";
        {
            u32 hist[256] = { 0 };
            for (u32 i = 0; i < g->count; i++) hist[(map_objs[i].kind & 0x2000) ? 0xFF : (u8)map_objs[i].kind]++;
            size_t at = 0;
            for (int k = 0; k < 256 && at < sizeof kinds - 16; k++)
                if (hist[k]) at += (size_t)snprintf(kinds + at, sizeof kinds - at, "%x:%u,", k, hist[k]);
        }
        fprintf(stderr, "P1_AUTOMAP kinds map=%d sub=%d %s\n", (int)map_id, (int)map_subarea, kinds);
        fprintf(stderr, "P1_AUTOMAP plan map=%d sub=%d serial=%u objs=%u/%u tris=%u walls=%u exits=%u sky=%d bounds=%.0f,%.0f..%.0f,%.0f "
                "y=%.0f..%.0f effects=%u/%u/%u/%u steep=%u actor_only=%u pattern=%u\n",
                (int)map_id, (int)map_subarea, area.serial, objs, (unsigned)g->count, tri_count, wall_count, exit_count,
                !!(area.flags & EL_AM_AREA_SKY), bx0, bz0, bx1, bz1, y0, y1, fx[0], fx[1], fx[2], fx[3] + fx[4] + fx[5] + fx[6] + fx[7],
                steep, only800, (unsigned)g->searchPattern);
    }
}

/* ---- the marks, every tick ---------------------------------------------------------------------------- */

/* the game's text tables at fixed RDRAM addresses (big-endian pointers): the item names, the bosses' names */
extern void *D_803AB288[];
extern u8 *D_803AC418[];
extern f32 D_803AB48C[]; /* per boss + 1: how near starts the fight (swapped to native: port/data/common.c) */

static ElAmMarker marks[160];
static uint32_t mark_count;

/* A Japanese string's English (text/en.tsv), its first line, after `prefix`; `fallback` when there is none */
static void label_of(char *out, size_t size, const char *prefix, const u8 *jp, const char *fallback) {
    const char *en = jp ? Port_TextPlain(jp) : NULL;
    const char *name = en && *en ? en : fallback;
    size_t at = (size_t)snprintf(out, size, "%s", prefix);
    if (at >= size) at = size - 1;
    for (const char *c = name; *c && *c != '\n' && *c != '\f' && at + 1 < size; c++)
        if ((unsigned char)*c >= 0x20) out[at++] = *c;
    out[at] = 0;
}

static const u8 *item_name(u32 item) { return item < 0x40 ? (const u8 *)Port_N64Ptr(D_803AB288, (int)item) : NULL; }

static ElAmMarker *new_mark(u16 kind, u32 index) {
    if (mark_count >= sizeof marks / sizeof marks[0]) return NULL;
    ElAmMarker *m = &marks[mark_count++];
    memset(m, 0, sizeof *m);
    m->id = (u32)kind << 24 | (index & 0xFFFFFF);
    m->kind = kind;
    m->to_map = m->to_sub = -1;
    return m;
}

/* An exit's destination, read from the ROM (that map is not loaded): its sub-area's flags (bit 0: an enemy area, a
 * field or a dungeon with random battles; towns and their houses have people instead) and its sky (an outdoor
 * area). The area's block is the ROM from MapAreaDef.romStart, loaded at dst (Map_LoadArea), so a pointer into it
 * is romStart + (pointer - dst). -1 when not known. */
extern MapAreaDef map_area_defs[36];
const unsigned char *Port_HostRom(void);

static int dest_info(s32 map, s32 sub) {
    static s16 cache[36][64];
    static int init;
    if (!init) {
        for (int i = 0; i < 36; i++)
            for (int j = 0; j < 64; j++) cache[i][j] = -2;
        init = 1;
    }
    if (map < 0 || map >= 36 || sub < 0 || sub >= 64) return -1;
    if (cache[map][sub] != -2) return cache[map][sub];
    cache[map][sub] = -1;
    const MapAreaDef *d = &map_area_defs[map];
    const u8 *rom = Port_HostRom();
    const u32 rs = (u32)(uptr)d->romStart, re = (u32)(uptr)d->romEnd, dst = (u32)(uptr)d->dst;
    if (!rom || re <= rs) return -1;
#define AM_AT(a, n) ((a) >= dst && (a) - dst + (n) <= re - rs ? rom + rs + ((a) - dst) : NULL)
    const u32 sa = (u32)(uptr)d->subareas + (u32)sub * 0x18, envs = (u32)(uptr)d->envSettings;
    const u8 *s = AM_AT(sa, 0x18);
    if (!s) return -1;
    const u32 env = (u32)(s[2] << 8 | s[3]), flags = (u32)(s[0x14] << 8 | s[0x15]);
    const u8 *e = AM_AT(envs + env * 0x20, 0x20);
#undef AM_AT
    if (!e) return -1;
    const u32 mapflags = (u32)(e[0x18] << 8 | e[0x19]);
    cache[map][sub] = (s16)((flags & 1) | ((mapflags & 4) ? 2 : 0));
    return cache[map][sub];
}

static int holds_item(u32 item) {
    for (int i = 0; i < 0x96; i++)
        if (item_inventory[i] == item) return 1;
    return 0;
}

static void build_marks(void) {
    mark_count = 0;
    /* the ways out (map_subarea_def->exits): a door when the exit opens its door object (kind 2-4:
     * Player_StartAreaExit swings or slides map_objs[mapObj]), else a way out with no door. Exit_IsEnabled's item
     * test: 0x10 needs the item, 0x20 shuts once it is held. */
    for (s32 i = 0; i < map_subarea_def->exitCount; i++) {
        const MapExit *x = &map_subarea_def->exits[i];
        const u32 kind = (x->flags >> 8) & 0xF;
        const int door = kind >= 2 && kind <= 4 && x->mapObj < map_subarea_grid->count;
        ElAmMarker *m = new_mark(door ? EL_AM_MARK_DOOR : EL_AM_MARK_EXIT, (u32)i);
        if (!m) break;
        m->x = x->x; m->z = x->z; m->angle = x->angle; m->half_w = x->halfW; m->half_d = x->halfD;
        m->y = player_state.actor.y;
        if (door) {
            /* the doorway: the door object's place, along the exit's way through (the box is in front of it) */
            const MapObj *o = &map_objs[x->mapObj];
            const float fx = sinf(x->angle), fz = cosf(x->angle);
            const float t = (o->x - x->x) * fx + (o->z - x->z) * fz;
            m->x = x->x + fx * t;
            m->z = x->z + fz * t;
            m->y = o->y;
        }
        m->to_map = (int16_t)x->dest[0];
        m->to_sub = (int16_t)x->dest[1];
        /* another area: another map, an enemy area (a field, a dungeon: dest_info bit 0) on one side, and neither side
         * a building (no sky, no enemies: a house, an inn, a castle hall), whose doors are the town's or field's own */
        const int di = dest_info((s32)x->dest[0], (s32)x->dest[1]);
        const int here_enemy = map_subarea_flags & 1, here_house = !here_enemy && !(map_env_flags & 4);
        const int there_enemy = di >= 0 && (di & 1), there_house = di >= 0 && !(di & 1) && !(di & 2);
        if ((s32)x->dest[0] != map_id && (here_enemy || there_enemy) && !here_house && !there_house) m->flags |= EL_AM_MARK_AWAY;
        m->item = x->id;
        const char *what = (m->flags & EL_AM_MARK_AWAY) ? (door ? "Door to another area" : "Way to another area")
                                                         : (door ? "Door" : "Way through");
        char where[40];
        snprintf(where, sizeof where, " (map %d, area %d)", (int)x->dest[0], (int)x->dest[1]);
        char pre[80];
        if ((x->flags & 0x10) && !holds_item(x->id)) {
            m->flags |= EL_AM_MARK_LOCKED;
            snprintf(pre, sizeof pre, "Locked %s%s: needs ", door ? "door" : "way", where);
            label_of(m->label, sizeof m->label, pre, item_name(x->id), "an item");
        } else if ((x->flags & 0x20) && holds_item(x->id)) {
            m->flags |= EL_AM_MARK_SHUT;
            snprintf(pre, sizeof pre, "%s%s: shut while you hold ", what, where);
            label_of(m->label, sizeof m->label, pre, item_name(x->id), "an item");
        } else {
            snprintf(m->label, sizeof m->label, "%s%s", what, where);
        }
    }
    /* people (npc_slots: the sub-area's NPCs, where they stand now), named by their speaker name */
    for (s32 i = 0; i < npc_count && i < 16; i++) {
        const Npc *n = &npc_slots[i];
        ElAmMarker *m = new_mark(EL_AM_MARK_PERSON, (u32)i);
        if (!m) break;
        m->x = n->actor.x; m->y = n->actor.y; m->z = n->actor.z; m->angle = n->actor.rot;
        label_of(m->label, sizeof m->label, "", n->def ? n->def->name : NULL, "Person");
    }
    /* chests (chest_objects: the sub-area's, Chest_LoadForMap), opened per chest_opened_flags */
    for (s32 i = 0; i < chest_count && i < 16; i++) {
        const Actor *c = &chest_objects[i];
        ElAmMarker *m = new_mark(EL_AM_MARK_CHEST, (u32)i);
        if (!m) break;
        m->x = c->x; m->y = c->y; m->z = c->z; m->angle = c->rot;
        m->item = c->animSetIndex;
        if (Chest_IsOpened(c->floorFlags)) m->flags |= EL_AM_MARK_DONE;
        label_of(m->label, sizeof m->label, (m->flags & EL_AM_MARK_DONE) ? "Chest (opened): " : "Chest: ",
                 item_name(c->animSetIndex), "an item");
    }
    /* spirits (spirit_objects), taken per spirit_collected_flags; the element is the player's choice when taken */
    for (s32 i = 0; i < spirit_count && i < 8; i++) {
        const SpiritObj *sp = &spirit_objects[i];
        ElAmMarker *m = new_mark(EL_AM_MARK_SPIRIT, (u32)i);
        if (!m) break;
        m->x = sp->x; m->y = sp->y; m->z = sp->z;
        if (Spirit_IsCollected(sp->flagId)) m->flags |= EL_AM_MARK_DONE;
        snprintf(m->label, sizeof m->label, "%s",
                 (m->flags & EL_AM_MARK_DONE) ? "Spirit (taken)" : "Spirit: raises an element of your choice");
    }
    /* the boss standing in this sub-area, not yet beaten (Boss_LoadForArea), and how near starts the fight */
    if (boss_field_index > 0 && boss_field_index <= 8) {
        ElAmMarker *m = new_mark(EL_AM_MARK_BOSS, (u32)boss_field_index - 1);
        if (m) {
            const Actor *b = &boss_field_record.actor;
            m->x = b->x; m->y = b->y; m->z = b->z; m->angle = b->rot;
            m->radius = D_803AB48C[boss_field_index];
            label_of(m->label, sizeof m->label, "Boss: ", (const u8 *)Port_N64Ptr(D_803AC418, boss_field_index - 1), "a boss");
        }
    }
}

/* ELTALE_AUTOMAP_LOG: what a sub-area has to mark (once per plan) */
static void log_marks(void) {
    for (s32 i = 0; i < map_subarea_def->exitCount; i++) {
        const MapExit *x = &map_subarea_def->exits[i];
        const MapObj *o = x->mapObj < map_subarea_grid->count ? &map_objs[x->mapObj] : NULL;
        fprintf(stderr, "P1_AUTOMAP exit map=%d sub=%d i=%d at=%.1f,%.1f a=%.3f w=%.1f d=%.1f flags=%04x id=%u obj=%u "
                "objat=%.1f,%.1f,%.1f objang=%.1f objkind=%04x model=%d bounds=%d..%d,%d..%d scale=%.3f to=%u,%u,%u\n",
                (int)map_id, (int)map_subarea, (int)i, x->x, x->z, x->angle, x->halfW, x->halfD, x->flags, x->id, x->mapObj,
                o ? o->x : 0, o ? o->y : 0, o ? o->z : 0, o ? o->angle : 0, o ? o->kind : 0, o ? o->model : -1,
                o ? map_obj_models[o->model].minX : 0, o ? map_obj_models[o->model].maxX : 0,
                o ? map_obj_models[o->model].minZ : 0, o ? map_obj_models[o->model].maxZ : 0, o ? o->scale : 0,
                x->dest[0], x->dest[1], x->dest[2]);
    }
    u32 talk[8] = { 0 };
    for (s32 i = 0; i < npc_count && i < 16; i++) talk[npc_slots[i].def ? npc_slots[i].def->talk_kind & 7 : 0]++;
    fprintf(stderr, "P1_AUTOMAP marks map=%d sub=%d exits=%d npcs=%d (talk 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u) chests=%d "
            "spirits=%d boss=%d textspots=%d enemyarea=%d regions=%d\n", (int)map_id, (int)map_subarea,
            (int)map_subarea_def->exitCount, (int)npc_count, talk[0], talk[1], talk[2], talk[3], talk[4], talk[5], talk[6], talk[7],
            (int)chest_count, (int)spirit_count, (int)boss_field_index, (int)textspot_count, map_subarea_flags & 1,
            map_area_def && map_area_def->encounterRegions ? (int)map_area_def->encounterRegions[map_subarea].count : -1);
    for (s32 i = 0; i < npc_count && i < 16; i++) {
        const NpcDef *d = npc_slots[i].def;
        const char *en = d ? Port_TextPlain(d->name) : NULL;
        if (d)
            fprintf(stderr, "P1_AUTOMAP npc map=%d sub=%d i=%d talk=%u move=%u flags=%u item=%u name=%s\n", (int)map_id,
                    (int)map_subarea, (int)i, d->talk_kind, d->movement_kind, d->flags, d->u6.b.item_id, en ? en : "?");
    }
    for (s32 i = 0; i < chest_count && i < 16; i++) {
        const Actor *c = &chest_objects[i];
        const u8 *jp = item_name(c->animSetIndex);
        fprintf(stderr, "P1_AUTOMAP chest map=%d sub=%d i=%d at=%.1f,%.1f,%.1f flag=%u item=%u opened=%d name=%s\n", (int)map_id,
                (int)map_subarea, (int)i, c->x, c->y, c->z, c->floorFlags, c->animSetIndex, Chest_IsOpened(c->floorFlags) != 0,
                jp && Port_TextPlain(jp) ? Port_TextPlain(jp) : "?");
        if (jp && !Port_TextPlain(jp))
            fprintf(stderr, "P1_AUTOMAP chest name at %08x: %02x %02x %02x %02x %02x %02x\n", (unsigned)(uptr)jp, jp[0], jp[1], jp[2],
                    jp[3], jp[4], jp[5]);
    }
    for (s32 i = 0; i < spirit_count && i < 8; i++)
        fprintf(stderr, "P1_AUTOMAP spirit map=%d sub=%d i=%d at=%.1f,%.1f,%.1f flag=%u taken=%d\n", (int)map_id, (int)map_subarea,
                (int)i, spirit_objects[i].x, spirit_objects[i].y, spirit_objects[i].z, spirit_objects[i].flagId,
                Spirit_IsCollected(spirit_objects[i].flagId) != 0);
    if (boss_field_index > 0)
        fprintf(stderr, "P1_AUTOMAP boss map=%d sub=%d index=%d at=%.1f,%.1f radius=%.1f\n", (int)map_id, (int)map_subarea,
                (int)boss_field_index - 1, boss_field_record.actor.x, boss_field_record.actor.z, D_803AB48C[boss_field_index]);
    for (s32 i = 0; i < textspot_count && i < 16; i++) {
        const TextSpotDef *t = textspot_list[i].entry;
        fprintf(stderr, "P1_AUTOMAP textspot map=%d sub=%d at=%.1f,%.1f r=%.1f flags=%d\n", (int)map_id, (int)map_subarea,
                t->x, t->z, t->radius, (int)t->flags);
    }
}

/* ---- the truth view (a test) --------------------------------------------------------------------------- */

static int truth_env = -1, shots_env = -1;
static int truth_now;

int nd_automap_truth(void) { return truth_now; }

/* libultra's guLookAtF (not in this build's gu files): row vectors, the eye at the origin of the view */
static void look_at(float m[4][4], float ex, float ey, float ez, float ax, float ay, float az, float ux, float uy, float uz) {
    float lx = ax - ex, ly = ay - ey, lz = az - ez, l = -1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= l; ly *= l; lz *= l;
    float rx = uy * lz - uz * ly, ry = uz * lx - ux * lz, rz = ux * ly - uy * lx;
    l = 1.0f / sqrtf(rx * rx + ry * ry + rz * rz);
    rx *= l; ry *= l; rz *= l;
    ux = ly * rz - lz * ry; uy = lz * rx - lx * rz; uz = lx * ry - ly * rx;
    l = 1.0f / sqrtf(ux * ux + uy * uy + uz * uz);
    ux *= l; uy *= l; uz *= l;
    m[0][0] = rx; m[1][0] = ry; m[2][0] = rz; m[3][0] = -(ex * rx + ey * ry + ez * rz);
    m[0][1] = ux; m[1][1] = uy; m[2][1] = uz; m[3][1] = -(ex * ux + ey * uy + ez * uz);
    m[0][2] = lx; m[1][2] = ly; m[2][2] = lz; m[3][2] = -(ex * lx + ey * ly + ez * lz);
    m[0][3] = 0; m[1][3] = 0; m[2][3] = 0; m[3][3] = 1;
}

/* The view straight down over the plan's bounds: a perspective from far above (1% at the ends for 1,000 units of
 * height), north (+z) up. The host's map in its shot mode frames the same box (port/host/automap.cpp). */
void nd_automap_truth_camera(ElRdCamera *cam) {
    const float cx = (area.bounds[0] + area.bounds[2]) * 0.5f, cz = (area.bounds[1] + area.bounds[3]) * 0.5f;
    const float cy = (area.y_min + area.y_max) * 0.5f;
    const float aspect = cam->aspect > 0 ? cam->aspect : 1.3333334f;
    float half = fmaxf((area.bounds[3] - area.bounds[1]) * 0.5f, (area.bounds[2] - area.bounds[0]) * 0.5f / aspect) * 1.04f;
    if (half < 100.0f) half = 100.0f;
    const float H = 100000.0f;
    const float fovy = 2.0f * atanf(half / H) * 57.29577951308232f;
    const float near_ = H - (area.y_max - cy) - 200.0f, far_ = H + (cy - area.y_min) + 200.0f;
    float view[4][4], persp[4][4];
    u16 norm;
    /* (the eye a hair south of straight above, the up vector the game's y: the renderer's pictures between ticks
     * rebuild the view from the eye and the point looked at with that up vector, which straight down would not
     * define. North stays up; the tilt is 0.06 degrees) */
    look_at(view, cx, cy + H, cz - H * 0.001f, cx, cy, cz, 0.0f, 1.0f, 0.0f);
    guPerspectiveF(persp, &norm, fovy, aspect, near_, far_, 1.0f);
    float m[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += view[i][k] * persp[k][j];
            m[i * 4 + j] = s;
        }
    memcpy(cam->projection, m, sizeof m);
    memcpy(cam->view, view, sizeof view);
    memcpy(cam->perspective, persp, sizeof persp);
    cam->shake[0] = cam->shake[1] = 0.0f;
    cam->persp_norm = norm;
    cam->near_plane = near_;
    cam->far_plane = far_;
    cam->fovy = fovy;
    cam->eye[0] = cx; cam->eye[1] = cy + H; cam->eye[2] = cz - H * 0.001f;
    cam->at[0] = cx; cam->at[1] = cy; cam->at[2] = cz;
    cam->flags = 0;
}

/* At the tick's start (nd_frame_begin): is this tick drawn from above? */
void nd_automap_frame_begin(void) {
    if (truth_env < 0) truth_env = getenv("ELTALE_AUTOMAP_TRUTH") != NULL;
    if (shots_env < 0) shots_env = getenv("ELTALE_AUTOMAP_SHOTS") != NULL;
    const int play = core_game_mode == 1 && !(core_flags & 0x80) && have_plan && plan_map == map_id && plan_sub == map_subarea;
    truth_now = play && (truth_env || (shots_env && ticks_on_plan >= 15 && ticks_on_plan < 72));
}

/* ---- the tick ------------------------------------------------------------------------------------------ */

void nd_automap_tick(unsigned frame) {
    if (!send_area || !send_player) return;
    const int play = core_game_mode == 1 && !(core_flags & 0x80) && map_subarea_grid && map_objs && map_obj_models &&
                     map_subarea_def;
    if (play) {
        if (!have_plan || plan_map != map_id || plan_sub != map_subarea || plan_grid != map_subarea_grid ||
            plan_objs != map_objs) {
            build_plan();
            have_plan = 1;
            plan_map = map_id;
            plan_sub = map_subarea;
            plan_grid = map_subarea_grid;
            plan_objs = map_objs;
            ticks_on_plan = 0;
            send_area(&area);
        } else if (objs_hash(map_subarea_grid) != plan_objs_hash) {
            /* a collidable object moved (wave objects): the plan again, the same sub-area */
            build_plan();
            send_area(&area);
        }
    }
    static int log = -1;
    if (log < 0) log = getenv("ELTALE_AUTOMAP_LOG") != NULL;
    mark_count = 0;
    /* the marks: in play, and through the fade of a way out while the sub-area is still the plan's (the minimap keeps
     * them until the next area comes) */
    if (have_plan && core_game_mode == 1 && plan_map == map_id && plan_sub == map_subarea && map_subarea_grid == plan_grid &&
        map_objs == plan_objs && map_subarea_def) {
        build_marks();
        if (log && play && ticks_on_plan == 1) log_marks();
    }
    ElAmPlayer p;
    memset(&p, 0, sizeof p);
    p.markers = marks;
    p.marker_count = mark_count;
    p.frame = frame;
    p.area_serial = have_plan ? area.serial : 0;
    p.flags = (play && have_plan ? EL_AM_PLAYER_PLAY : 0) | ((battle_flags & 1) ? EL_AM_PLAYER_BATTLE : 0);
    if (shots_env > 0 && play && have_plan) {
        if (ticks_on_plan == 30) p.flags |= EL_AM_PLAYER_SHOT_TRUTH;
        if (ticks_on_plan == 42) p.flags |= EL_AM_PLAYER_SHOT_OVER;
        if (ticks_on_plan == 80) p.flags |= EL_AM_PLAYER_SHOT_MAP;
    }
    if (truth_now) p.flags |= EL_AM_PLAYER_TRUTH;
    p.x = player_state.actor.x;
    p.y = player_state.actor.y;
    p.z = player_state.actor.z;
    p.rot = player_state.actor.rot;
    send_player(&p);
    /* ELTALE_AUTOMAP_TRACE=1 (a test, tools/automap_path.py): where the player is, every tick in play */
    static int trace = -1;
    if (trace < 0) trace = getenv("ELTALE_AUTOMAP_TRACE") != NULL;
    if (trace && play && have_plan)
        fprintf(stderr, "P1_AUTOMAP_POS frame=%u map=%d sub=%d x=%.3f y=%.3f z=%.3f rot=%.4f\n", frame, (int)map_id,
                (int)map_subarea, p.x, p.y, p.z, p.rot);
    if (play && have_plan) ticks_on_plan++;
}
