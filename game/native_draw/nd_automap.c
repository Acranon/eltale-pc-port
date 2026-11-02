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
#include "game/enemy/enemy.h"
#include "game/save/save_menu.h"
#include "port/data.h"
#include "port/text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*send_area)(const ElAmArea *);
static void (*send_player)(const ElAmPlayer *);
static void (*send_save)(int, uint64_t);

void nd_automap_connect(void *area_fn, void *player_fn, void *save_fn) {
    send_area = (void (*)(const ElAmArea *))area_fn;
    send_player = (void (*)(const ElAmPlayer *))player_fn;
    send_save = (void (*)(int, uint64_t))save_fn;
}

/* (fog of war) the save menu's save or load of `slot`, an F2 or F4 (save_menu.c's SaveMenu_CaptureGameState and
 * SaveMenu_RestoreGameState call it, patches/p0.patch): the host keeps what was explored with each save. The key is
 * a hash of the saved record, the same when it is written and when it is read back (from the Controller Pak or a
 * quick save file): the time played, the day, the place and the chests, spirits, items and bosses block. Read-only. */
void Port_AutomapSaveEvent(int kind, s32 slot) {
    if (slot < 0 || slot >= 17) return;
    const SaveHeader *e = &save_menu_save_headers[slot];
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    const u32 head[4] = { e->play_time, e->day_count, (u32)e->map_id, (u32)e->subarea };
    const u8 *parts[2] = { (const u8 *)head, (const u8 *)&save_menu_game_state_block };
    const size_t sizes[2] = { sizeof head, sizeof save_menu_game_state_block };
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < sizes[k]; i++) h = (h ^ parts[k][i]) * UINT64_C(0x100000001B3);
    /* ELTALE_AUTOMAP_FOG_LOG=1 (a test): the key, headless too (no host there to keep a file) */
    static int log = -1;
    if (log < 0) log = getenv("ELTALE_AUTOMAP_FOG_LOG") != NULL;
    if (log) fprintf(stderr, "P1_AUTOMAP_SAVE_KEY %s slot=%d key=%016llx\n", kind ? "read" : "write", (int)slot, (unsigned long long)h);
    if (send_save) send_save(kind ? EL_AM_SAVE_READ : EL_AM_SAVE_WRITE, h);
}

/* ---- the plan ------------------------------------------------------------------------------------------ */

static ElAmTri *tris;
static ElAmWall *walls;
static ElAmExit *exits;
static ElAmTri *water;
static uint32_t water_count, water_cap;
static ElAmZone zones[64];
static float zone_points[2 * 256];
static uint32_t zone_count, zone_point_count;
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

/* the game's text tables at fixed RDRAM addresses (big-endian pointers): the item names, the bosses' names */
extern void *D_803AB288[];
extern u8 *D_803AC418[];
extern f32 D_803AB48C[]; /* per boss + 1: how near starts the fight (swapped to native: port/data/common.c) */
extern ItemDef D_803AAAA0[]; /* the item defs (swapped to native): handler_index 1 restores, 2 warps */

/* The random battle regions of the sub-area (Battle_ResetState's tables: map_area_def->encounterRegions and
 * encounterGroups[map_subarea], only in an enemy area), each named by its groups' monsters (the enemy area set's
 * defs and names, loaded for the sub-area: Enemy_LoadAreaSet) */
