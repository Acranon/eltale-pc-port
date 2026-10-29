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

#endif
