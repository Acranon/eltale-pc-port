#include "game/audio/audio.h"
#include "game/core/main.h"
#include "game/input/input.h"
#include "overlays/intro/intro.h"
#ifdef ELTALE_P1_OS
#include "overlays/title/title.h"
#endif

/* P0 starts the actual game boot and DMA path. Audio synthesis, PakMenu,
 * gameplay rendering and overlay execution are later port milestones. */
#ifndef ELTALE_P1_AUDIO /* the real game/audio/audio.c and libultra audio (P1 --audio) */
void Audio_Init(void) { }
void Audio_InitSeqPlayers(u8 *a, u8 *b, u8 *c, u8 *d, u8 *e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void Audio_InitSoundPlayer(u8 *a, u8 *b, u8 *c) {
    (void)a; (void)b; (void)c;
}
void Audio_StartThread(NNSched *sc) { (void)sc; }
#endif
#if defined(ELTALE_P1_OS) && !defined(ELTALE_P1_AUDIO)
/* Audio synthesis belongs to P1c; keep sequence state quiescent while the
 * OS/title path runs. */
s32 Audio_GetSeqState(s32 slot) { (void)slot; return 0; }
void Audio_LoadSeq(s32 slot, s32 seq) { (void)slot; (void)seq; }
s32 Audio_PlaySeq(s32 slot) { (void)slot; return 0; }
void Audio_StopSeq(s32 slot) { (void)slot; }
s32 Audio_SetSeqVolume(s32 volume) { (void)volume; return 0; }
s32 Audio_StartSeqFadeOut(u32 frames) { (void)frames; return 0; }
void Sfx_Enqueue(s32 id, s32 volume, s32 pan) { (void)id; (void)volume; (void)pan; }
void Sfx_StopChannel4(void) { }
void Sfx_StopAllChannels(void) { }
#endif
s32 func_801009EC(OSMesgQueue *a, OSMesgQueue *b) { (void)a; (void)b; return 0; }
void func_8020C0D0(void) {
#ifdef ELTALE_P1_OS
    if (core_game_mode == 3) { Title_Init(); return; }
#endif
    Intro_Init();
}
#ifdef ELTALE_P1_OS
s32 func_8020C11C(void) { return Title_Update(); }
#endif
static unsigned intro_update_count;
s32 func_8020C16C(void) {
    s32 result = Intro_Update();
    intro_update_count++;
    return result;
}
unsigned Port_HostIntroUpdateCount(void) { return intro_update_count; }

/* The P0 host has no Controller Pak or audio output yet. The first intro
 * update treats the Pak scan as successful and emits no optional screen. */
#ifndef ELTALE_P1_OS
void Input_ReadController(s32 port) { (void)port; }
#endif
#ifndef ELTALE_P1_OS
s32 Pak_ScanGameFiles(s32 port) { (void)port; return 0; }
s32 Pak_ClassifyError(s32 error) { return error; }
#endif
void func_8003D8EC(char *format, ...) { (void)format; }
