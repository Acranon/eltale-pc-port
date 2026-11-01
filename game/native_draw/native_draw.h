/* native_draw.h: the game's side of the native renderer (docs/renderer/NATIVE_DRAW.md). PC-only.
 *
 * The patched draw code (work/src, patches/p0.patch) calls in here. Per kind of draw (EL_KIND_* in
 * port/render.h) two switches:
 * - ND_ON(kind): the native renderer draws this kind: the draw site calls nd_*() with what it draws;
 * - ND_DL(kind): the kind's display-list commands are kept in the frame list. Off for a native kind
 *   (unless ELTALE_ND_COMPARE=1: both), and off for kinds ELTALE_DL_KINDS leaves out (the reference
 *   pictures of one kind alone).
 * A draw site keeps its code as it was and drops what it queued afterwards (ND_DL_BEGIN/ND_DL_END
 * rewind gfx_dl_head), so the game does exactly the same work either way, and with every kind's
 * list kept (the default, and always without the native renderer) the frame list is byte for byte
 * the original.
 *
 * State the native kinds draw with (camera, lights, colours, fog) is sent from where the game queues
 * it, whenever any kind is native (nd_kinds != 0). */
#ifndef NATIVE_DRAW_H
#define NATIVE_DRAW_H

#include "port/render.h"

#ifdef ELTALE_P1_OS

extern unsigned nd_kinds;    /* native kinds: bits 1 << EL_KIND_* (0 without the native renderer) */
extern unsigned nd_dl_kinds; /* kinds whose display-list commands stay in the frame list */

#define ND_ON(kind) (nd_kinds & (1u << (kind)))
#define ND_DL(kind) (nd_dl_kinds & (1u << (kind)))
#define ND_ANY() (nd_kinds != 0)
/* the commands a section queued are dropped when its kind's list is off */
#define ND_DL_BEGIN(mark) Gfx *mark = gfx_dl_head
#define ND_DL_END(kind, mark) do { if (!ND_DL(kind)) gfx_dl_head = (mark); } while (0)

struct GfxFrameData;
struct MapObj;

/* Start-up: the switches from the command line and the environment (call once, before the game). */
void nd_config(int argc, char **argv);
/* The native renderer is up: its functions, and which kinds it can draw. */
void nd_attach(const ElRenderApi *api);
/* After the menu's reset (the exe's data went back to the start, the renderer's did not). */
void nd_after_reset(void);
/* A ROM DMA wrote [host, host + bytes). */
void nd_forget(uintptr_t host, uint32_t bytes);

/* The tick: at the start of Gfx_BuildFrameDisplayList, and when its graphics task starts (the
 * frame's number, as P1_GFX_TASK counts them). */
void nd_frame_begin(void);
void nd_frame_end(unsigned frame);

/* State, where the game queues it */
void nd_camera(struct GfxFrameData *frame, const void *shake_mtx); /* end of Camera_Update */
void nd_lights(const void *lights3);                               /* gSPSetLights3 */
void nd_env_color(int r, int g, int b, int a);
void nd_prim_color(int r, int g, int b, int a);
void nd_fog_color(int r, int g, int b, int a);
void nd_fog_position(int near_, int far_);                         /* gSPFogPosition */

/* Map objects (EL_KIND_MAP). `pass`: which entry state the model is called with. */
enum { ND_MAP_OPAQUE = 0, ND_MAP_XLU = 1, ND_MAP_SCROLL = 2 /* + 0..4: the scrolling texture */ };
void nd_map_draw(const struct MapObj *obj, int pass, const void *mtx, uint32_t dl, int xform, float x, float y,
                 float z, float rx, float ry, float rz, float tile_t);

#else

#define ND_ON(kind) 0
#define ND_DL(kind) 1
#define ND_ANY() 0
#define ND_DL_BEGIN(mark) Gfx *mark = gfx_dl_head
#define ND_DL_END(kind, mark) (void)(mark)
#define nd_frame_begin() ((void)0)
#define nd_camera(f, s) ((void)0)
#define nd_lights(l) ((void)0)
#define nd_env_color(r, g, b, a) ((void)0)
#define nd_prim_color(r, g, b, a) ((void)0)
#define nd_fog_color(r, g, b, a) ((void)0)
#define nd_fog_position(n, f) ((void)0)
#define nd_map_draw(...) ((void)0)

#endif

#endif
