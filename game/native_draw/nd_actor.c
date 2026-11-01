/* nd_actor.c: actor models drawn natively (EL_KIND_ACTOR; docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * The player, NPCs, chests, enemies and the field boss are all drawn by one walker, anim.c's
 * Anim_DrawModel / Anim_DrawNode: an AnimSet's node tree, a CPU matrix per node (the actor's
 * Math_MtxFRotScaleTrans times the node chain at the TCB frame; billboard nodes aimed at the eye),
 * and one display list per node, or a dlSeq flipbook picked by Actor.ticks. That walker stays the
 * game's: each node it queues comes here as (the very Mtx it loaded, the list it called), plus the
 * actor and the inputs of the matrix for R6. A list is a mesh once (nd_model.c), per entry state.
 *
 * The entry state. Actor model lists set their own combiner, render mode, geometry mode, TEXTURE,
 * tiles, texture loads and PRIM (frame images of New Game, the map tour, the ending, a battle and the
 * menus: no triangle reads a PRIM, texture or combiner its list did not set). What they take from the
 * frame list is othermode H: 2-cycle (the frame), the player's setup list D_10000A0 (texture
 * perspective, bilinear, no TLUT, ...: the same as the map's D_10000E8) and the texture filter the
 * draw functions set (Npc_DrawAll: point-sampled beyond ~141 units). So two entry lists, bilinear and
 * point. The light slot's additive ENV flash and lights are the draw's state (Env_ApplyLightSlot calls
 * nd_env_color / nd_lights). */
#include "game/native_draw/nd_internal.h"
#include "game/native_draw/nd_actor.h"

#include "game/core/main.h"
#include "game/actor/actor.h"
#include "game/actor/anim.h"

#include <string.h>

extern u8 D_10000A0[]; /* segment 1: the player's setup list */

static Gfx entry[2][4]; /* [0] bilinear, [1] point-sampled */
static int filter_point;

static const Actor *cur_actor;
static const AnimSet *cur_set;
static int cur_flags;
static int active;

void nd_actor_attach(void) {
    for (int k = 0; k < 2; k++) {
        Gfx *g = entry[k];
        gDPSetCycleType(g++, G_CYC_2CYCLE);
        gSPDisplayList(g++, D_10000A0);
        if (k) gDPSetTextureFilter(g++, G_TF_POINT);
        gSPEndDisplayList(g++);
    }
}

void nd_actor_filter(int filter) { filter_point = filter == G_TF_POINT; }

void nd_actor_begin(const Actor *actor, const AnimSet *set, int flags) {
    if (!ND_ON(EL_KIND_ACTOR)) return;
    cur_actor = actor;
    cur_set = set;
    cur_flags = flags;
    active = 1;
}

void nd_actor_end(void) { active = 0; }

void nd_actor_node(int idx, int node_flags, const void *mtx, uint32_t dl, int frame, int ticks) {
    if (!active || !dl) return;
    const NdMesh *mesh = nd_model_get(dl, entry[filter_point]);
    if (!mesh) return;
    nd_model_use(mesh);
    ElRdDraw d;
    memset(&d, 0, sizeof d);
    d.id = (uint64_t)EL_KIND_ACTOR << EL_RDD_KIND_SHIFT | (uint64_t)(idx & 0xFF) << 48 |
           ((uint64_t)(uintptr_t)cur_actor & 0xFFFFFFFFFFFFull);
    d.mesh = nd_model_id(mesh);
    nd_decode_mtx(mtx, d.matrix);
    d.xform = EL_RDX_ACTOR_NODE;
    d.pos[0] = cur_actor->x; d.pos[1] = cur_actor->y; d.pos[2] = cur_actor->z;
    d.rot[0] = cur_actor->rotX; d.rot[1] = cur_actor->rot; d.rot[2] = cur_actor->rotZ;
    d.scale = cur_actor->scale;
    d.inputs[EL_RDI_ANIM_SET] = (uint32_t)(uintptr_t)cur_set;
    d.inputs[EL_RDI_FRAME] = (uint32_t)(frame & 0xFFFF) | (uint32_t)(ticks & 0xFFFF) << 16;
    d.inputs[EL_RDI_NODE] = (uint32_t)(idx & 0xFF) | (uint32_t)(node_flags & 0xFF) << 8 | (uint32_t)(cur_flags & 0xFF) << 16;
    d.inputs[EL_RDI_DL] = dl;
    nd_api()->draw(&d);
}
