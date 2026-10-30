/* Drawing with RT64 (eltale_rt64.dll / libeltale_rt64.so, port/rt64): each graphics task becomes
 * a frame image (frame_image.c) that RT64 draws, and each retrace shows the framebuffer the VI
 * points at. The library is loaded at run time, so the headless build needs no graphics. */
#include "port/gfx.h"
#include "port/rt64/eltale_rt64.h"

#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern void (*Port_GfxTaskHook)(OSTask *task, unsigned frame);
extern void (*Port_ViHook)(u32 framebuffer);
void Port_OsRequestStop(void);

static int (*rt_init)(const char *, int, int);
static void (*rt_draw)(const uint8_t *, uint32_t, uint32_t, uint32_t);
static void (*rt_present)(const ElRt64Vi *);
static int (*rt_poll)(void);
static const char *(*rt_data_dir)(void);
static int reset_request;
static void (*rt_keep)(uint32_t, uint32_t);
static void (*rt_pad)(uint16_t *, int8_t *, int8_t *);
static void (*rt_audio)(const int16_t *, uint32_t, uint32_t);
extern void (*Port_AiBufferHook)(void *buffer, u32 bytes, u32 rate);

static void play_audio(void *buffer, u32 bytes, u32 rate) { rt_audio(buffer, bytes, rate); }
extern void (*Port_LivePad)(u16 *buttons, s8 *stick_x, s8 *stick_y);

static void draw_task(OSTask *task, unsigned frame) {
    (void)frame;
    PortFrameStats st;
    u32 root;
    const u8 *image = Port_BuildFrameImage((u32)(uptr)task->t.data_ptr, &root, &st);
    rt_draw(image, (u32)(uptr)task->t.ucode, (u32)(uptr)task->t.ucode_data, root);
}

/* The game's two framebuffers (Gfx_InitFrames) and its F3DEX ucode (Gfx_InitFrameTask). */
enum { GAME_FB0 = 0x803B5000, GAME_FB1 = 0x803DA800, UCODE = 0x8004BA50, UCODE_DATA = 0x80070260 };

static void present(u32 framebuffer) {
    PortViRegs r;
    /* any other framebuffer is a screen the CPU drew (the Controller Pak manager, before the game's
     * frames exist): draw it into the first game framebuffer and show that */
    if (framebuffer && framebuffer != GAME_FB0 && framebuffer != GAME_FB1) {
        u32 root;
        rt_draw(Port_BuildCpuFrameImage(framebuffer, GAME_FB0, &root), UCODE, UCODE_DATA, root);
        framebuffer = GAME_FB0;
    }
    if (Port_ViRegs(framebuffer, &r)) {
        ElRt64Vi vi = { r.status, r.origin, r.width, r.intr, r.v_current, r.timing, r.v_sync, r.h_sync, r.leap,
                        r.h_start, r.v_start, r.v_burst, r.x_scale, r.y_scale };
        rt_present(&vi);
    }
    int status = rt_poll();
    if (status != 1) {
        reset_request = status >= 2 ? status : 0;
        Port_OsRequestStop();
    }
}

int Port_Rt64TakeReset(void) {
    int r = reset_request;
    reset_request = 0;
    return r;
}

const char *Port_Rt64DataDir(void) { return rt_data_dir ? rt_data_dir() : "."; }

int Port_Rt64Start(void) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("eltale_rt64.dll");
#define SYM(name) (void *)GetProcAddress(lib, name)
#else
    void *lib = dlopen("libeltale_rt64.so", RTLD_NOW);
#define SYM(name) dlsym(lib, name)
#endif
    if (!lib) {
        fprintf(stderr, "RT64: cannot load the eltale_rt64 library (tools/build_rt64.cmd)\n");
        return -1;
    }
    rt_init = (int (*)(const char *, int, int))SYM("ElRt64_Init");
    rt_draw = (void (*)(const uint8_t *, uint32_t, uint32_t, uint32_t))SYM("ElRt64_DrawTask");
    rt_present = (void (*)(const ElRt64Vi *))SYM("ElRt64_Present");
    rt_poll = (int (*)(void))SYM("ElRt64_Poll");
    rt_keep = (void (*)(uint32_t, uint32_t))SYM("ElRt64_KeepRange");
    rt_pad = (void (*)(uint16_t *, int8_t *, int8_t *))SYM("ElRt64_GetPad");
    rt_audio = (void (*)(const int16_t *, uint32_t, uint32_t))SYM("ElRt64_QueueAudio");
    rt_data_dir = (const char *(*)(void))SYM("ElRt64_DataDir");
    if (!rt_init || !rt_draw || !rt_present || !rt_poll || rt_init("Eltale Monsters", 1280, 960) != 0) {
        fprintf(stderr, "RT64: setup failed\n");
        return -1;
    }
    /* the two framebuffers and the depth buffer (Gfx_InitFrames), 320x240 16-bit each */
    if (rt_keep) {
        rt_keep(0x003B5000, 0x25800);
        rt_keep(0x003DA800, 0x25800);
        rt_keep(0x002DA800, 0x25800);
    }
    if (rt_pad) Port_LivePad = (void (*)(u16 *, s8 *, s8 *))rt_pad;
    if (rt_audio) Port_AiBufferHook = play_audio;
    Port_GfxTaskHook = draw_task;
    Port_ViHook = present;
    return 0;
}
