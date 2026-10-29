/* The common data block (ROM D74AC0..D86170, at 0x8039ADB0 from boot on): HUD textures and
 * display lists, and tables the CPU reads. The tables without pointers have the same layout
 * natively, so they are swapped to host byte order in place, once, after Gfx_InitFrames loads
 * the block (Port_SwapCommonBlock). Pointer tables in it stay N64 data; the game reads them with
 * Port_N64Ptr / Port_N64U32 (port/data.h).
 *
 * Counts: the records up to the next address the code names, cut where the data stops being
 * that record (checked with ELTALE_DUMP_COMMON=1: past the cut come strings, vertices or other
 * records). Shapes: w a 32-bit field, h a 16-bit field, b a byte. */
#include "port/data.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Port_DumpCommonBlock(void);

typedef struct { uint32_t addr, size, count; const char *shape; } Table;

static const Table tables[] = {
    { 0x803AAAA0, 0x0C, 25, "hhhhbbbb" },    /* ItemDef: 25, then the item name strings */
    { 0x803AAA40, 0x10, 6, "hhwww" },         /* Struct_803AAA40: map, sub-area, spawn x/z/rot */
    { 0x803AB358, 0x0A, 2, "hhhhh" },         /* MagicVisualDef */
    { 0x803AA690, 0x0A, 5, "hhhhh" },         /* MagicVisualDef */
    { 0x803AB36C, 0x10, 2, "hhhhww" },        /* EnemyEffectDef */
    { 0x803AA6C4, 0x10, 5, "hhhhww" },        /* EnemyEffectDef: 5, then vertices */
    { 0x803AB3D0, 0x18, 7, "hhhhhhwww" },     /* Unk803AB3D0 */
    { 0x803AB48C, 0x04, 9, "w" },             /* f32 */
    { 0x803AB4B0, 0x38, 8, "wwwwwwwwwwwwww" }, /* CameraPreset: 8, then other records */
    { 0x803AA628, 0x08, 13, "hhhh" },         /* Struct_803AA628 */
};

void Port_SwapCommonBlock(void) {
    Port_SwapEnemyDefs(0x803AB820, 7); /* the boss defs: 0x188 bytes, exactly 7 records */
    Port_SwapEnemyDefs(0x803AB9A8, 1);
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
    if (getenv("ELTALE_DUMP_COMMON")) Port_DumpCommonBlock();
}

/* ELTALE_DUMP_COMMON=1: print the swapped tables, one record per line (w as hex and float). */
void Port_DumpCommonBlock(void) {
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t d = i < 7 ? 0x803AB820 + i * 0x38 : 0x803AB9A8;
        const unsigned char *p = (const unsigned char *)(uintptr_t)d;
        uint16_t hp, cnt; uint32_t exp, att; float scale;
        memcpy(&hp, p + 6, 2); memcpy(&exp, p + 0x10, 4); memcpy(&scale, p + 0x20, 4);
        memcpy(&cnt, p + 0x2C, 2); memcpy(&att, p + 0x30, 4);
        printf("BOSSDEF %08x max_hp=%u exp=%u scale=%g attacks=%u@%08x item=%u\n", d, hp, exp, scale, cnt, att, p[0x34]);
    }
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++) {
        const Table *tb = &tables[t];
        for (uint32_t i = 0; i < tb->count; i++) {
            uint32_t a = tb->addr + i * tb->size;
            printf("COMMON %08x[%u]", tb->addr, i);
            for (const char *s = tb->shape; *s; s++) {
                const unsigned char *p = (const unsigned char *)(uintptr_t)a;
                if (*s == 'w') { uint32_t v; float f; memcpy(&v, p, 4); memcpy(&f, p, 4); printf(" %08x(%g)", v, f); a += 4; }
                else if (*s == 'h') { uint16_t v; memcpy(&v, p, 2); printf(" %d", (int16_t)v); a += 2; }
                else { printf(" b%02x", *p); a += 1; }
            }
            printf("\n");
        }
    }
}
