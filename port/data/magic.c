/* The magic and effect block (ROM 0xD490C0, DMA'd to 0x800C0000 at boot, Core_MainThread): its
 * records keep their N64 layout and are byte-swapped in place after the DMA. Its pointer tables
 * stay big-endian; native code reads them with Port_N64Ptr (game/magic/magic.h: MAGIC_SPELLS,
 * MAGIC_FX, MAGIC_PARTICLES; hud.c for the spell names).
 * - 5 spell groups (elements) of 15 spells: SpellDef (0x44), SpellFxDef (0x48), SpellParticleDef
 *   (0xC) rows, found through D_800C185C, D_800C3268 and D_800C363C;
 * - D_800C186E: the MP cost by input count, u16 [1..4] (entry 0 is the low half of
 *   D_800C185C[4] and is not read);
 * - D_800C1878: 8 launch directions (Vec3f);
 * - D_800C18D8: 100 MagicVisualDef;
 * - D_800DA0F0: 70 EffectTexAnim (img stays an N64 address);
 * - D_800EB700, D_800EB7C0: two light animations of 8 frames of 6 words.
 * The block's AnimSets (D_800EB2B4) are swapped on use like every AnimSet. */
#include "port/data.h"

#include <stdint.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

extern uint8_t D_800C185C[], D_800C186E[], D_800C1878[], D_800C18D8[], D_800C3268[], D_800C363C[],
    D_800DA0F0[], D_800EB700[], D_800EB7C0[];

static uint32_t swap_shape(uint32_t a, const char *shape) {
    for (const char *s = shape; *s; s++) {
        if (*s == 'w') { Port_Swap32(a); a += 4; }
        else if (*s == 'h') { Port_Swap16(a); a += 2; }
        else a += 1;
    }
    return a;
}

static void swap_rows(const uint8_t *table, uint32_t size, const char *shape) {
    for (int g = 0; g < 5; g++) {
        uint32_t row = (uint32_t)(uintptr_t)Port_N64Ptr(table, g);
        if (!Port_InRdram((void *)(uintptr_t)row)) Port_HostFatal("magic block: a group table is not in RDRAM");
        for (int i = 0; i < 15; i++, row += size)
            if (swap_shape(row, shape) != row + size) Port_HostFatal("magic block: record shape does not match its size");
    }
}

static void swap_words(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++) Port_Swap32((uint32_t)(uintptr_t)p + i * 4);
}

void Port_SwapMagicBlock(void) {
    swap_rows(D_800C185C, 0x44, "hhhbbbbhhhhhhhhbbwhhhhhhhhhbbbbbbbbbbbbbbbbbb");
    swap_rows(D_800C3268, 0x48, "hhhbbwwhhhbbwwhhhbbwwhhhbbwwbbbbbbbb");
    swap_rows(D_800C363C, 0xC, "hhhhhh");
    for (int i = 1; i <= 4; i++) Port_Swap16((uint32_t)(uintptr_t)D_800C186E + i * 2);
    swap_words(D_800C1878, 8 * 3);
    for (int i = 0; i < 100; i++) swap_shape((uint32_t)(uintptr_t)D_800C18D8 + i * 0xA, "hhhhh");
    for (int i = 0; i < 70; i++) swap_shape((uint32_t)(uintptr_t)D_800DA0F0 + i * 0x14, "hhhhwhhhh");
    swap_words(D_800EB700, 8 * 6);
    swap_words(D_800EB7C0, 8 * 6);
}
