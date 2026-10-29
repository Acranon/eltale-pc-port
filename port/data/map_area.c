/* Map area data: the ROM block Map_LoadArea reads into RDRAM is big-endian N64 records with 32-bit
 * pointers. The game's code reads these records through its native types (little-endian, 64-bit
 * pointers), so after the DMA we decode the area's record graph into native copies and the game
 * works on those. Display lists, textures and vertices stay in RDRAM as N64 bytes, and their
 * addresses stay N64 addresses (docs/P0_STATUS.md, "Address model"): the renderer reads them.
 *
 * Array lengths come from the records' own counts where they have one, from the largest index
 * the other records use, and otherwise from the next record start in the block: every aligned
 * word in the block that holds an address inside the block marks one. */
#include "port/data.h"
#include "game/battle/battle.h"
#include "game/camera/camera.h"
#include "game/env/env.h"
#include "game/map/chest.h"
#include "game/map/collision.h"
#include "game/map/map.h"
#include "game/npc/npc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

enum { ARENA_BYTES = 4 << 20, MAX_TARGETS = 1 << 16 };
static _Alignas(16) u8 arena[ARENA_BYTES]; /* .bss: below 4 GiB, like every address the game uses */
static size_t arena_used;
static u32 block_start, block_end;
static u32 targets[MAX_TARGETS];
static unsigned target_count;

static void *alloc(size_t bytes) {
    size_t at = (arena_used + 15) & ~(size_t)15;
    if (bytes > ARENA_BYTES - at) Port_HostFatal("map area decode arena full");
    arena_used = at + bytes;
    memset(arena + at, 0, bytes);
    return arena + at;
}

static int in_block(u32 a) { return a >= block_start && a < block_end; }
static const u8 *ram(u32 a) { return (const u8 *)(uptr)a; }
static u16 r16(u32 a) { const u8 *p = ram(a); return (u16)(p[0] << 8 | p[1]); }
static u32 r32(u32 a) { return (u32)r16(a) << 16 | r16(a + 2); }
static f32 rf32(u32 a) { u32 v = r32(a); f32 f; memcpy(&f, &v, 4); return f; }
static void *rptr(u32 a) { return (void *)(uptr)r32(a); } /* an N64 address, kept as is */

static int cmp_u32(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static void scan_targets(void) {
    target_count = 0;
    for (u32 a = block_start; a + 4 <= block_end; a += 4) {
        u32 v = r32(a);
        if (in_block(v) && !(v & 1) && target_count < MAX_TARGETS) targets[target_count++] = v;
    }
    qsort(targets, target_count, sizeof targets[0], cmp_u32);
}

/* Records of `size` bytes from `start` up to the next record start (or the block's end). */
static unsigned bounded_count(u32 start, u32 size, unsigned cap) {
    u32 end = block_end;
    unsigned lo = 0, hi = target_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (targets[mid] <= start) lo = mid + 1; else hi = mid;
    }
    if (lo < target_count && targets[lo] < end) end = targets[lo];
    unsigned n = (end - start) / size;
    return n < cap ? n : cap;
}

/* A pointer-free record has the same layout natively: copy it and swap each field by `shape`
 * (w: 4 bytes, h: 2 bytes, b: 1 byte). */
static void *decode_flat(u32 a, unsigned count, u32 size, const char *shape) {
    if (!a) return NULL;
    if (a < 0x80000000u || a >= 0x80800000u) Port_HostFatal("map record outside RDRAM");
    u8 *out = alloc((size_t)count * size);
    for (unsigned i = 0; i < count; i++) {
        u32 src = a + i * size, off = 0;
        u8 *dst = out + (size_t)i * size;
        for (const char *s = shape; *s; s++) {
            if (*s == 'w') { u32 v = r32(src + off); memcpy(dst + off, &v, 4); off += 4; }
            else if (*s == 'h') { u16 v = r16(src + off); memcpy(dst + off, &v, 2); off += 2; }
            else { dst[off] = ram(src)[off]; off += 1; }
        }
        if (off != size) Port_HostFatal("map record shape does not match its size");
    }
    return out;
}