static void build_zones(void) {
    zone_count = zone_point_count = 0;
    if (!(map_subarea_flags & 1) || !map_area_def || !map_area_def->encounterRegions || !map_area_def->encounterGroups) return;
    const EncounterRegionList *rl = &map_area_def->encounterRegions[map_subarea];
    const EncounterGroupTable *gt = &map_area_def->encounterGroups[map_subarea];
    for (s32 i = 0; gt->points && i < gt->point_count && zone_point_count < 256; i++) {
        zone_points[zone_point_count * 2] = gt->points[i].x;
        zone_points[zone_point_count * 2 + 1] = gt->points[i].z;
        zone_point_count++;
    }
    for (s32 i = 0; rl->regions && i < rl->count && zone_count < 64; i++) {
        const EncounterRegion *r = &rl->regions[i];
        ElAmZone *z = &zones[zone_count++];
        memset(z, 0, sizeof *z);
        z->x0 = r->x; z->z0 = r->z; z->x1 = (float)r->x + r->x_extent; z->z1 = (float)r->z + r->z_extent;
        const u16 *gl = (const u16 *)r->group_list;
        z->group_count = gl[0] > 7 ? 7 : gl[0];
        size_t at = 0;
        for (u32 g = 0; g < z->group_count; g++) {
            z->groups[g] = gl[1 + g];
            at += (size_t)snprintf(z->label + at, at < sizeof z->label ? sizeof z->label - at : 0, "%s", g ? " / " : "");
            const EncounterGroup *grp = gt->groups ? &gt->groups[gl[1 + g]] : NULL;
            int named = 0;
            for (u32 k = 0; grp && grp->entries && k < grp->entry_count && at < sizeof z->label; k++) {
                const EncounterGroupEntry *e = &grp->entries[k];
                const char *en = NULL;
                if (enemy_area_set && enemy_area_set->defs && enemy_names) {
                    const EnemyDef *d = &enemy_area_set->defs[e->enemy_def_index];
                    en = Port_TextMonsterPlain((const u8 *)Port_N64Ptr(enemy_names, d->anim_set_index));
                }
                if (!en) break;
                const s32 lo = e->count, hi = e->count + e->extra_count_max;
                if (hi > 1)
                    at += (size_t)snprintf(z->label + at, sizeof z->label - at, lo == hi ? "%s%s x%d" : "%s%s x%d-%d", named ? ", " : "", en, lo, hi);
                else
                    at += (size_t)snprintf(z->label + at, sizeof z->label - at, "%s%s", named ? ", " : "", en);
                named = 1;
            }
            if (!named && at < sizeof z->label) at += (size_t)snprintf(z->label + at, sizeof z->label - at, "group %u", gl[1 + g]);
        }
        if (at >= sizeof z->label) z->label[sizeof z->label - 1] = 0;
    }
}

/* The region a battle could start in at (x, z), by the game's rule (Battle_FindEncounterRegion: the first region
 * holding the point; Battle_ChooseNearbyEncounterPoint: a point at squared distance 2500..8100), -1 none */
