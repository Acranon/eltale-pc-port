/* The common data block (ROM D74AC0..D86170, at 0x8039ADB0 from boot on): HUD textures and
 * display lists, and tables the CPU reads. The tables without pointers have the same layout
 * natively, so they are swapped to host byte order in place, once, after Gfx_InitFrames loads
 * the block (Port_SwapCommonBlock). Pointer tables in it stay N64 data; the game reads them with
 * Port_N64Ptr / Port_N64U32 (port/data.h).
 *
 * A table runs to the next address the code names in the block. Shapes: w a 32-bit field, h a
 * 16-bit field, b a byte. */
#include "port/data.h"

#include <stdint.h>

typedef struct { uint32_t addr, size, count; const char *shape; } Table;

static const Table tables[] = {
    { 0x803AAAA0, 0x0C, 168, "hhhhbbbb" },   /* ItemDef */
    { 0x803AAA40, 0x10, 6, "hhwww" },         /* Struct_803AAA40: map, sub-area, spawn x/z/rot */
    { 0x803AB358, 0x0A, 2, "hhhhh" },         /* MagicVisualDef */
    { 0x803AA690, 0x0A, 5, "hhhhh" },         /* MagicVisualDef */
    { 0x803AB36C, 0x10, 2, "hhhhww" },        /* EnemyEffectDef */
    { 0x803AA6C4, 0x10, 9, "hhhhww" },        /* EnemyEffectDef */
    { 0x803AB3D0, 0x18, 7, "hhhhhhwww" },     /* Unk803AB3D0 */
    { 0x803AB48C, 0x04, 9, "w" },             /* f32 */
    { 0x803AB4B0, 0x38, 15, "wwwwwwwwwwwwww" },/* CameraPreset */
    { 0x803AA628, 0x08, 13, "hhhh" },         /* Struct_803AA628 */
};

void Port_SwapCommonBlock(void) {
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++) {
        const Table *tb = &tables[t];
        for (uint32_t i = 0; i < tb->count; i++) {
            uint32_t a = tb->addr + i * tb->size;
            for (const char *s = tb->shape; *s; s++) {
                if (*s == 'w') { Port_Swap32(a); a += 4; }
                else if (*s == 'h') { Port_Swap16(a); a += 2; }
                else a += 1;
            }
        }
    }
}