static MapModel *decode_models(u32 a, unsigned count) {
    MapModel *m = alloc(count * sizeof *m);
    for (unsigned i = 0; i < count; i++, a += 0x20) {
        m[i].minX = r16(a); m[i].maxX = r16(a + 2); m[i].unk4 = r16(a + 4);
        m[i].unk6 = r16(a + 6); m[i].minZ = r16(a + 8); m[i].maxZ = r16(a + 0xA);
        m[i].dl = rptr(a + 0x10);
        m[i].ntris = r16(a + 0x18);
        u32 tris = r32(a + 0x14), verts = r32(a + 0x1C);
        m[i].tris = decode_flat(tris, m[i].ntris, 0x14, "hhhhwww");
        unsigned nverts = 0;
        for (unsigned t = 0; t < m[i].ntris; t++) {
            CollTri *tri = &m[i].tris[t];
            s16 v[3] = { tri->v0, tri->v1, tri->v2 };
            for (int k = 0; k < 3; k++) if (v[k] >= 0 && (unsigned)v[k] + 1 > nverts) nverts = v[k] + 1;
        }
        m[i].verts = verts ? decode_flat(verts, nverts ? nverts : 1, 8, "hhhh") : NULL;
    }
    return m;
}

static MapGrid *decode_grids(u32 a, unsigned count, unsigned *model_count) {
    MapGrid *g = alloc(count * sizeof *g);
    for (unsigned i = 0; i < count; i++, a += 0x2C) {
        g[i].count = r16(a); g[i].alwaysCount = r16(a + 2);
        g[i].w = r16(a + 4); g[i].h = r16(a + 6);
        g[i].originX = rf32(a + 8); g[i].originZ = rf32(a + 0xC);
        g[i].cellW = rf32(a + 0x10); g[i].cellD = rf32(a + 0x14);
        g[i].searchPattern = r16(a + 0x28);
        u32 objs = r32(a + 0x18), cells = r32(a + 0x1C), lists = r32(a + 0x20), always = r32(a + 0x24);
        unsigned nobjs = objs ? bounded_count(objs, 0x18, 4096) : 0;
        if (nobjs < g[i].count) nobjs = g[i].count;
        g[i].objs = decode_flat(objs, nobjs, 0x18, "wwwwwhh");
        for (unsigned k = 0; k < nobjs; k++) {
            s16 model = g[i].objs[k].model;
            if (model >= 0 && (unsigned)model + 1 > *model_count) *model_count = model + 1;
        }
        unsigned ncells = (unsigned)(g[i].w > 0 ? g[i].w : 0) * (unsigned)(g[i].h > 0 ? g[i].h : 0);
        g[i].cells = cells ? decode_flat(cells, ncells ? ncells : 1, 2, "h") : NULL;
        g[i].lists = lists ? decode_flat(lists, bounded_count(lists, 2, 1 << 16), 2, "h") : NULL;
        g[i].alwaysList = always ? decode_flat(always, g[i].alwaysCount ? g[i].alwaysCount : 1, 2, "h") : NULL;
    }
    return g;
}

static MapSubareaDef *decode_subareas(u32 a, unsigned count, unsigned *grids, unsigned *envs) {
    MapSubareaDef *s = alloc(count * sizeof *s);
    for (unsigned i = 0; i < count; i++, a += 0x18) {
        s[i].grid = r16(a); s[i].env = r16(a + 2);
        s[i].exitCount = (s32)r32(a + 8);
        s[i].areaSetIndex = r16(a + 0x12); s[i].flags = r16(a + 0x14);
        u32 exits = r32(a + 4), entries = r32(a + 0xC);
        s[i].exits = decode_flat(exits, s[i].exitCount > 0 ? (unsigned)s[i].exitCount : 0, 0x24, "wwwwwhhhhhhhh");
        s[i].entries = entries ? decode_flat(entries, bounded_count(entries, 0x14, 256), 0x14, "wwwhhhh") : NULL;
        if ((unsigned)s[i].grid + 1 > *grids) *grids = s[i].grid + 1;
        if ((unsigned)s[i].env + 1 > *envs) *envs = s[i].env + 1;
    }
    return s;
}

