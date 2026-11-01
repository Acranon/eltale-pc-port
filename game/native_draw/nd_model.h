/* nd_model.h: the model loader (docs/renderer/NATIVE_DRAW.md, "The model loader").
 *
 * A model is a static F3DEX 1.23 display list in the ROM or a map block. The loader walks it once,
 * as the RSP and RDP would (segments, the vertex cache, the geometry mode, othermodes, combiner,
 * tiles, TMEM loads, TLUTs), from an entry state the caller gives as a short native list (what the
 * game's frame list sets before it calls the model). What comes out is a mesh: vertices, groups of
 * triangles with one material each, and the decoded textures, all uploaded to the renderer once.
 *
 * A mesh is cached by (list address, entry list) and dropped when a ROM DMA overwrites anything it was
 * made from (the list, its vertices, its textures). Palettes are not baked in: a material names a
 * palette by the memory its TLUT was loaded from, and each frame the palettes the drawn meshes use are
 * read again (the clock palette and the backdrop fade change them while the game runs). */
#ifndef ND_MODEL_H
#define ND_MODEL_H

#include <stdint.h>

#include "port/render.h"

typedef struct NdMesh NdMesh;

void nd_model_init(const ElRenderApi *api);

/* The mesh for the list at `dl` (an N64 address, segmented address or native pointer) entered with
 * the native list `entry` (NULL: none) run first. NULL when the list can't be converted. */
const NdMesh *nd_model_get(uint32_t dl, const void *entry);

/* The mesh's renderer id (ElRdDraw.mesh). */
uint32_t nd_model_id(const NdMesh *mesh);

/* The mesh is drawn this frame: its palettes are sent at nd_model_frame_end. */
void nd_model_use(const NdMesh *mesh);

/* The end of a frame: the used palettes, read from memory now, sent when they changed. */
void nd_model_frame_end(void);

/* A ROM DMA wrote [host, host + bytes): drop the meshes made from it. */
void nd_model_forget(uintptr_t host, uint32_t bytes);

/* Drop everything (the menu's reset, or the renderer's buffers are full). */
void nd_model_reset(void);

/* For other loaders (the 2D core, nd_2d.c), sharing the renderer's texel buffer and palette ids:
 * - texels (RGBA8 or palette indices, as ElRdTile) by content -> their offset; ~0u when the buffers
 *   are full (call nd_model_reset, then load again);
 * - a palette of fixed values (raw 16-bit entries) by content -> its id, sent at the frame's end;
 * ~0u when the palette ids are used up. Either palette is marked used this frame. */
uint32_t nd_model_texture(const uint32_t *texels, uint32_t count);
uint32_t nd_model_palette_values(const uint16_t *entries, int count);
/* a palette read from memory each frame (count 16-bit entries at src; native: host order) -> its id */
uint32_t nd_model_palette_source(const uint8_t *src, int native, int count);

#endif
