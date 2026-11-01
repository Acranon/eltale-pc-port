/* nd_shadow.c: blob shadows drawn natively (EL_KIND_SHADOW; docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * Shadow_DrawAll (gfx/shadow.c) calls the common list's setup D_1000270 once, sets the fog range
 * reversed (gSPFogPosition(far, near): the shade alpha is 1 near the camera and fades out with
 * distance), then for each queued shadow loads a matrix and draws the 4-vertex quad D_1000230 (two
 * triangles). D_1000270: a 16x16 I8 blob, 2-cycle, combiner colour 0 / alpha TEX0 x SHADE, fog then
 * alpha blend, Z compare decal without update, fog colour black, back faces culled.
 *
 * Here the quad is one mesh: a native list that calls D_1000270 and draws the quad, entered with what
 * Env_ApplyLightSlot (just before Shadow_DrawAll) leaves set: bilinear filtering and no TLUT. The fog
 * range and colour are the frame's state (sent by Shadow_DrawAll where it queues them), the matrix is
 * the one Shadow_DrawAll built. */
#include "game/native_draw/nd_internal.h"
#include "game/native_draw/nd_shadow.h"

#include "game/core/main.h"

#include <string.h>

extern Gfx D_1000270[];
extern Vtx D_1000230[];

static Gfx entry[4], model[5];

void nd_shadow_attach(void) {
    Gfx *g = entry;
    /* Gfx_BuildFrameDisplayList: 2-cycle; Env_ApplyLightSlot: bilinear, no TLUT (D_1000270 sets neither) */
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gDPSetTextureFilter(g++, G_TF_BILERP);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gSPEndDisplayList(g++);
    /* Shadow_DrawAll: the setup (its fog colour is the material's own), then one shadow's quad */
    g = model;
    gSPDisplayList(g++, D_1000270);
    gSPVertex(g++, D_1000230, 4, 0);
    gSP2Triangles(g++, 0, 1, 2, 0, 1, 3, 2, 0);
    gSPEndDisplayList(g++);
}

void nd_shadow_draw(const void *owner, int slot, const void *mtx, float x, float y, float z, float nx, float ny,
                    float nz, float scale) {
    const NdMesh *mesh = nd_model_get((uint32_t)(uintptr_t)model, entry);
    if (!mesh) return;
    nd_model_use(mesh);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_SHADOW << EL_RDD_KIND_SHIFT |
           (owner ? (uint64_t)(uintptr_t)owner & 0xFFFFFFFFFFFFull : (uint64_t)slot);
    d.mesh = nd_model_id(mesh);
    nd_decode_mtx(mtx, d.matrix);
    d.xform = EL_RDX_ALIGN_Y;
    d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
    d.rot[0] = nx; d.rot[1] = ny; d.rot[2] = nz;
    d.scale = scale;
    nd_api()->draw(&d);
}