static EnvAreaSettings *decode_env(u32 a, unsigned count) {
    EnvAreaSettings *e = alloc(count * sizeof *e);
    for (unsigned i = 0; i < count; i++, a += 0x20) {
        e[i].count = (s32)r32(a + 4);
        e[i].frames = decode_flat(r32(a), e[i].count > 0 ? (unsigned)e[i].count : 0, 0x3C, "wwwwwwwwwwwwwww");
        e[i].hasLightDirection = (s32)r32(a + 8);
        e[i].lightDirX = (s32)r32(a + 0xC); e[i].lightDirY = (s32)r32(a + 0x10);
        e[i].lightDirZ = (s32)r32(a + 0x14);
        e[i].mapFlags = r16(a + 0x18);
        e[i].backdropId = (s32)r32(a + 0x1C);
    }
    return e;
}

static MapDlPair *decode_dl_pairs(u32 a) {
    if (!a) return NULL;
    unsigned n = bounded_count(a, 8, 1024);
    MapDlPair *p = alloc(n * sizeof *p);
    for (unsigned i = 0; i < n; i++) { p[i].a = rptr(a + i * 8); p[i].b = rptr(a + i * 8 + 4); }
    return p;
}

static CameraPresetSet *decode_camera(u32 a, unsigned count) {
    if (!a) return NULL;
    CameraPresetSet *c = alloc(count * sizeof *c);
    for (unsigned i = 0; i < count; i++, a += 8) {
        c[i].count = r16(a + 4);
        c[i].presets = decode_flat(r32(a), c[i].count, 0x38, "wwwwwwwwwwwwww");
    }
    return c;
}

static CollWalls *decode_walls(u32 a, unsigned count) {
    if (!a) return NULL;
    CollWalls *w = alloc(count * sizeof *w);
    for (unsigned i = 0; i < count; i++, a += 0x10) {
        w[i].count = (s32)r32(a + 0xC);
        unsigned nranges = w[i].count > 0 ? (unsigned)w[i].count : 0;
        w[i].ranges = decode_flat(r32(a + 8), nranges, 8, "ww");
        unsigned nidx = 0, nverts = 0;
        for (unsigned r = 0; r < nranges; r++) {
            unsigned end = (unsigned)w[i].ranges[r].start + (unsigned)w[i].ranges[r].count + 1;
            if (end > nidx) nidx = end;
        }
        w[i].idx = decode_flat(r32(a + 4), nidx, 4, "w");
        for (unsigned k = 0; k < nidx; k++)
            if ((unsigned)w[i].idx[k] + 1 > nverts) nverts = w[i].idx[k] + 1;
        w[i].verts = decode_flat(r32(a), nverts, 8, "ww");
    }
    return w;
}

static EncounterRegionList *decode_regions(u32 a, unsigned count) {
    if (!a) return NULL;
    EncounterRegionList *l = alloc(count * sizeof *l);
    for (unsigned i = 0; i < count; i++, a += 8) {
        u32 regions = r32(a);
        l[i].count = (s32)r32(a + 4);
        /* group_list is read as a u16 count and u16 group ids */
        l[i].regions = regions ? decode_flat(regions, l[i].count > 0 ? (unsigned)l[i].count : 0, 0x18,
                                             "hhhhhhhhhhhh") : NULL;
    }
    return l;
}

static EncounterGroupTable *decode_groups(u32 a, unsigned count) {
    if (!a) return NULL;
    EncounterGroupTable *t = alloc(count * sizeof *t);
    for (unsigned i = 0; i < count; i++, a += 0x10) {
        u32 forms = r32(a), groups = r32(a + 4), points = r32(a + 8);
        t[i].point_count = (s32)r32(a + 0xC);
        t[i].points = decode_flat(points, t[i].point_count > 0 ? (unsigned)t[i].point_count : 0, 4, "hh");
        if (forms) {
            unsigned n = bounded_count(forms, 4, 256);
            struct Vec3f **f = alloc(n * sizeof *f);
            for (unsigned k = 0; k < n; k++) {
                u32 v = r32(forms + k * 4);
                f[k] = in_block(v) ? decode_flat(v, bounded_count(v, 12, 64), 12, "www") : NULL;
            }
            t[i].formation_positions = f;
        }
        if (groups) {
            unsigned n = bounded_count(groups, 8, 256);
            EncounterGroup *g = alloc(n * sizeof *g);
            for (unsigned k = 0; k < n; k++) {
                u32 entries = r32(groups + k * 8);
                g[k].entry_count = r16(groups + k * 8 + 4);
                g[k].formation_start = r16(groups + k * 8 + 6);
                g[k].entries = in_block(entries) ? decode_flat(entries, g[k].entry_count, 0xC, "www") : NULL;
            }
            t[i].groups = g;
        }
    }
    return t;
}

