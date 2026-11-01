/* The window (eltale_host.dll / libeltale_host.so, port/host): loaded at run time, so the headless
 * build needs no graphics. It owns the window, input, sound, settings and the F1 menu, and the
 * renderer --renderer picks:
 * - rt64 (the default for now): each graphics task becomes a frame image (frame_image.c) that RT64
 *   draws (eltale_rt64.dll, loaded by the host), and each retrace shows the framebuffer the VI
 *   points at;
 * - native: our own renderer (port/render). In R0 it draws nothing of the game's (a clear colour)
 *   and shows the screens the CPU draws (the Controller Pak manager) as they are. */
#include "port/gfx.h"
#include "port/host/eltale_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern void (*Port_GfxTaskHook)(OSTask *task, unsigned frame);
extern void (*Port_ViHook)(u32 framebuffer);
extern void (*Port_AiBufferHook)(void *buffer, u32 bytes, u32 rate);
extern void (*Port_LivePad)(u16 *buttons, s8 *stick_x, s8 *stick_y);
void Port_OsRequestStop(void);

static int renderer = EL_RENDERER_RT64;
static int (*h_init)(const char *, int, int, int);
static void (*h_draw)(const uint8_t *, uint32_t, uint32_t, uint32_t);
static void (*h_keep)(uint32_t, uint32_t);
static void (*h_present)(const ElHostVi *);
static void (*h_present_cpu)(const uint16_t *, int, int);
static int (*h_poll)(void);
static const char *(*h_data_dir)(void);
static int (*h_language)(void);
static int (*h_text_font)(void);
static int (*h_take_quick_key)(void);
static void (*h_notice)(const char *);
static void (*h_pad)(uint16_t *, int8_t *, int8_t *);
static void (*h_audio)(const int16_t *, uint32_t, uint32_t);
static void (*h_shutdown)(void);
static int quick_key;
static unsigned quick_key_age; /* retraces since the key was pressed */
static int reset_request;

static void play_audio(void *buffer, u32 bytes, u32 rate) { h_audio(buffer, bytes, rate); }

static void draw_task(OSTask *task, unsigned frame) {
    (void)frame;
    PortFrameStats st;
    u32 root;
    const u8 *image = Port_BuildFrameImage((u32)(uptr)task->t.data_ptr, &root, &st);
    h_draw(image, (u32)(uptr)task->t.ucode, (u32)(uptr)task->t.ucode_data, root);
}

/* The game's two framebuffers (Gfx_InitFrames) and its F3DEX ucode (Gfx_InitFrameTask). */
enum { GAME_FB0 = 0x803B5000, GAME_FB1 = 0x803DA800, UCODE = 0x8004BA50, UCODE_DATA = 0x80070260 };

static void present(u32 framebuffer) {
    PortViRegs r;
    /* any other framebuffer is a screen the CPU drew (the Controller Pak manager, before the game's
     * frames exist): native pixels, 320x240 RGBA5551 */
    int cpu = framebuffer && framebuffer != GAME_FB0 && framebuffer != GAME_FB1;
    if (cpu && renderer == EL_RENDERER_NATIVE) {
        h_present_cpu((const uint16_t *)(uptr)framebuffer, 320, 240);
    } else {
        if (cpu) { /* RT64 draws it into the first game framebuffer, from a copy display list, and shows that */
            u32 root;
            h_draw(Port_BuildCpuFrameImage(framebuffer, GAME_FB0, &root), UCODE, UCODE_DATA, root);
            framebuffer = GAME_FB0;
        }
        if (Port_ViRegs(framebuffer, &r)) {
            ElHostVi vi = { r.status, r.origin, r.width, r.intr, r.v_current, r.timing, r.v_sync, r.h_sync, r.leap,
                            r.h_start, r.v_start, r.v_burst, r.x_scale, r.y_scale };
            h_present(&vi);
        }
    }
    int status = h_poll();
    if (status != EL_POLL_RUN) {
        reset_request = status >= EL_POLL_RESET ? status : 0;
        Port_OsRequestStop();
    }
    /* F2/F4: the game takes the key at the end of a gameplay or title frame (Port_QuickFrame); one
     * not taken soon was pressed somewhere else (a menu, a scene) */
    int key = h_take_quick_key();
    if (key) quick_key = key, quick_key_age = 0;
    if (quick_key && ++quick_key_age > 8) {
        Port_WindowNotice(quick_key == 1 ? "Quick save: only in play" : "Quick load: only in play or on the title screen");
        quick_key = 0;
    }
}

