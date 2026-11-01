/* nd_effect.h: effects and the screen quads drawn natively (EL_KIND_EFFECT, EL_KIND_SCREEN; nd_effect.c,
 * docs/renderer/NATIVE_DRAW.md "Effects"). PC-only. Called from gfx/sprite_effect.c, gfx/effect.c,
 * gfx/fade.c and actor/anim.c, each right after the game queued the draw (the Mtx it loaded is the
 * matrix); the colour the game queues before a draw goes through nd_env_color where it is queued. Each
 * call costs a test when the kind is not native. */
#ifndef ND_EFFECT_H
#define ND_EFFECT_H

#include <stdint.h>

#ifdef ELTALE_P1_OS

struct EffectSprite;
struct EffectStreak;
struct EffectRing;
struct EffectModel;

/* Effect_UpdateAndDrawSprites, first: a new tick of the effect pools (birth ticks count these) */
void nd_effect_begin(void);

/* Effect_UpdateAndDrawSprites: the slot's sprite, after its matrix and list were queued */
void nd_effect_sprite(int slot, const struct EffectSprite *s, const void *mtx);

/* Effect_UpdateAndDrawStreaks: a streak (dist: how far it went, its length) */
void nd_effect_streak(int slot, const struct EffectStreak *p, const void *mtx, float dist);

/* Effect_DrawStatUpMarkers: marker `slot` at (x, y, z), aimed at the eye, scaled */
void nd_effect_marker(int slot, int frames_left, const void *mtx, float x, float y, float z, float scale);

/* Effect_UpdateAndDrawRings: segment `seg` of ring `ring`, from (x0, y0, z0) along (dx, dy, dz) */
void nd_effect_ring(int ring, int seg, const struct EffectRing *p, const void *mtx, float x0, float y0, float z0,
                    float dx, float dy, float dz, float len);

/* Effect_UpdateAndDrawModelEffects: a model effect's state is queued (texture, combiner, env colour,
 * culling); its nodes come through nd_effect_node until nd_effect_model_end */
void nd_effect_model_begin(int slot, const struct EffectModel *p);
void nd_effect_model_end(void);

/* Anim_DrawNode, after it queued the node (as nd_actor_node): draws only inside a model effect */
void nd_effect_node(int idx, int node_flags, const void *mtx, uint32_t dl, int frame);

/* Fade_UpdateAndDraw (EL_KIND_SCREEN): the letterbox bars, and the fade quad (ortho and modelview: the
 * Mtxs it loaded; opaque: the render mode it set) */
void nd_screen_letterbox(void);
void nd_screen_fade(const void *ortho, const void *mv, int opaque);

#endif

#endif
