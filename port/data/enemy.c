/* Enemy definitions keep their N64 layout (EnemyDef.attacks is a u32 address, game/enemy/enemy.h)
 * and are swapped to host byte order in place, with their attack lists:
 * - an area's defs, after Enemy_LoadAreaSet's DMA. The set's block holds the defs, then the
 *   anim-set table, then the name table, so the count is (anim_tables - defs) / 0x38; a
 *   remainder means the layout is not what we think, and stops the port.
 * - the boss defs in the common block (Port_SwapCommonBlock): D_803AB820[7] and D_803AB9A8. */
#include "port/data.h"
#include "game/enemy/enemy.h"

#include <stdint.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

static void swap_shape(uint32_t a, const char *shape) {
    for (const char *s = shape; *s; s++) {
        if (*s == 'w') { Port_Swap32(a); a += 4; }
        else if (*s == 'h') { Port_Swap16(a); a += 2; }
        else a += 1;
    }
}

void Port_SwapEnemyDefs(uint32_t defs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t d = defs + i * 0x38;
        swap_shape(d, "hhhhhhhhwwwwwhhhhhhwbbbb");
        const EnemyDef *def = (const EnemyDef *)(uintptr_t)d;
        if (def->attacks && def->attack_count > 64) Port_HostFatal("enemy def with an implausible attack count");
        for (uint32_t k = 0; def->attacks && k < def->attack_count; k++)
            swap_shape(def->attacks + k * 0x18, "hhhhhhhbbww");
    }
}

void Port_SwapAreaEnemyDefs(const struct EnemyAreaSet *set) {
    uint32_t defs = (uint32_t)(uintptr_t)set->defs, anims = (uint32_t)(uintptr_t)set->anim_tables;
    if (anims <= defs || (anims - defs) % 0x38) Port_HostFatal("enemy area set: defs do not end at the anim tables");
    Port_SwapEnemyDefs(defs, (anims - defs) / 0x38);
}

/* A SceneEntry (N64 layout, game/event/scene.h) and its messages, swapped in place. */
void Port_SwapSceneEntry(void *entry) {
    uint32_t e = (uint32_t)(uintptr_t)entry;
    swap_shape(e, "wwwwwwwwwwwwwwwhh");
    const uint32_t *w = (const uint32_t *)entry;
    uint32_t messages = w[0x34 / 4];
    int32_t count = (int32_t)w[0x38 / 4];
    if (messages && count > 0 && count < 256)
        for (int32_t i = 0; i < count; i++) swap_shape(messages + (uint32_t)i * 0xC, "wwbbbb");
}

/* The ending's camera tour (Core_RunCameraCutsceneLoop): `count` CutsceneSequenceEntry records
 * (N64 layout, game/event/cutscene.h) at `table`, each with a camera-track KfAnim and a list of
 * image cues that ends at a cue with frame 0. Swapped in place after the block's DMA. */
void Port_SwapCutsceneTable(uint32_t table, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = table + i * 0x20;
        swap_shape(e, "wwwwwwww");
        const uint32_t *w = (const uint32_t *)(uintptr_t)e;
        Port_SwapKfAnim(w[6]);
        for (uint32_t cue = w[7], n = 0;; cue += 0x14, n++) {
            if (n > 64) Port_HostFatal("cutscene image cues: no end");
            swap_shape(cue, "hhhhhhww");
            if (*(const uint16_t *)(uintptr_t)cue == 0) break;
        }
    }
}
