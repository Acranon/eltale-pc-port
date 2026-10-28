#include "game/audio/audio.h"
#include "game/core/main.h"
#include "game/input/input.h"
#include "overlays/intro/intro.h"

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
void func_8020C0D0(void) { Intro_Init(); }
static unsigned intro_update_count;
s32 func_8020C16C(void) {
    s32 result = Intro_Update();
    intro_update_count++;
    return result;
}
unsigned Port_HostIntroUpdateCount(void) { return intro_update_count; }

/* The P0 host has no Controller Pak or audio output yet. The first intro
 * update treats the Pak scan as successful and emits no optional screen. */
void Input_ReadController(s32 port) { (void)port; }
s32 Pak_ScanGameFiles(s32 port) { (void)port; return 0; }
s32 Pak_ClassifyError(s32 error) { return error; }
void func_8003D8EC(char *format, ...) { (void)format; }
