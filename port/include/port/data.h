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

/* port/data/anim.c: in-place byte swapping of N64-layout records in RDRAM. */
#include <stdint.h>
void Port_DataForget(uint32_t start, uint32_t bytes);
void Port_Swap16(uint32_t addr);
void Port_Swap32(uint32_t addr);
/* Swap the AnimSet table at `table` (and its nodes, keyframes and display-list tables). */
void Port_SwapAnimSets(uint32_t table);
/* Swap one AnimSet and its nodes, keyframes and display-list table (idempotent). */
void Port_SwapAnimSet(uint32_t set);
/* Swap the display-list table a native AnimSet copy points to (its nodes already swapped). */
struct AnimSet;
void Port_SwapAnimSetDls(const struct AnimSet *set);
/* Swap the keyframe tables the game's own AnimNodes use (after the boot copy). */
void Port_SwapNativeKeyframes(void);

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
