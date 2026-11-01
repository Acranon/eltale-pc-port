/* nd_shadow.h: blob shadows drawn natively (EL_KIND_SHADOW; nd_shadow.c, docs/renderer/NATIVE_DRAW.md).
 * PC-only. Its own header so the shadow kind adds no lines to native_draw.h. */
#ifndef ND_SHADOW_H
#define ND_SHADOW_H

#ifdef ELTALE_P1_OS

/* One shadow of Shadow_DrawAll: `owner` the object that queued it (Shadow_Queue's x: an address inside
 * the actor, chest or billboard, so a stable id; NULL: unknown, then `slot`), `mtx` the Mtx the RSP
 * loads for it, and that matrix's inputs (position, the floor's up vector, the scale). */
void nd_shadow_draw(const void *owner, int slot, const void *mtx, float x, float y, float z, float nx, float ny,
                    float nz, float scale);

#else

#define nd_shadow_draw(...) ((void)0)

#endif

#endif
