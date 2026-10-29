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

/* A Port_GfxTaskHook that builds each frame's image and logs its statistics. */
void Port_FrameImageTrace(OSTask *task, unsigned frame);

#endif
