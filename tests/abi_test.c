#include "game/core/main.h"
#include "game/map/map.h"
#include "overlays/intro/intro_data.h"

#include <stddef.h>

_Static_assert(sizeof(void *) == 8, "P0 requires a 64-bit host");
_Static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8,
               "N64 scalar widths must stay fixed");
_Static_assert(sizeof(Gfx) == 8, "F3DEX commands must be eight bytes");
_Static_assert(sizeof(Mtx) == 64, "N64 matrix packets must be 64 bytes");
_Static_assert(offsetof(GfxFrameData, dl) == 0x8A70,
               "the display-list packet region must retain its N64 offset");
_Static_assert(offsetof(ActorBattle, attack_power) == 0x84,
               "expanded status handles must not shift live battle stats");
_Static_assert(sizeof(ActorBattle) == 0x98,
               "actor battle record must keep its runtime size");
_Static_assert(sizeof(((IntroSpriteCue *)0)->texture_addr) == sizeof(void *),
               "C-defined cue pointers must survive a 64-bit host");
_Static_assert(sizeof(((MapBackdropDef *)0)->dl_addr) == 4,
               "segmented N64 display-list addresses stay 32-bit words");

int main(void) { return 0; }
