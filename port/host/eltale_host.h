/* eltale_host.dll: the PC side of the game behind a C interface (docs/renderer/HOST_LAYER.md).
 *
 * The host owns the window (SDL3), the keyboard and controllers, the sound output, the data folder
 * and settings.json, the F1 menu (Dear ImGui) and the renderer, chosen at start:
 * - EL_RENDERER_RT64: RT64 (eltale_rt64.dll, port/rt64) draws each graphics task from a frame image;
 * - EL_RENDERER_NATIVE: our own Vulkan renderer (port/render, Granite). In R0 it presents a clear
 *   colour in gameplay and the CPU-drawn screens (the Controller Pak manager) as one texture.
 *
 * It is a DLL because the menu's Reset restores the exe's writable sections (port/p0/host.c): the
 * window, sound and GPU state must live outside the exe. The exe loads it at run time
 * (port/gfx/host_bridge.c), so headless builds need no graphics. Everything here is called on the
 * game's thread. */
#ifndef ELTALE_HOST_H
#define ELTALE_HOST_H

#include <stdint.h>

#ifdef ELTALE_HOST_BUILD
#ifdef _WIN32
#define ELHOST_API __declspec(dllexport)
#else
#define ELHOST_API __attribute__((visibility("default")))
#endif
#else
#define ELHOST_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { EL_RENDERER_RT64 = 0, EL_RENDERER_NATIVE = 1 };

/* ElHost_Poll's answers */
enum { EL_POLL_QUIT = 0, EL_POLL_RUN = 1, EL_POLL_RESET = 2, EL_POLL_RESET_PAK_MANAGER = 3 };

/* The VI registers (libultra's VI manager computes them from the OSViMode and framebuffer). */
typedef struct ElHostVi {
    uint32_t status, origin, width, intr, v_current, timing, v_sync, h_sync, leap, h_start, v_start,
             v_burst, x_scale, y_scale;
} ElHostVi;

/* Open the window and the renderer. Returns 0 on success. */
ELHOST_API int ElHost_Init(const char *title, int width, int height, int renderer);

/* The renderer that is up (EL_RENDERER_*). */
ELHOST_API int ElHost_Renderer(void);

/* Draw one graphics task (RT64 only; the native renderer ignores it in R0). `rdram` is an 8 MiB
 * big-endian N64 RDRAM image; `ucode`, `ucode_data` and `dl` are the task's physical addresses. */
ELHOST_API void ElHost_DrawTask(const uint8_t *rdram, uint32_t ucode, uint32_t ucode_data, uint32_t dl);

/* RT64: leave [start, start + bytes) of its RDRAM alone in later DrawTask calls (framebuffers, Z). */
ELHOST_API void ElHost_KeepRange(uint32_t start, uint32_t bytes);

/* Show the game's frame the VI registers point at. While the menu is open the game waits in here. */
ELHOST_API void ElHost_Present(const ElHostVi *vi);

/* Show a screen the CPU drew: `pixels` are 320x240 RGBA5551 in host order (the Controller Pak
 * manager). The native renderer uploads it as one texture; RT64 is drawn through the exe's copy
 * display list instead (ElHost_DrawTask then ElHost_Present). */
ELHOST_API void ElHost_PresentCpuFrame(const uint16_t *pixels, int width, int height);

/* Handle window events. EL_POLL_RUN to carry on, EL_POLL_QUIT once the window was closed or Quit
 * chosen, EL_POLL_RESET / EL_POLL_RESET_PAK_MANAGER for the menu's resets. */
ELHOST_API int ElHost_Poll(void);

/* The data folder (saves, settings.json), UTF-8: ELTALE_DATA_DIR, else %APPDATA%\eltale. */
ELHOST_API const char *ElHost_DataDir(void);

/* The text language chosen in the menu: 1 English, 0 Japanese. */
ELHOST_API int ElHost_Language(void);

/* The English font chosen: 0 the game's own, 1.. port/text/en_fonts.h's. */
ELHOST_API int ElHost_TextFont(void);

/* The quick-save key pressed since the last call (menu closed): 1 F2, 2 F4, 0 none. */
ELHOST_API int ElHost_TakeQuickKey(void);

/* Show a short message (UTF-8) over the game for a few seconds. */
ELHOST_API void ElHost_Notice(const char *text);

/* The pad from the keyboard and the first game controller, in N64 terms: button bits as
 * OSContPad.button, stick -80..80 (y up). Call after ElHost_Poll. */
ELHOST_API void ElHost_GetPad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);

/* Play one AI buffer: interleaved 16-bit stereo, host order, at `rate` Hz. */
ELHOST_API void ElHost_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate);

ELHOST_API void ElHost_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
