#ifndef PORT_DATA_H
#define PORT_DATA_H

struct MapAreaDef;
struct ChestGroup;

/* port/data/map_area.c: the native copy of a map area's records, decoded from the N64 block
 * Map_LoadArea read into RDRAM. `reloaded` is nonzero right after that DMA; otherwise the
 * copy decoded last time is returned, so the game keeps its changes to it. */
struct MapAreaDef *Port_MapArea(const struct MapAreaDef *def, int reloaded);

/* The native copy of one sub-area's chests (chest_map_placements). */
struct ChestGroup *Port_ChestGroup(const struct ChestGroup *groups, int subarea);

/* The number of sub-areas in the area Port_MapArea decoded last. */
unsigned Port_MapAreaSubareaCount(void);

/* port/data/anim.c: in-place byte swapping of N64-layout records in RDRAM. */
#include <stdint.h>
void Port_DataForget(uint32_t start, uint32_t bytes);
void Port_Swap16(uint32_t addr);
void Port_Swap32(uint32_t addr);
/* Swap the AnimSet table at `table`, just loaded from [lo, hi) (and the nodes, keyframes and
 * display-list tables of the sets whose nodes came with it). */
void Port_SwapAnimSets(uint32_t table, uint32_t lo, uint32_t hi);
/* Swap one AnimSet and its nodes, keyframes and display-list table (idempotent). */
void Port_SwapAnimSet(uint32_t set);
/* Swap the display-list table a native AnimSet copy points to (its nodes already swapped). */
struct AnimSet;
void Port_SwapAnimSetDls(const struct AnimSet *set);
/* Byte-swap a host buffer record by `shape` (w: 4 bytes, h: 2, b: 1); returns the bytes covered. */
uint32_t Port_SwapRecord(void *p, const char *shape);
/* Byte-swap a native u16 array that a ROM DMA filled (palettes the game blends as numbers). */
void Port_SwapHalfwords(void *p, uint32_t bytes);
/* Swap a KfAnim record and its keyframes. */
void Port_SwapKfAnim(uint32_t anim);
/* Swap the keyframe tables the game's own AnimNodes use (after the boot copy). */
void Port_SwapNativeKeyframes(void);
/* port/data/magic.c: swap the magic block's records (after its boot DMA to 0x800C0000). */
void Port_SwapMagicBlock(void);
/* port/data/common.c: swap the common block's pointer-free tables (after its boot DMA). */
void Port_SwapCommonBlock(void);
/* port/data/enemy.c: swap N64-layout EnemyDefs (and their attack lists) in place. */
struct EnemyAreaSet;
void Port_SwapEnemyDefs(uint32_t defs, uint32_t count);
void Port_SwapAreaEnemyDefs(const struct EnemyAreaSet *set);
/* A SceneEntry and its messages (N64 layout), swapped in place. */
void Port_SwapSceneEntry(void *entry);
/* The ending's cutscene sequence table, its camera tracks and image cues. */
void Port_SwapCutsceneTable(uint32_t table, uint32_t count);
/* port/data/map_area.c: native copies of a scene's NPC table. */
struct NpcDef;
struct NpcDef *Port_NpcDefs(uint32_t addr, int32_t count);

/* Entry i of an N64 pointer table in RDRAM (big-endian 32-bit addresses), as a pointer. */
static inline void *Port_N64Ptr(const void *table, int i) {
    const unsigned char *p = (const unsigned char *)table + i * 4;
    return (void *)(uintptr_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

/* N64 big-endian fields of a record in RDRAM. */
static inline int Port_InRdram(const void *p) {
    return (uintptr_t)p >= 0x80000000u && (uintptr_t)p < 0x80800000u;
}
static inline uint16_t Port_N64U16(const void *rec, int off) {
    const unsigned char *p = (const unsigned char *)rec + off;
    return (uint16_t)(p[0] << 8 | p[1]);
}
static inline uint32_t Port_N64U32(const void *rec, int off) {
    return (uint32_t)Port_N64U16(rec, off) << 16 | Port_N64U16(rec, off + 2);
}

#endif
