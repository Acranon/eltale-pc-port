#include "game/audio/audio.h"
#include "game/core/main.h"

/* P0 starts the actual game boot and DMA path. Audio synthesis, PakMenu,
 * gameplay rendering and overlay execution are later port milestones. */
void Audio_Init(void) { }
void Audio_InitSeqPlayers(u8 *a, u8 *b, u8 *c, u8 *d, u8 *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void Audio_InitSoundPlayer(u8 *a, u8 *b, u8 *c) {
    (void)a; (void)b; (void)c;
}
void Audio_StartThread(NNSched *sc) { (void)sc; }
s32 func_801009EC(OSMesgQueue *a, OSMesgQueue *b) { (void)a; (void)b; return 0; }