int Port_WindowTakeReset(void) {
    int r = reset_request;
    reset_request = 0;
    return r;
}

const char *Port_WindowDataDir(void) { return h_data_dir ? h_data_dir() : "."; }

/* The menu's text language (1 English, 0 Japanese), or -1 without the window. */
int Port_WindowLanguage(void) { return h_language ? h_language() : -1; }

/* The menu's English font (0 the game's own), 0 without the window. */
int Port_WindowTextFont(void) { return h_text_font ? h_text_font() : 0; }

int Port_WindowTakeQuickKey(void) {
    int key = quick_key;
    quick_key = 0;
    return key;
}

void Port_WindowNotice(const char *text) {
    if (h_notice) h_notice(text);
}

static void shutdown_host(void) {
    if (h_shutdown) h_shutdown();
}

int Port_WindowSetRenderer(const char *name) {
    if (!strcmp(name, "rt64")) renderer = EL_RENDERER_RT64;
    else if (!strcmp(name, "native")) renderer = EL_RENDERER_NATIVE;
    else return -1;
    return 0;
}

int Port_WindowStart(void) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("eltale_host.dll");
#define SYM(name) (void *)GetProcAddress(lib, name)
#else
    void *lib = dlopen("libeltale_host.so", RTLD_NOW);
#define SYM(name) dlsym(lib, name)
#endif
    if (!lib) {
        fprintf(stderr, "host: cannot load the eltale_host library (tools/build_host.py)\n");
        return -1;
    }
    h_init = (int (*)(const char *, int, int, int))SYM("ElHost_Init");
    h_draw = (void (*)(const uint8_t *, uint32_t, uint32_t, uint32_t))SYM("ElHost_DrawTask");
    h_keep = (void (*)(uint32_t, uint32_t))SYM("ElHost_KeepRange");
    h_present = (void (*)(const ElHostVi *))SYM("ElHost_Present");
    h_present_cpu = (void (*)(const uint16_t *, int, int))SYM("ElHost_PresentCpuFrame");
    h_poll = (int (*)(void))SYM("ElHost_Poll");
    h_data_dir = (const char *(*)(void))SYM("ElHost_DataDir");
    h_language = (int (*)(void))SYM("ElHost_Language");
    h_text_font = (int (*)(void))SYM("ElHost_TextFont");
    h_take_quick_key = (int (*)(void))SYM("ElHost_TakeQuickKey");
    h_notice = (void (*)(const char *))SYM("ElHost_Notice");
    h_pad = (void (*)(uint16_t *, int8_t *, int8_t *))SYM("ElHost_GetPad");
    h_audio = (void (*)(const int16_t *, uint32_t, uint32_t))SYM("ElHost_QueueAudio");
    h_shutdown = (void (*)(void))SYM("ElHost_Shutdown");
#undef SYM
    if (!h_init || !h_draw || !h_keep || !h_present || !h_present_cpu || !h_poll || !h_data_dir || !h_language ||
        !h_text_font || !h_take_quick_key || !h_notice || !h_pad || !h_audio || !h_shutdown) {
        fprintf(stderr, "host: eltale_host is not the one this game needs (rebuild it: tools/build_host.py)\n");
        return -1;
    }
    if (h_init("Eltale Monsters", 1280, 960, renderer) != 0) {
        fprintf(stderr, "host: setup failed\n");
        return -1;
    }
    atexit(shutdown_host);
    /* RT64: the two framebuffers and the depth buffer (Gfx_InitFrames), 320x240 16-bit each */
    if (renderer == EL_RENDERER_RT64) {
        h_keep(0x003B5000, 0x25800);
        h_keep(0x003DA800, 0x25800);
        h_keep(0x002DA800, 0x25800);
        Port_GfxTaskHook = draw_task;
    }
    Port_LivePad = (void (*)(u16 *, s8 *, s8 *))h_pad;
    Port_AiBufferHook = play_audio;
    Port_ViHook = present;
    return 0;
}
