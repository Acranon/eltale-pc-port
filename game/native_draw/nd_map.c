/* nd_map.c: map objects drawn natively (EL_KIND_MAP; docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * MapObj_UpdateAndDrawNearby (static, spline-animated, door-slot and wave objects, spinners) and
 * MapObj_DrawTranslucent (translucent and wave objects, scrolling-texture objects) call nd_map_draw for
 * each object they draw, with the matrix they built for it. The model is entered with the state the
 * frame list has at that point (the entry lists below); what changes per draw (the light slot's env
 * colour, the translucent alpha, the scroll offset) is the draw's. */
#include "game/native_draw/nd_internal.h"

#include "game/core/main.h"
#include "game/map/map.h"

#include <string.h>

/* The entry states (made in nd_map_attach): what the frame list has set when it calls a model */
static Gfx entry_map[8], entry_xlu[8], entry_scroll[5][24];

extern Gfx D_10000E8[];
extern u8 *mapobj_scrolltex_tex[5];
extern u16 mapobj_scrolltex_height[6];

void nd_map_attach(void) {
    Gfx *g = entry_map;
    /* Gfx_BuildFrameDisplayList: 2-cycle; MapObj_UpdateAndDrawNearby: the map's setup list */
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gSPDisplayList(g++, D_10000E8);
    gSPEndDisplayList(g++);
    /* MapObj_DrawTranslucent (after MapObj_UpdateAndDrawNearby set bilinear filtering and no TLUT) */
    g = entry_xlu;
    gSPDisplayList(g++, entry_map);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetRenderMode(g++, 0x0C1849D8, 0);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gSPEndDisplayList(g++);
    for (int k = 0; k < 5; k++) {
        u32 h = mapobj_scrolltex_height[k];
        g = entry_scroll[k];
        gSPDisplayList(g++, entry_xlu);
        gDPSetRenderMode(g++, 0xC81049D8, 0);
        gDPSetCombine(g++, 0x121A04, 0xFFFFFFF8);
        gDPLoadTextureTile(g++, mapobj_scrolltex_tex[k], G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, h, 0, 0, 31, h - 1, 0, G_TX_WRAP,
                           G_TX_WRAP, 5, (h == 32) ? 5 : 6, G_TX_NOLOD, G_TX_NOLOD);
        /* the scroll offset is the draw's (ElRdDraw.tile_offset) */
        gDPSetTileSize(g++, G_TX_RENDERTILE, 0, 0, 32 << 2, h << 2);
        gSPEndDisplayList(g++);
    }
}

void nd_map_draw(const struct MapObj *obj, int pass, const void *mtx, uint32_t dl, int xform, float x, float y,
                 float z, float rx, float ry, float rz, float tile_t) {
    const void *entry = pass == ND_MAP_OPAQUE ? (const void *)entry_map
                      : pass == ND_MAP_XLU ? (const void *)entry_xlu
                      : (const void *)entry_scroll[pass - ND_MAP_SCROLL];
    const NdMesh *mesh = nd_model_get(dl, entry);
    if (!mesh) return;
    nd_model_use(mesh);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_MAP << EL_RDD_KIND_SHIFT | (uint64_t)pass << 48 | ((uint64_t)(uintptr_t)obj & 0xFFFFFFFFFFFFull);
    d.mesh = nd_model_id(mesh);
    nd_decode_mtx(mtx, d.matrix);
    d.xform = (uint32_t)xform;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.rot[0] = rx; d.rot[1] = ry; d.rot[2] = rz;
    d.scale = obj->scale;
    d.tile_offset[1] = tile_t;
    nd_api()->draw(&d);
}

/* ---- R7b: the draw distance (docs/renderer/NATIVE_DRAW.md "Modern options") ----------------------------------
 *
 * The game draws the map objects of the grid cells around the camera's eye (MapObj_UpdateAndDrawNearby: a disc of
 * 7, 5 or 2 cells, or a 3x3 square, by the sub-area's search pattern; pattern 0 draws the whole sub-area), and its
 * fog ends at about the disc's edge. With the Graphics page's draw distance k > 1 the renderer pushes the fog and
 * the far plane k times as far (nd_renderer.cpp), and these functions draw the objects of the cells in a disc k
 * times as wide that the game's search left out, and the billboards out to k times the game's 1,500 units.
 *
 * Drawing only: nothing the game keeps is touched. The game's collection (MapObj_CollectCell) also updates what it
 * collects (wave objects move, spline objects step, emitters spawn sprites with Math_Rand, ambient sounds, spinners),
 * so widening the game's own search would change the game; this pass reads the grid and the objects as they are and
 * only sends draws: static objects (spline-animated ones at their current frame, frozen until the game's search
 * reaches them), translucent and scrolling-texture objects, billboards (without the shadow the game queues for
 * its own). Wave objects, emitters, spinners and the candle flames are left to the game's own distance. The ids are
 * the game's (the object's address), so an object passing from this ring into the game's disc stays one item for
 * the pictures between ticks. Classic (k = 1) draws nothing here. */

