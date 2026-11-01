/* nd_backdrop.c: the backdrop drawn natively (EL_KIND_BACKDROP; docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * Map_DrawBackdrop (maps with map_env_flags & 4) draws the sky and far ground: a 16-sided cylinder of
 * radius 300 in segment 1 (map_backdrop_defs[].dl_addr: 0x01001300, 0x01002410 or 0x01003630), its
 * matrix a translation to the camera eye (y - 10). The list sets all its own state: 1-cycle, TEX0 x ENV,
 * no Z, shade only, and loads its CI8 strips and the palette from fixed N64 addresses
 * (map_backdrop_tex 0x8007C6B0, map_backdrop_tlut 0x800806B0) that name native .bss: the loader reads
 * them through Port_N64ToNative. The palette is the day-phase fade (Env_StepPaletteFade): the loader
 * sends it again at the end of each tick when it changed. A new backdrop's texture comes by ROM DMA
 * into map_backdrop_tex, which drops the mesh (nd_forget). The ENV colour is the draw's
 * (D_80084B60..68, set just before the list; the next Env_ApplyLightSlot sets the light slot's again). */
#include "game/native_draw/nd_internal.h"

#include "game/core/main.h"

#include <string.h>

/* The frame list's state when Map_DrawBackdrop calls the list (Gfx_BuildFrameDisplayList: 2-cycle);
 * the list sets the rest itself. */
static Gfx entry_backdrop[2];

void nd_backdrop_attach(void) {
    Gfx *g = entry_backdrop;
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gSPEndDisplayList(g++);
}

/* `which`: the backdrop (env_area_settings->backdropId), the item's stable id. `mtx`: the Mtx the game
 * built; x, y, z: its inputs (Math_MtxRotScaleTrans with no rotation, scale 1), for R6. */
void nd_backdrop_draw(int which, const void *mtx, uint32_t dl, int r, int g, int b, float x, float y, float z) {
    if (nd_automap_truth()) return; /* (the map's truth view: no sky) */
    const NdMesh *mesh = nd_model_get(dl, entry_backdrop);
    if (!mesh) return;
    nd_model_use(mesh);
    nd_env_color(r, g, b, 255);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_BACKDROP << EL_RDD_KIND_SHIFT | (uint32_t)which;
    d.mesh = nd_model_id(mesh);
    nd_decode_mtx(mtx, d.matrix);
    d.xform = EL_RDX_ROT_RAD;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.scale = 1.0f;
    nd_api()->draw(&d);
}
