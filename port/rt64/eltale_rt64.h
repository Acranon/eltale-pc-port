/* eltale_rt64.dll: RT64 behind a C interface.
 *
 * RT64 is C++ built with clang-cl (the MSVC ABI); the game is built with MinGW Clang. The DLL
 * keeps the two apart: it owns the SDL window, RT64 and an RDRAM buffer, and the game hands it
 * one frame image at a time (port/gfx/frame_image.c) plus the VI registers to show. */
#ifndef ELTALE_RT64_H
#define ELTALE_RT64_H

#include <stdint.h>

#ifdef ELTALE_RT64_BUILD
#define ELRT64_API __declspec(dllexport)
#else
#define ELRT64_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The VI registers (libultra's VI manager computes them from the OSViMode and framebuffer). */
typedef struct ElRt64Vi {
    uint32_t status, origin, width, intr, v_current, timing, v_sync, h_sync, leap, h_start, v_start,
             v_burst, x_scale, y_scale;
} ElRt64Vi;

/* Open the window and set RT64 up. Returns 0 on success. */
ELRT64_API int ElRt64_Init(const char *title, int width, int height);

/* Draw one graphics task. `rdram` is an 8 MiB big-endian N64 RDRAM image; `ucode`, `ucode_data`
 * and `dl` are the task's physical addresses (OSTask t.ucode, t.ucode_data, t.data_ptr). */
ELRT64_API void ElRt64_DrawTask(const uint8_t *rdram, uint32_t ucode, uint32_t ucode_data, uint32_t dl);

/* Leave [start, start + bytes) of RT64's RDRAM alone in later DrawTask calls: framebuffers and the
 * depth buffer, which RT64 renders itself. Copying the CPU's (empty) copy over them looks to RT64
 * like the CPU wrote them, and it shows that instead. Up to 8 ranges, physical addresses. */
ELRT64_API void ElRt64_KeepRange(uint32_t start, uint32_t bytes);

/* Show the frame the VI registers point at. */
ELRT64_API void ElRt64_Present(const ElRt64Vi *vi);

/* Handle window events (and the menu, which pauses the game inside ElRt64_Present while it is
 * open). Returns 1 to carry on, 0 once the window was closed or Quit chosen, 2 for a reset, 3 for
 * a reset into the Controller Pak manager. */
ELRT64_API int ElRt64_Poll(void);

/* The data folder (saves, settings.json), UTF-8: ELTALE_DATA_DIR, else %APPDATA%\eltale. */
ELRT64_API const char *ElRt64_DataDir(void);

/* The pad from the keyboard and the first game controller, in N64 terms: button bits as
 * OSContPad.button, stick -80..80 (y up). Call after ElRt64_Poll. */
ELRT64_API void ElRt64_GetPad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);

/* Play one AI buffer: interleaved 16-bit stereo, host order, at `rate` Hz. */
ELRT64_API void ElRt64_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate);

ELRT64_API void ElRt64_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