#include "game/ui/menu.h"
#include "game/math/keyframe.h"
#include "game/math/math.h"
#include "game/camera/camera.h"
#include "game/native_draw/nd_billboard.h"

#include <math.h>

#define FAR_MAX 4096
static MapObj *far_opaque[FAR_MAX], *far_billboard[FAR_MAX], *far_xlu[FAR_MAX];
static int far_opaque_n, far_billboard_n, far_billboard_game, far_xlu_n;
static float far_k = 1.0f;

/* the game's search: its offsets, and its radius in cells */
static const CellOffset *far_game_cells(int pattern, int *n, int *radius) {
    switch (pattern) {
    case 1: *n = 0x9D; *radius = 7; return mapobj_cell_disc_r7;
    case 2: *n = 0x59; *radius = 5; return mapobj_cell_disc_r5;
    case 3: *n = 9; *radius = 1; return mapobj_cell_square_3x3;
    case 4: *n = 0x15; *radius = 2; return mapobj_cell_disc_r2;
    default: return NULL;
    }
}

static void far_take(MapObj *s) {
    u16 kind = (s->kind & 0x2000) ? 0 : (u8)s->kind;
    if (!kind || kind == 8) {
        if (far_opaque_n < FAR_MAX) far_opaque[far_opaque_n++] = s;
    } else if (kind == 0x10) {
        if (far_billboard_n < FAR_MAX) far_billboard[far_billboard_n++] = s;
    } else if (kind == 0x7F || kind == 0x80 || (kind >= 0x40 && kind < 0x45)) {
        if (far_xlu_n < FAR_MAX) far_xlu[far_xlu_n++] = s;
    }
}

/* At the end of the game's opaque objects (MapObj_UpdateAndDrawNearby): the ring's cells collected, its opaque
 * objects drawn */
void nd_map_far_opaque(void) {
    far_opaque_n = far_billboard_n = far_billboard_game = far_xlu_n = 0;
    far_k = nd_api()->draw_distance ? nd_api()->draw_distance() : 1.0f;
    if (far_k <= 1.0f) return;
    const MapGrid *g = map_subarea_grid;
    int n = 0, r = 0;
    const CellOffset *cells = far_game_cells(g->searchPattern, &n, &r);
    if (cells) {
        /* the game's cells, so the ring leaves them out */
        enum { SPAN = 17 };
        u8 game[SPAN][SPAN];
        memset(game, 0, sizeof game);
        for (int i = 0; i < n; i++) game[cells[i].dz + SPAN / 2][cells[i].dx + SPAN / 2] = 1;
        /* (the eye's cell as the game works it out) */
        s32 cx = (camera_state.eyeX - g->originX) / g->cellW;
        s32 cz = (camera_state.eyeZ - g->originZ) / g->cellD;
        float rf = (r + 0.5f) * far_k;
        int R = (int)rf;
        if (R > 40) R = 40;
        for (int dz = -R; dz <= R; dz++)
            for (int dx = -R; dx <= R; dx++) {
                if ((float)(dx * dx + dz * dz) > rf * rf) continue;
                if (dx >= -SPAN / 2 && dx <= SPAN / 2 && dz >= -SPAN / 2 && dz <= SPAN / 2 && game[dz + SPAN / 2][dx + SPAN / 2])
                    continue;
                s32 gx = cx + dx, gz = cz + dz;
                if (gx < 0 || gz < 0 || gx >= g->w || gz >= g->h) continue;
                const u16 *list = &g->lists[g->cells[g->w * gz + gx]];
                for (u32 k = 0, count = list[0]; k < count; k++) far_take(&g->objs[list[1 + k]]);
            }
    }
    /* the game's own billboards (those its search collected): it leaves out those past 1,500 units */
    for (s32 i = 0; i < mapobj_billboard_draw_count && far_billboard_n < FAR_MAX; i++) {
        far_billboard[far_billboard_n++] = mapobj_billboard_list[i];
        far_billboard_game++;
    }

    for (int i = 0; i < far_opaque_n; i++) {
        MapObj *s = far_opaque[i];
        u16 flags = s->kind;
        f32 x = s->x, y = s->y, z = s->z, rot = s->angle;
        if (flags & 0x2000) {
            /* a spline-animated object at its current frame (the game steps it when it is near) */
            f32 out[16];
            AnimNode *anim = &mapobj_loop_anims[(flags & 0xF00) >> 8];
            Math_EvalTcbSpline(&anim->track, out, flags & 0xFF);
            x += out[0] * s->scale;
            y += out[1] * s->scale;
            z += out[2] * s->scale;
        } else if (flags & 0x100) {
            s32 k = MapObjAnim_Find(s);
            if (k != 0) {
                f32 v[2] = { mapobj_anim_slots[k - 1].x, 0.0f };
                Math_RotateVec2(s->angle * 0.017453292519943295, (Vec2f *)v);
                x -= v[0];
                z -= v[1];
                rot += mapobj_anim_slots[k - 1].z;
            }
        }
        Mtx m;
        guPosition(&m, 0.0f, rot, 0.0f, s->scale, x, y, z);
        nd_map_draw(s, ND_MAP_OPAQUE, &m, (u32)(uptr)map_obj_models[s->model].dl, EL_RDX_POSITION, x, y, z, 0.0f, rot, 0.0f,
                    0.0f);
    }
}

