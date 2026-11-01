/* nd_2d.h: the 2D core of the native renderer (docs/renderer/NATIVE_DRAW.md, "2D"). PC-only.
 *
 * The game's 2D drawing (HUD, text windows, menus, the title, intro and ending pictures) is texture
 * rectangles, fill rectangles and a few triangles under the 2D projection. Each draw site's gbi macros
 * call in here as they queue their commands (nd_2d_gbi.h wraps the macros a 2D file uses; sites that
 * write raw command words call these functions themselves), with the values the macro was given. The
 * core keeps the RDP state those values make (othermodes, combiner, colours, tiles, what TMEM was
 * loaded from, the palette), and each rectangle becomes one native item (ElRd2D) with a material made
 * from that state, in the frame's draw order. Nothing reads the display list.
 *
 * Static lists the 2D code calls (the HUD's setup and icon lists in the common block, the text glyph
 * setup) are read once into a list of these same calls and replayed from the cache; one that has
 * triangles (the compass, the element glows) becomes a mesh (nd_model.c) drawn with the 2D projection.
 *
 * The section's kind (EL_KIND_HUD, TEXT, MENU, PICTURE; nd_2d_section) says whether its items are
 * recorded: only while ND_ON(kind). The state is kept whenever the native renderer is on, so a native
 * section sees the state a section before it left (as the RDP does). */
#ifndef ND_2D_H
#define ND_2D_H

#include <stdint.h>

#include "port/render.h"

#ifdef ELTALE_P1_OS

extern unsigned nd_kinds;
#define ND2D_TRACK() (nd_kinds != 0)

struct GfxFrameData;

/* The frame: its data block (segment 2: the matrices) and the start of its recording */
void nd_2d_frame(struct GfxFrameData *frame);
/* Which kind the code that follows draws (-1: none, the 3D kinds) */
void nd_2d_section(int kind);
/* Where the next items belong on a wider screen (EL_RD2D_ANCHOR_*), and an element number for their
 * ids (stable over ticks); until the next call or section */
void nd_2d_element(int element, int anchor);
/* R7: the HUD's clip columns (Hud_DrawClippedTextureRect's hud_clip_left/right) for the current element: on a
 * picture wider than 4:3 a WORLD element's reach the picture's sides; else unchanged */
void nd_2d_clip_x(int32_t *left, int32_t *right);

/* The RDP state, as the gbi macros set it */
void nd_2d_othermode(uint32_t cmd, uint32_t shift, uint32_t len, uint32_t data);   /* G_SETOTHERMODE_H/L */
void nd_2d_color(uint32_t cmd, uint32_t value);                                     /* env, blend, fog, fill */
void nd_2d_prim_color(uint32_t m, uint32_t l, uint32_t rgba);
void nd_2d_combine(uint32_t w0, uint32_t w1);                                       /* the 24 low bits of w0 */
void nd_2d_texture_image(uint32_t fmt, uint32_t siz, uint32_t width, uintptr_t address);
void nd_2d_tile(uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t tile, uint32_t palette,
                uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts);
void nd_2d_tile_size(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt); /* 10.2 */
void nd_2d_load_tile(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
void nd_2d_load_block(uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
void nd_2d_load_tlut(uint32_t tile, uint32_t count);                                /* count: entries - 1 */
void nd_2d_texture(uint32_t s, uint32_t t, uint32_t level, uint32_t tile, uint32_t on);
void nd_2d_geometry(uint32_t clear, uint32_t set);
void nd_2d_matrix(uintptr_t address, uint32_t flags);                               /* gSPMatrix */
void nd_2d_display_list(uintptr_t address);                                         /* gSPDisplayList */

/* gDPLoadTextureBlock's calls (for code that writes the commands itself) */
void nd_2d_load_texture_block(uintptr_t timg, uint32_t fmt, uint32_t siz, uint32_t width, uint32_t height, uint32_t pal,
                              uint32_t cms, uint32_t cmt, uint32_t masks, uint32_t maskt, uint32_t shifts, uint32_t shiftt);

/* One command written as words by the code itself (a state command or a display list call) */
void nd_2d_words(uint32_t w0, uint32_t w1);

/* Drawing */
void nd_2d_texrect(int32_t xl, int32_t yl, int32_t xh, int32_t yh, uint32_t tile, int32_t s, int32_t t, int32_t dsdx,
                   int32_t dtdy); /* gSPTextureRectangle's arguments (10.2, S10.5, S5.10) */
void nd_2d_fillrect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry); /* gDPFillRectangle's (pixels) */

/* The native draw core's hooks */
void nd_2d_frame_begin(void);
void nd_2d_reset(void);
void nd_2d_forget(uintptr_t host, uint32_t bytes);

#else

#define ND2D_TRACK() 0
#define nd_2d_frame(f) ((void)0)
#define nd_2d_section(k) ((void)0)
#define nd_2d_element(e, a) ((void)0)
#define nd_2d_clip_x(l, r) ((void)0)

#endif

#endif
