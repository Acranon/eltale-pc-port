/* P0 links the game modules so their data and native calls are available.
 * These later-stage SDK/audio paths must fail visibly if the first intro
 * display-list path unexpectedly reaches one. Their real ports belong in
 * the OS, math and audio implementations that follow this boot milestone. */
__attribute__((noreturn)) void Port_HostFatal(const char *what);

#define P0_UNAVAILABLE(name) \
    __attribute__((noreturn)) void name(void) { Port_HostFatal(#name " reached before its native port"); }

#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(Audio_GetSeqState)
P0_UNAVAILABLE(Audio_LoadSeq)
P0_UNAVAILABLE(Audio_PlaySeq)
P0_UNAVAILABLE(Audio_SetSeqVolume)
P0_UNAVAILABLE(Audio_StartSeqFadeOut)
P0_UNAVAILABLE(Audio_StopSeq)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(Gfx_WaitForFrameMessage)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(guLookAtReflectF)
P0_UNAVAILABLE(guMtxCatF)
P0_UNAVAILABLE(guMtxF2L)
P0_UNAVAILABLE(guMtxIdent)
P0_UNAVAILABLE(guMtxXFMF)
P0_UNAVAILABLE(guOrtho)
P0_UNAVAILABLE(guPerspectiveF)
P0_UNAVAILABLE(guPosition)
P0_UNAVAILABLE(guTranslate)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(Sfx_Enqueue)
P0_UNAVAILABLE(Sfx_StopChannel4)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(sins)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(func_8003D0F0)
P0_UNAVAILABLE(func_8003D554)
P0_UNAVAILABLE(func_8003D6B0)
P0_UNAVAILABLE(Pak_AllocateGameFile)
P0_UNAVAILABLE(Pak_DeleteGameFile)
P0_UNAVAILABLE(Pak_ReadFile)
P0_UNAVAILABLE(Pak_TryRepair)
P0_UNAVAILABLE(Pak_WriteFile)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(func_8020C11C)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(func_8020C15C)
#endif
#ifndef ELTALE_P1_OS
P0_UNAVAILABLE(Sfx_StopAllChannels)
#endif
