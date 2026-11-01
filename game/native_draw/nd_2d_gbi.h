/* nd_2d_gbi.h: the gbi macros a 2D draw file uses, each also telling the native 2D core (nd_2d.h) the
 * values it was given. PC-only; include it after every other header in a 2D file (hud.c, text.c, ...).
 *
 * The macros queue exactly the commands they always did (the bodies are the SDK's, from
 * out/<build>/include/PR/gbi.h); while the native renderer is on (ND2D_TRACK) they also call the core.
 * Only the macros everything else is built from are wrapped (othermodes, colours, combiner, texture
 * image, tiles, loads, TLUT, matrices, display lists, rectangles), so gDPLoadTextureTile,
 * gDPLoadTLUT_pal256, gDPSetRenderMode and the rest reach the core through them. Code that writes
 * command words itself calls the core's functions next to them. */
#ifndef ND_2D_GBI_H
#define ND_2D_GBI_H

#ifdef ELTALE_P1_OS

#include "game/native_draw/nd_2d.h"

#undef gSPSetOtherMode
#define gSPSetOtherMode(pkt, cmd, sft, len, data)                                                          \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(cmd, 24, 8) | _SHIFTL(sft, 8, 8) | _SHIFTL(len, 0, 8));                   \
        _g->words.w1 = (unsigned int)(data);                                                              \
        if (ND2D_TRACK()) nd_2d_othermode((cmd), (sft), (len), (unsigned int)(data));                     \
    }

#undef gDPSetColor
#define gDPSetColor(pkt, c, d)                                                                             \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(c, 24, 8);                                                                 \
        _g->words.w1 = (unsigned int)(d);                                                                 \
        if (ND2D_TRACK()) nd_2d_color((c), (unsigned int)(d));                                            \
    }

#undef gDPSetPrimColor
#define gDPSetPrimColor(pkt, m, l, r, g, b, a)                                                             \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_SETPRIMCOLOR, 24, 8) | _SHIFTL(m, 8, 8) | _SHIFTL(l, 0, 8));            \
        _g->words.w1 = (_SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8));     \
        if (ND2D_TRACK()) nd_2d_prim_color((m), (l), _g->words.w1);                                       \
    }

#undef gDPSetCombine
#define gDPSetCombine(pkt, muxs0, muxs1)                                                                   \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(G_SETCOMBINE, 24, 8) | _SHIFTL(muxs0, 0, 24);                              \
        _g->words.w1 = (unsigned int)(muxs1);                                                             \
        if (ND2D_TRACK()) nd_2d_combine(_SHIFTL(muxs0, 0, 24), (unsigned int)(muxs1));                    \
    }

#undef gSetImage
#define gSetImage(pkt, cmd, fmt, siz, width, i)                                                            \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        uintptr_t _nd_i = (uintptr_t)(i);                                                                 \
        _g->words.w0 = _SHIFTL(cmd, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) |                  \
                       _SHIFTL((width) - 1, 0, 12);                                                       \
        _g->words.w1 = (unsigned int)(uintptr_t)(_nd_i);                                                  \
        if (ND2D_TRACK() && (cmd) == G_SETTIMG) nd_2d_texture_image((fmt), (siz), (width), _nd_i);        \
    }

#undef gDPSetTile
#define gDPSetTile(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts)       \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(G_SETTILE, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) |            \
                       _SHIFTL(line, 9, 9) | _SHIFTL(tmem, 0, 9);                                         \
        _g->words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL(palette, 20, 4) | _SHIFTL(cmt, 18, 2) |             \
                       _SHIFTL(maskt, 14, 4) | _SHIFTL(shiftt, 10, 4) | _SHIFTL(cms, 8, 2) |              \
                       _SHIFTL(masks, 4, 4) | _SHIFTL(shifts, 0, 4);                                      \
        if (ND2D_TRACK())                                                                                 \
            nd_2d_tile((fmt), (siz), (line), (tmem), (tile), (palette), (cmt), (maskt), (shiftt), (cms),   \
                       (masks), (shifts));                                                                \
    }

#undef gDPLoadTileGeneric
#define gDPLoadTileGeneric(pkt, c, tile, uls, ult, lrs, lrt)                                               \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(c, 24, 8) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12);                    \
        _g->words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL(lrs, 12, 12) | _SHIFTL(lrt, 0, 12);                 \
        if (ND2D_TRACK()) {                                                                               \
            if ((c) == G_LOADTILE) nd_2d_load_tile((tile), (uls), (ult), (lrs), (lrt));                   \
            else nd_2d_tile_size((tile), (uls), (ult), (lrs), (lrt));                                     \
        }                                                                                                 \
    }

#undef gDPLoadBlock
#define gDPLoadBlock(pkt, tile, uls, ult, lrs, dxt)                                                        \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_LOADBLOCK, 24, 8) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12));         \
        _g->words.w1 = (_SHIFTL(tile, 24, 3) | _SHIFTL((MIN(lrs, G_TX_LDBLK_MAX_TXL)), 12, 12) |          \
                        _SHIFTL(dxt, 0, 12));                                                             \
        if (ND2D_TRACK()) nd_2d_load_block((tile), (uls), (ult), MIN(lrs, G_TX_LDBLK_MAX_TXL), (dxt));     \
    }

