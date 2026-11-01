/* nd_billboard.c: billboards and animated-texture billboards drawn natively (EL_KIND_BILLBOARD;
 * docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * MapObj_UpdateAndDrawNearby draws the billboards (kind 0x10: trees, bushes, ...) with
 * MapObj_DrawBillboards: per type (0..3) the area's animTexDls[type] pair, the setup list once, then
 * per object a matrix that faces the camera's yaw and the quad list. MapObj_DrawTranslucent draws the
 * animated-texture billboards (kinds 0x11/0x12: the candle flames) with MapObj_DrawAnimTexBillboards:
 * mapobj_animtex_dls[kind - 0x11].a once, then per object the flipbook frame's CI8 texture
 * ((model >> 1) & 7), the matrix, the env alpha mapobj_animtex_alpha[model & 3] (the flicker) and the
 * quad list. The game keeps doing all of that (culling, the matrices, Shadow_Queue, model++); these are
 * called with what it drew.
 *
 * Meshes: a billboard's is (quad list, entry list with its setup list); an animated billboard's is
 * (quad list, entry list with its setup list and one frame's texture load): 2 kinds x 8 frames. The
 * flicker alpha is the env colour the draw site sends (nd_env_color) right before the draw: each tick's
 * own value, kept as it is (R6 must not blend it). */
#include "game/native_draw/nd_internal.h"
#include "game/native_draw/nd_billboard.h"

#include "game/core/main.h"
#include "game/map/map.h"
#include "game/ui/menu.h"

#include <stdio.h>
#include <string.h>

extern Gfx D_10000E8[];
extern u16 mapobj_animtex_width[2];

enum { PASS_BILLBOARD = 0, PASS_ANIMTEX = 1 };

/* Billboards: an entry list per setup list (the area's animTexDls[type].a), made when first drawn. The
 * list only names the setup list's address: a DMA over the area block drops the meshes made through it
 * (nd_model_forget), and the next draw walks the new contents. */
#define BB_ENTRIES 64
static struct {
    u32 setup;
    Gfx list[8];
} bb_entry[BB_ENTRIES];
static int bb_entry_count;

/* Animated billboards: [kind - 0x11][frame] */
static Gfx at_entry[2][8][16];

static void entry_head(Gfx **gp, u32 render_mode) {
    Gfx *g = *gp;
    /* Gfx_BuildFrameDisplayList: 2-cycle; MapObj_UpdateAndDrawNearby: the map's setup list */
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gSPDisplayList(g++, D_10000E8);
    gDPPipeSync(g++);
    gDPSetRenderMode(g++, render_mode, 0);
    *gp = g;
}

void nd_billboard_attach(void) {
    bb_entry_count = 0;
    for (int k = 0; k < 2; k++)
        for (int f = 0; f < 8; f++) {
            Gfx *g = at_entry[k][f];
            /* MapObj_DrawTranslucent: the translucent render mode, bilinear; then the kind's setup list
             * and the frame's texture (MapObj_DrawAnimTexBillboards) */
            entry_head(&g, 0x0C1849D8);
            gDPSetTextureFilter(g++, G_TF_BILERP);
            gSPDisplayList(g++, mapobj_animtex_dls[k].a);
            gDPLoadTextureBlock(g++, mapobj_animtex_frame_tables[k][f], G_IM_FMT_CI, G_IM_SIZ_8b, mapobj_animtex_width[k], 32,
                                0, 0, 0, 0, 0, 0, 0);
            gSPEndDisplayList(g++);
        }
}

static const void *bb_entry_for(u32 setup) {
    for (int i = 0; i < bb_entry_count; i++)
        if (bb_entry[i].setup == setup) return bb_entry[i].list;
    if (bb_entry_count == BB_ENTRIES) {
        static int said;
        if (!said++) fprintf(stderr, "native draw: billboard setup lists: more than %d\n", BB_ENTRIES);
        return NULL;
    }
    Gfx *g = bb_entry[bb_entry_count].list;
    bb_entry[bb_entry_count].setup = setup;
    /* MapObj_UpdateAndDrawNearby: the cut-out render mode; MapObj_DrawBillboards: the setup list, then
     * bilinear filtering */
    entry_head(&g, 0xC8113078);
    gSPDisplayList(g++, setup);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gSPEndDisplayList(g++);
    return bb_entry[bb_entry_count++].list;
}

static void draw(const struct MapObj *obj, int pass, const NdMesh *mesh, const void *mtx, float x, float y, float z,
                 float yaw, float scale) {
    nd_model_use(mesh);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_BILLBOARD << EL_RDD_KIND_SHIFT | (uint64_t)pass << 48 |
           ((uint64_t)(uintptr_t)obj & 0xFFFFFFFFFFFFull);
    d.mesh = nd_model_id(mesh);
    nd_decode_mtx(mtx, d.matrix);
    /* the matrix is Math_Mtx(F)RotScaleTrans(0, camera yaw, 0, scale, pos): R6 rebuilds it with the
     * interpolated camera's yaw (the billboard keeps facing the camera) */
    d.xform = EL_RDX_ROT_RAD;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.rot[1] = yaw;
    d.scale = scale;
    nd_api()->draw(&d);
}

void nd_billboard_draw(const struct MapObj *obj, uint32_t setup_dl, uint32_t quad_dl, const void *mtx, float x, float y,
                       float z, float yaw, float scale) {
    const void *entry = bb_entry_for(setup_dl);
    if (!entry) return;
    const NdMesh *mesh = nd_model_get(quad_dl, entry);
    if (mesh) draw(obj, PASS_BILLBOARD, mesh, mtx, x, y, z, yaw, scale);
}

void nd_animtex_draw(const struct MapObj *obj, int which, int frame, const void *mtx, float x, float y, float z,
                     float yaw, float scale) {
    const NdMesh *mesh = nd_model_get((u32)(uptr)mapobj_animtex_dls[which].b, at_entry[which][frame & 7]);
    if (mesh) draw(obj, PASS_ANIMTEX, mesh, mtx, x, y, z, yaw, scale);
}
