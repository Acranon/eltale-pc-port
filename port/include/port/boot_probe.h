#ifndef ELTALE_PORT_BOOT_PROBE_H
#define ELTALE_PORT_BOOT_PROBE_H

/* GfxFrame is a runtime struct: its host pointers make it larger than the
 * N64 record. Keep the two host frames away from fixed-address ROM buffers. */
#define D_80301000 port_gfx_frames

struct GfxFrame;
__attribute__((noreturn)) void Port_P0DisplayListReady(struct GfxFrame *frame);

/* ELTALE_START_MODE=n: the first game mode after boot (7 = the ending's camera tour), to test a
 * mode without playing to it. Called after Core_ResetGameState. */
void Port_OverrideStartMode(void);

/* ELTALE_MAP_TOUR: called once per gameplay frame. */
void Port_MapTourStep(void);

/* Quick saves (port/p0/quicksave.c): Port_QuickFrame at the end of each gameplay and title frame
 * takes F2/F4 (and the ELTALE_QUICK_*_AT tests); Port_QuickLoadApply, after the session's reset,
 * puts a requested quick load in place as the title's Continue would. */
void Port_QuickFrame(void);
void Port_QuickLoadApply(void);

#endif
