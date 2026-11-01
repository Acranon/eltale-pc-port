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