static NpcDef *decode_npc_defs(u32 defs, unsigned count);

static NpcList *decode_npcs(u32 a) {
    if (!a) return NULL;
    NpcList *l = alloc(sizeof *l);
    l->count = r16(a);
    l->defs = decode_npc_defs(r32(a + 4), l->count);
    return l;
}

static NpcDef *decode_npc_defs(u32 defs, unsigned count) {
    NpcDef *d = alloc(count * sizeof *d);
    for (unsigned i = 0; i < count; i++, defs += 0x2C) {
        d[i].area = r16(defs); d[i].talk_kind = r16(defs + 2);
        d[i].movement_kind = ram(defs)[4]; d[i].flags = ram(defs)[5];
        d[i].u6.boss_index = r16(defs + 6); /* the u8 view is ordered for the host (npc.h) */
        d[i].message_offset = r16(defs + 8); d[i].alternate_message_offset = r16(defs + 0xA);
        d[i].anim_sets = rptr(defs + 0x10); /* an N64 address: the NPC asset block, not decoded here */
        d[i].name = rptr(defs + 0x14);
        d[i].talk_anim_index = r16(defs + 0x18);
        d[i].x = rf32(defs + 0x1C); d[i].z = rf32(defs + 0x20);
        d[i].angle = rf32(defs + 0x24); d[i].scale = rf32(defs + 0x28);
    }
    return d;
}

static MapAreaDef *decode_area(const MapAreaDef *def) {
    static MapAreaDef out;
    arena_used = 0;
    block_start = (u32)(uptr)def->dst;
    block_end = block_start + (u32)((uptr)def->romEnd - (uptr)def->romStart);
    scan_targets();
    out = *def;
    u32 subareas = (u32)(uptr)def->subareas;
    unsigned nsub = bounded_count(subareas, 0x18, 64), ngrids = 0, nenvs = 0, nmodels = 0;
    if (!nsub) Port_HostFatal("map area has no sub-areas");
    out.subareas = decode_subareas(subareas, nsub, &ngrids, &nenvs);
    out.grids = decode_grids((u32)(uptr)def->grids, ngrids, &nmodels);
    out.models = decode_models((u32)(uptr)def->models, nmodels);
    out.envSettings = decode_env((u32)(uptr)def->envSettings, nenvs);
    out.animTexDls = decode_dl_pairs((u32)(uptr)def->animTexDls);
    out.cameraPresets = decode_camera((u32)(uptr)def->cameraPresets, nsub);
    out.cameraWalls = decode_walls((u32)(uptr)def->cameraWalls, nsub);
    out.actorWalls = decode_walls((u32)(uptr)def->actorWalls, nsub);
    out.encounterRegions = decode_regions((u32)(uptr)def->encounterRegions, nsub);
    out.encounterGroups = decode_groups((u32)(uptr)def->encounterGroups, nsub);
    out.npcList = decode_npcs((u32)(uptr)def->npcList);
    return &out;
}

MapAreaDef *Port_MapArea(const MapAreaDef *def, int reloaded) {
    static const MapAreaDef *last_def;
    static MapAreaDef *last;
    if (reloaded || def != last_def || !last) {
        last = decode_area(def);
        last_def = def;
    }
    return last;
}

/* chest_map_placements[].groups points at the area block: one ChestGroup per sub-area. */
ChestGroup *Port_ChestGroup(const ChestGroup *groups, s32 subarea) {
    u32 a = (u32)(uptr)groups + (u32)subarea * 8;
    ChestGroup *g = alloc(sizeof *g);
    g->count = r16(a);
    g->defs = decode_flat(r32(a + 4), g->count, 0x24, "wwwwwwwwbbbb");
    return g;
}

/* A scene's NPC table (SceneEntry.npc_defs_addr): native copies, in the current area's arena. */
NpcDef *Port_NpcDefs(u32 addr, s32 count) {
    return count > 0 ? decode_npc_defs(addr, (unsigned)count) : NULL;
}
