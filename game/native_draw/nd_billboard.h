/* nd_billboard.h: billboards and animated-texture billboards drawn natively (EL_KIND_BILLBOARD;
 * nd_billboard.c, docs/renderer/NATIVE_DRAW.md). PC-only. Called from map.c's MapObj_DrawBillboards and
 * MapObj_DrawAnimTexBillboards. */
#ifndef ND_BILLBOARD_H
#define ND_BILLBOARD_H

#include "game/native_draw/native_draw.h"

#ifdef ELTALE_P1_OS

struct MapObj;

/* A billboard (kind 0x10): the area's animTexDls[type] pair (setup list, quad list), the matrix the game
 * built (Math_MtxFRotScaleTrans(0, yaw, 0, scale, x, y, z): it faces the camera's yaw) and its inputs. */
void nd_billboard_draw(const struct MapObj *obj, uint32_t setup_dl, uint32_t quad_dl, const void *mtx, float x, float y,
                       float z, float yaw, float scale);

/* An animated-texture billboard (kinds 0x11/0x12, the candle flames): `which` 0/1 (kind - 0x11), the
 * flipbook frame (0..7) it loads, the matrix (Math_MtxRotScaleTrans(0, yaw, 0, ...)) and its inputs. The
 * flicker is the env alpha the game sets before it (nd_env_color): the tick's own, never blended. */
void nd_animtex_draw(const struct MapObj *obj, int which, int frame, const void *mtx, float x, float y, float z,
                     float yaw, float scale);

#else

#define nd_billboard_draw(...) ((void)0)
#define nd_animtex_draw(...) ((void)0)

#endif

#endif