/* After the game's billboards (MapObj_UpdateAndDrawNearby's billboard section): the ring's, and the game's past
 * 1,500 units, out to k times that (no cone: the GPU clips; no shadow) */
void nd_map_far_billboards(void) {
    if (far_k <= 1.0f || !far_billboard_n) return;
    const MapDlPair *e = map_area_def->animTexDls;
    const f32 far2 = (1500.0f * far_k) * (1500.0f * far_k);
    for (int i = 0; i < far_billboard_n; i++) {
        MapObj *s = far_billboard[i];
        if ((u16)s->model > 3) continue;
        f32 dx = s->x - camera_state.eyeX, dz = s->z - camera_state.eyeZ, d2 = dx * dx + dz * dz;
        if (d2 >= far2) continue;
        /* (the game's own nearer than 1,500 units: the game drew them, or its cone left them out) */
        if (i >= far_billboard_n - far_billboard_game && sqrtf(d2) < 1500.0f) continue;
        f32 mf[4][4];
        Mtx m;
        Math_MtxFRotScaleTrans((MtxF *)mf, 0.0f, camera_state.yaw, 0.0f, s->scale, s->x, s->y, s->z);
        guMtxF2L(mf, &m);
        nd_billboard_draw(s, (u32)(uptr)e[s->model].a, (u32)(uptr)e[s->model].b, &m, s->x, s->y, s->z, camera_state.yaw,
                          s->scale);
    }
}

/* At the start of the game's translucent objects (MapObj_DrawTranslucent): the ring's, first (they are the farthest).
 * The env alpha is the map's translucency, as the game sets it for its own; put back after when the game set none */
void nd_map_far_translucent(int game_set_env) {
    if (far_k <= 1.0f || !far_xlu_n) return;
    uint8_t before[4];
    nd_env_last(before);
    if (!game_set_env) nd_env_color(0, 0, 0, mapobj_translucent_alpha[map_id]);
    for (int i = 0; i < far_xlu_n; i++) {
        MapObj *s = far_xlu[i];
        u16 kind = (u8)s->kind;
        Mtx m;
        guPosition(&m, 0.0f, s->angle, 0.0f, s->scale, s->x, s->y, s->z);
        if (kind >= 0x40 && kind < 0x45)
            nd_map_draw(s, ND_MAP_SCROLL + (kind - 0x40), &m, (u32)(uptr)map_obj_models[s->model].dl, EL_RDX_POSITION, s->x,
                        s->y, s->z, 0.0f, s->angle, 0.0f, (f32)((mapobj_scrolltex_offset % 256) << 2));
        else
            nd_map_draw(s, ND_MAP_XLU, &m, (u32)(uptr)map_obj_models[s->model].dl, EL_RDX_POSITION, s->x, s->y, s->z, 0.0f,
                        s->angle, 0.0f, 0.0f);
    }
    if (!game_set_env) nd_env_color(before[0], before[1], before[2], before[3]);
}
