/* nd_actor.h: actor models drawn natively (EL_KIND_ACTOR; nd_actor.c, docs/renderer/NATIVE_DRAW.md).
 * PC-only. Called from anim.c (Anim_DrawModel, Anim_DrawNode) and the actor draw functions (player.c,
 * npc.c, chest.c, enemy.c, boss.c). Each call costs a test when the kind is not native. */
#ifndef ND_ACTOR_H
#define ND_ACTOR_H

#include <stdint.h>

#ifdef ELTALE_P1_OS

struct Actor;
struct AnimSet;

/* The texture filter the frame list has when the next actor is drawn (G_TF_POINT or G_TF_BILERP): the
 * player's setup list and Enemy_DrawAll/Boss_Draw set bilinear, each NPC its own, chests keep the last. */
void nd_actor_filter(int filter);

/* nd_automap.c: this tick is the area map's truth view (a test, docs/AUTOMAP.md): Npc_DrawAll draws every person,
 * not only those near the player, so the map's people marks can be checked against their models */
int nd_automap_truth(void);

/* Anim_DrawModel: the actor and the AnimSet it draws (its nodes call nd_actor_node until nd_actor_end).
 * Anim_DrawNode is also the model effects' walker (effect.c): those calls come without a begin. */
void nd_actor_begin(const struct Actor *actor, const struct AnimSet *set, int flags);
void nd_actor_end(void);

/* Anim_DrawNode, after it queued the node: its index and flags, the Mtx it loaded, its display list (0:
 * none), the frame and ticks it was drawn at. */
void nd_actor_node(int idx, int node_flags, const void *mtx, uint32_t dl, int frame, int ticks);

#endif

#endif
