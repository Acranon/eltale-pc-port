#ifndef PORT_GFX_H
#define PORT_GFX_H

#include "ultra64.h"

typedef struct PortFrameStats {
    u32 commands, matrices, vertices, textures, native_bytes, unknown;
    u32 unknown_words[4]; /* the first unresolved address words */
    u8 unknown_ops[4]; /* and their commands */
} PortFrameStats;

/* port/gfx/frame_image.c: the N64-format RDRAM image of one graphics task. `dl_word` is the
 * task's display-list pointer; *root_n64 receives its N64 address. The image stays valid until
 * the next call. */
const u8 *Port_BuildFrameImage(u32 dl_word, u32 *root_n64, PortFrameStats *stats);

/* The image of a screen the CPU drew into `fb` (native u16 pixels, 320x240), with a display list
 * (*root_n64) that copies it into `target`. */
const u8 *Port_BuildCpuFrameImage(u32 fb, u32 target, u32 *root_n64);

/* A Port_GfxTaskHook that builds each frame's image and logs its statistics. */
void Port_FrameImageTrace(OSTask *task, unsigned frame);

/* VI registers for a framebuffer (port/gfx/vi.c); 0 until the game has set a mode. */
typedef struct PortViRegs {
    u32 status, origin, width, intr, v_current, timing, v_sync, h_sync, leap, h_start, v_start, v_burst,
        x_scale, y_scale;
} PortViRegs;
int Port_ViRegs(u32 framebuffer, PortViRegs *regs);

/* port/gfx/rt64_bridge.c: draw with eltale_rt64 (a window). Returns 0 on success. */
int Port_Rt64Start(void);
/* After the scheduler stopped: 2 if the menu asked for a reset, 3 for a reset into the Controller
 * Pak manager, else 0. */
int Port_Rt64TakeReset(void);
/* The data folder for saves and settings (eltale_rt64's ElRt64_DataDir). */
const char *Port_Rt64DataDir(void);
/* The quick-save key pressed since the last call: 1 F2 (save), 2 F4 (load), 0 none or no window. */
int Port_Rt64TakeQuickKey(void);
/* Show a short message over the game for a few seconds (nothing without the window). */
void Port_Rt64Notice(const char *text);

#endif