static int zone_at(float x, float z) {
    int found = -1;
    for (u32 i = 0; i < zone_count; i++) {
        const EncounterRegion *r = &map_area_def->encounterRegions[map_subarea].regions[i];
        if (r->x <= x && r->z <= z && x - r->x <= r->x_extent && z - r->z <= r->z_extent) { found = (int)i; break; }
    }
    if (found < 0) return -1;
    for (u32 i = 0; i < zone_point_count; i++) {
        const float dx = zone_points[i * 2] - x, dz = zone_points[i * 2 + 1] - z, d = dx * dx + dz * dz;
        if (d >= 2500.0f && d <= 8100.0f) return found;
    }
    return -1;
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
    build_zones();
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
    area.zones = zones; area.zone_count = zone_count;
    area.points = zone_points; area.point_count = zone_point_count;
    if (zone_count && zone_point_count) area.flags |= EL_AM_AREA_ENEMY;
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

/* What a person offers (EL_AM_SVC_*), from their NpcDef: talk kind 6 is the innkeeper's rest choice; talk kind 2
 * gives def item when the player holds none (Npc_StartTalk: Item_CountInInventory == 0, then Item_AddToInventory),
 * and the item's own use (D_803AAAA0[item].handler_index: 2 Item_UseWarp, the Traveler's Wings; 1
 * Item_UseRestore with HP to give, unk4: the bread) says which. Other items (no use: the story's key items) are 0 with *key set. */
static int service_of(u32 talk, u32 item, int *key) {
    if (key) *key = 0;
    if (talk == 6) return EL_AM_SVC_INN;
    if (talk != 2) return EL_AM_SVC_NONE;
    const u32 h = item < 25 ? D_803AAAA0[item].handler_index : 0;
    if (h == 2) return EL_AM_SVC_WINGS;
    if (h == 1 && D_803AAAA0[item].unk4 > 0) return EL_AM_SVC_FOOD; /* restores HP (unk4; unk6 is MP) */
    if (h == 1) return EL_AM_SVC_NONE; /* (an MP remedy: not marked, docs/AUTOMAP.md) */
    if (key) *key = 1;
    return EL_AM_SVC_NONE;
}

/* A sub-area of another map (or this one) from the ROM image, as dest_info reads it: its NPCs' services (the map's
 * NpcList, NpcDef.area == sub; none in an enemy area: Npc_InitArea) and its exits' destinations. */
static const u8 *rom_at(const MapAreaDef *d, u32 a, u32 n) {
    const u8 *rom = Port_HostRom();
    const u32 rs = (u32)(uptr)d->romStart, re = (u32)(uptr)d->romEnd, dst = (u32)(uptr)d->dst;
    if (!rom || re <= rs || a < dst || a - dst + n > re - rs) return NULL;
    return rom + rs + (a - dst);
}
static u32 rd16(const u8 *p) { return (u32)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return rd16(p) << 16 | rd16(p + 2); }

static int own_services(s32 map, s32 sub) {
    static s8 cache[36][64];
    static int init;
    if (!init) { memset(cache, -1, sizeof cache); init = 1; }
    if (map < 0 || map >= 36 || sub < 0 || sub >= 64) return 0;
    if (cache[map][sub] >= 0) return cache[map][sub];
    cache[map][sub] = 0;
    const int di = dest_info(map, sub);
    if (di < 0 || (di & 1)) return 0;
    const MapAreaDef *d = &map_area_defs[map];
    const u8 *l = rom_at(d, (u32)(uptr)d->npcList, 8);
    if (!l) return 0;
    const u32 n = rd16(l), defs = rd32(l + 4);
    int bits = 0;
    for (u32 i = 0; i < n && i < 256; i++) {
        const u8 *e = rom_at(d, defs + i * 0x2C, 0x2C);
        if (!e) break;
        if (rd16(e) != (u32)sub) continue;
        const int sv = service_of(rd16(e + 2), e[7], NULL);
        if (sv) bits |= (int)EL_AM_SVC_BIT(sv);
    }
    cache[map][sub] = (s8)bits;
    return bits;
}

/* a building: no sky and no enemies (a house, an inn, a tavern, a castle hall) */
static int is_building(s32 map, s32 sub) {
    const int di = dest_info(map, sub);
    return di >= 0 && !(di & 1) && !(di & 2);
}

/* What lies behind an exit to (map, sub): the services of that building or room and of the rooms one or two doors
 * further in (exits to other buildings of the same map; not back to `from`, the sub-area the door is in). An exit
 * to a street or a field is not a door to a service. */
static int services_behind(s32 map, s32 sub, s32 from) {
    if (!is_building(map, sub)) return 0;
    s32 at[16], depth[16];
    int n = 0, bits = 0;
    at[n] = sub; depth[n++] = 0;
    for (int q = 0; q < n; q++) {
        bits |= own_services(map, at[q]);
        if (depth[q] >= 2) continue;
        const MapAreaDef *d = &map_area_defs[map];
        const u8 *sa = rom_at(d, (u32)(uptr)d->subareas + (u32)at[q] * 0x18, 0x18);
        if (!sa) continue;
        const u32 ex = rd32(sa + 4), cnt = rd32(sa + 8);
        for (u32 i = 0; i < cnt && i < 64; i++) {
            const u8 *e = rom_at(d, ex + i * 0x24, 0x24);
            if (!e) break;
            const s32 m = (s32)rd16(e + 0x1E), s2 = (s32)rd16(e + 0x20);
            if (m != map || s2 == from || !is_building(m, s2)) continue;
            int seen = 0;
            for (int k = 0; k < n; k++) seen |= at[k] == s2;
            if (seen || n >= 16) continue;
            at[n] = s2; depth[n++] = depth[q] + 1;
        }
    }
    return bits;
}

static const char *const service_names[EL_AM_SVC_COUNT] = { "", "Inn", "Traveler's Wings", "Food" };

static void services_text(char *out, size_t size, int bits) {
    size_t at = 0;
    out[0] = 0;
    for (int k = 1; k < EL_AM_SVC_COUNT; k++)
        if (bits & (int)EL_AM_SVC_BIT(k) && at < size)
            at += (size_t)snprintf(out + at, size - at, "%s%s", at ? ", " : "", service_names[k]);
}

/* The sign's text: the ROM text block (D_D37700, a ROM address) + textOffset, as TextSpot_LoadMessage reads it */
extern u8 D_D37700[];

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
        /* the services behind it (an inn, the Wings, food: in the building or a room or two further in) */
        m->services = (u8)services_behind((s32)x->dest[0], (s32)x->dest[1], (s32)x->dest[0] == map_id ? map_subarea : -1);
        if (m->services) services_text(m->detail, sizeof m->detail, m->services);
    }
    /* people (npc_slots: the sub-area's NPCs, where they stand now), named by their speaker name */
    for (s32 i = 0; i < npc_count && i < 16; i++) {
        const Npc *n = &npc_slots[i];
        ElAmMarker *m = new_mark(EL_AM_MARK_PERSON, (u32)i);
        if (!m) break;
        m->x = n->actor.x; m->y = n->actor.y; m->z = n->actor.z; m->angle = n->actor.rot;
        label_of(m->label, sizeof m->label, "", n->def ? n->def->name : NULL, "Person");
        if (!n->def) continue;
        int key = 0;
        m->service = (u8)service_of(n->def->talk_kind, n->def->u6.b.item_id, &key);
        m->item = n->def->talk_kind == 2 ? n->def->u6.b.item_id : 0;
        if (m->service == EL_AM_SVC_INN) {
            snprintf(m->detail, sizeof m->detail, "Inn: rest and save");
        } else if (m->service) {
            label_of(m->detail, sizeof m->detail, "Gives ", item_name(m->item), service_names[m->service]);
            const size_t at = strlen(m->detail);
            snprintf(m->detail + at, sizeof m->detail - at, " when you have none");
        } else if (key) {
            m->flags |= EL_AM_MARK_KEY_GIVER;
            label_of(m->detail, sizeof m->detail, "Gives ", item_name(m->item), "an item");
        } else if (n->def->talk_kind == 5) {
            m->flags |= EL_AM_MARK_STORY;
            snprintf(m->detail, sizeof m->detail, "Has something to say about the story");
        }
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
    /* signs and other things to read (textspot_list: TextSpot_LoadForMap's, this sub-area's), by their
     * textspot_table index; the text's first line from the ROM through the English script */
    const u8 *rom = Port_HostRom();
    for (s32 i = 0; i < textspot_count && i < 16; i++) {
        const TextSpotDef *t = textspot_list[i].entry;
        if (!t) continue;
        ElAmMarker *m = new_mark(EL_AM_MARK_SIGN, (u32)(t - textspot_table));
        if (!m) break;
        m->x = t->x; m->z = t->z; m->y = player_state.actor.y; m->angle = t->angle; m->radius = t->radius;
        const u32 at = (u32)(uptr)D_D37700 + t->textOffset;
        label_of(m->label, sizeof m->label, "Sign: ", rom ? rom + at : NULL, "");
        if (!strcmp(m->label, "Sign: ")) snprintf(m->label, sizeof m->label, "Sign");
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
            fprintf(stderr, "P1_AUTOMAP npc map=%d sub=%d i=%d talk=%u move=%u flags=%u item=%u use=%u name=%s\n", (int)map_id,
                    (int)map_subarea, (int)i, d->talk_kind, d->movement_kind, d->flags, d->u6.b.item_id,
                    d->u6.b.item_id < 25 ? D_803AAAA0[d->u6.b.item_id].handler_index : 99, en ? en : "?");
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
        fprintf(stderr, "P1_AUTOMAP textspot map=%d sub=%d index=%d at=%.1f,%.1f r=%.1f flags=%d\n", (int)map_id, (int)map_subarea,
                (int)(t - textspot_table), t->x, t->z, t->radius, (int)t->flags);
    }
    for (u32 i = 0; i < zone_count; i++)
        fprintf(stderr, "P1_AUTOMAP zone map=%d sub=%d i=%u box=%.0f,%.0f..%.0f,%.0f groups=%u label=%s\n", (int)map_id,
                (int)map_subarea, i, zones[i].x0, zones[i].z0, zones[i].x1, zones[i].z1, zones[i].group_count, zones[i].label);
    for (u32 i = 0; i < map_subarea_def->exitCount && i < 64; i++) {
        const MapExit *x = &map_subarea_def->exits[i];
        const int sv = services_behind((s32)x->dest[0], (s32)x->dest[1], (s32)x->dest[0] == map_id ? map_subarea : -1);
        if (sv) fprintf(stderr, "P1_AUTOMAP service_door map=%d sub=%d exit=%u to=%u,%u services=%d\n", (int)map_id,
                        (int)map_subarea, i, x->dest[0], x->dest[1], sv);
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
    /* (fog of war) the intro or the title: what was explored is forgotten (a reset, the end, New Game) */
    {
        static int was_title;
        const int title = core_game_mode == 3 || core_game_mode == 4;
        if (title && !was_title && send_save) send_save(EL_AM_SAVE_TITLE, 0);
        was_title = title;
    }
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
    p.flags = (play && have_plan ? EL_AM_PLAYER_PLAY : 0) | ((battle_flags & 1) ? EL_AM_PLAYER_BATTLE : 0) |
              ((battle_flags & 1) && !(battle_flags & 0x100) ? EL_AM_PLAYER_RANDOM_BATTLE : 0);
    /* the random battle roll (Battle_TryStartRandomEncounter, run at the next tick's start from where the player is
     * now): read-only */
    p.travel = battle_travel_accumulator;
    p.roll_limit = battle_encounter_roll_limit;
    p.zone = -1;
    if (play && have_plan && (map_subarea_flags & 1) && !(battle_flags & 1) && !(battle_flags & 0x8000) &&
        !(player_state.actor.animFlags & 1)) {
        p.danger |= EL_AM_DANGER_ROLLS;
        p.zone = (int16_t)zone_at(player_state.actor.x, player_state.actor.z);
        if (p.zone >= 0) p.danger |= EL_AM_DANGER_ZONE;
    }
    /* ELTALE_AUTOMAP_BATTLES=1 (a test): each random battle's start, where the roll found the player (the last
     * tick's place) and the region the rule gives there */
    static int blog = -1;
    if (blog < 0) blog = getenv("ELTALE_AUTOMAP_BATTLES") != NULL;
    static int was_battle;
    static float last_x, last_z;
    static int last_zone = -1;
    if (blog && (battle_flags & 1) && !was_battle && !(battle_flags & 0x100) && have_plan)
        fprintf(stderr, "P1_AUTOMAP_BATTLE map=%d sub=%d x=%.3f z=%.3f rule_zone=%d centre=%.1f,%.1f\n", (int)map_id,
                (int)map_subarea, last_x, last_z, last_zone, battle_center_x, battle_center_z);
    was_battle = battle_flags & 1;
    last_x = player_state.actor.x;
    last_z = player_state.actor.z;
    last_zone = have_plan && plan_map == map_id && plan_sub == map_subarea ? zone_at(last_x, last_z) : -1;
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
