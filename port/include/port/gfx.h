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

/* port/gfx/host_bridge.c: the window (eltale_host). Pick the renderer first ("rt64" or "native";
 * -1 for neither), then start: 0 on success. */
int Port_WindowSetRenderer(const char *name);
int Port_WindowStart(void);
/* After the scheduler stopped: 2 if the menu asked for a reset, 3 for a reset into the Controller
 * Pak manager, else 0. */
int Port_WindowTakeReset(void);
/* The data folder for saves and settings (eltale_host's ElHost_DataDir). */
const char *Port_WindowDataDir(void);
/* The quick-save key pressed since the last call: 1 F2 (save), 2 F4 (load), 0 none or no window. */
int Port_WindowTakeQuickKey(void);
/* Show a short message over the game for a few seconds (nothing without the window). */
void Port_WindowNotice(const char *text);
/* The menu's text language (1 English, 0 Japanese, -1 without the window) and English font. */
int Port_WindowLanguage(void);
int Port_WindowTextFont(void);

#endif