#undef gDPLoadTLUTCmd
#define gDPLoadTLUTCmd(pkt, tile, count)                                                                   \
    {                                                                                                     \
        Gfx *_g = (Gfx *)pkt;                                                                             \
        _g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8);                                                        \
        _g->words.w1 = _SHIFTL((tile), 24, 3) | _SHIFTL((count), 14, 10);                                 \
        if (ND2D_TRACK()) nd_2d_load_tlut((tile), (count));                                               \
    }

#undef gSPTexture
#define gSPTexture(pkt, s, t, level, tile, on)                                                             \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_TEXTURE, 24, 8) | _SHIFTL(BOWTIE_VAL, 16, 8) | _SHIFTL(level, 11, 3) |  \
                        _SHIFTL(tile, 8, 3) | _SHIFTL(on, 0, 8));                                         \
        _g->words.w1 = (_SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16));                                          \
        if (ND2D_TRACK()) nd_2d_texture((s), (t), (level), (tile), (on));                                 \
    }

#undef gSPSetGeometryMode
#define gSPSetGeometryMode(pkt, word)                                                                      \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(G_SETGEOMETRYMODE, 24, 8);                                                 \
        _g->words.w1 = (unsigned int)(word);                                                              \
        if (ND2D_TRACK()) nd_2d_geometry(0, (unsigned int)(word));                                        \
    }

#undef gSPClearGeometryMode
#define gSPClearGeometryMode(pkt, word)                                                                    \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = _SHIFTL(G_CLEARGEOMETRYMODE, 24, 8);                                               \
        _g->words.w1 = (unsigned int)(word);                                                              \
        if (ND2D_TRACK()) nd_2d_geometry((unsigned int)(word), 0);                                        \
    }

/* gSPDisplayList and gSPMatrix (F3DEX: G_DL, G_MTX). The address is taken once: a matrix's is often
 * &mtx[gfx_mtx_index++]. */
#undef gDma1p
#define gDma1p(pkt, c, s, l, p)                                                                            \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        uintptr_t _nd_s = (uintptr_t)(s);                                                                 \
        _g->words.w0 = (_SHIFTL((c), 24, 8) | _SHIFTL((p), 16, 8) | _SHIFTL((l), 0, 16));                 \
        _g->words.w1 = (unsigned int)(uintptr_t)(_nd_s);                                                  \
        if (ND2D_TRACK()) {                                                                               \
            if ((c) == G_DL) nd_2d_display_list(_nd_s);                                                   \
            else if ((c) == G_MTX) nd_2d_matrix(_nd_s, (p));                                              \
        }                                                                                                 \
    }

/* A display list the code built itself this frame (the intro's sprite list): its commands reached the
 * core as they were queued, so the call is not followed */
#define gSPDisplayListBuilt(pkt, dl)                                                                       \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_DL, 24, 8) | _SHIFTL(G_DL_PUSH, 16, 8) | _SHIFTL(0, 0, 16));            \
        _g->words.w1 = (unsigned int)(uintptr_t)(dl);                                                     \
    }

/* For code that writes command words itself (the overlays' GFX_WORDS and GFX_TEXRECT_STEPS) */
#define ND2D_WORDS(a, b) if (ND2D_TRACK()) nd_2d_words((a), (b));
#define ND2D_TEXRECT(xl, yl, xh, yh, tile, s, t, dsdx, dtdy)     if (ND2D_TRACK()) nd_2d_texrect((xl), (yl), (xh), (yh), (tile), (s), (t), (dsdx), (dtdy));

#undef gSPTextureRectangle
#define gSPTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)                                   \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_TEXRECT, 24, 8) | _SHIFTL(xh, 12, 12) | _SHIFTL(yh, 0, 12));            \
        _g->words.w1 = (_SHIFTL(tile, 24, 3) | _SHIFTL(xl, 12, 12) | _SHIFTL(yl, 0, 12));                 \
        gImmp1(pkt, G_RDPHALF_1, (_SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16)));                               \
        gImmp1(pkt, G_RDPHALF_2, (_SHIFTL(dsdx, 16, 16) | _SHIFTL(dtdy, 0, 16)));                         \
        if (ND2D_TRACK()) nd_2d_texrect((xl), (yl), (xh), (yh), (tile), (s), (t), (dsdx), (dtdy));        \
    }

#undef gDPFillRectangle
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry)                                                          \
    {                                                                                                     \
        Gfx *_g = (Gfx *)(pkt);                                                                           \
        _g->words.w0 = (_SHIFTL(G_FILLRECT, 24, 8) | _SHIFTL((lrx), 14, 10) | _SHIFTL((lry), 2, 10));     \
        _g->words.w1 = (_SHIFTL((ulx), 14, 10) | _SHIFTL((uly), 2, 10));                                  \
        if (ND2D_TRACK()) nd_2d_fillrect((ulx), (uly), (lrx), (lry));                                     \
    }

#else

#define ND2D_WORDS(a, b)
#define ND2D_TEXRECT(xl, yl, xh, yh, tile, s, t, dsdx, dtdy)
#define gSPDisplayListBuilt(pkt, dl) gSPDisplayList(pkt, dl)

#endif

#endif
