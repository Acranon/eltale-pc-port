/* eltale_host.dll: the PC side of the game behind a C interface (docs/renderer/HOST_LAYER.md).
 *
 * The host owns the window (SDL3), the keyboard and controllers, the sound output, the data folder
 * and settings.json, the F1 menu (Dear ImGui) and the renderer: our own Vulkan renderer (port/render,
 * Granite; docs/renderer/NATIVE_DRAW.md), which draws the game from the recordings its draw sites make
 * (ElHost_RenderApi) and shows the CPU-drawn screens (the Controller Pak manager) as one texture.
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

/* ElHost_Poll's answers */
enum { EL_POLL_QUIT = 0, EL_POLL_RUN = 1, EL_POLL_RESET = 2, EL_POLL_RESET_PAK_MANAGER = 3 };

/* The VI registers (libultra's VI manager computes them from the OSViMode and framebuffer). */
typedef struct ElHostVi {
    uint32_t status, origin, width, intr, v_current, timing, v_sync, h_sync, leap, h_start, v_start,
             v_burst, x_scale, y_scale;
} ElHostVi;

/* Open the window and the renderer. Returns 0 on success. */
ELHOST_API int ElHost_Init(const char *title, int width, int height);

/* Show the game's newest picture (the VI registers of the frame the game shows; the renderer draws
 * from its recordings). While the menu is open the game waits in here. */
ELHOST_API void ElHost_Present(const ElHostVi *vi);

/* The game is about to sleep `ns` nanoseconds until its next event (its pacing, port/p0/host.c): the
 * native renderer shows the pictures between ticks that come due meanwhile (R6) and returns in time. */
ELHOST_API void ElHost_Wait(uint64_t ns);

/* Show a screen the CPU drew: `pixels` are 320x240 RGBA5551 in host order (the Controller Pak
 * manager), uploaded as one texture. */
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

/* The mouse camera's input since the last call (port/mouse_camera.h; docs/CONTROLS.md): the turn (dx right, dy
 * down) in mouse counts at the default sensitivity, from the mouse while the right button is held or mouse look
 * is on, and from the camera keys; wheel steps (+ zooms in); flags 1 = reset (middle button or the reset key).
 * Call after ElHost_Poll, with ElHost_GetPad. */
ELHOST_API void ElHost_GetCamera(float *dx, float *dy, int *wheel, int *flags);

/* The menus' pointer (port/mouse_menu.h; docs/CONTROLS.md "The mouse in menus") since the last call: where it is
 * on the picture shown, in the 1x picture's units (x across a picture `width` wide: 320 at 4:3 and for the
 * CPU-drawn screens, 426 at 16:9; y down 240), and flags: 1 over the picture and usable (the mouse in menus on,
 * mouse look off, the menu closed), 2 the cursor shown, 4 a left click, 8 a right click (let go without a drag),
 * 16 the left button held now, 32 the left button let go.
 * Call after ElHost_Poll, with ElHost_GetPad. */
ELHOST_API void ElHost_GetPointer(float *x, float *y, int *width, int *flags);

/* The spell UI (port/spell_ui.h; docs/SPELL_UI.md): its keys pressed since the last call (PORT_SPELLKEY_*: 1-4 the
 * crystals, 5-0 the quick bar's slots, the pages, clearing) and its switch (on: 1). Call with ElHost_GetPointer. */
ELHOST_API void ElHost_GetSpellKeys(unsigned *keys, int *on);

/* Play one AI buffer: interleaved 16-bit stereo, host order, at `rate` Hz. */
ELHOST_API void ElHost_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate);

ELHOST_API void ElHost_Shutdown(void);

/* ---- the native renderer (port/include/port/render.h, docs/renderer/NATIVE_DRAW.md) ---- */

/* The native renderer without a window (headless runs that write pictures: --dump-frames). Returns 0
 * on success. No SDL, no menu, no sound. */
ELHOST_API int ElHost_InitHeadless(void);

/* The native renderer's function table (a `const ElRenderApi *`), or null before it is up. */
ELHOST_API const void *ElHost_RenderApi(void);

/* Pictures for the compare tools: the frames `spec` names (as ELTALE_DUMP_IMAGE: "a,b,c" or
 * "first+step"; empty: all) written as dir/frame_N.png, 320x240, with the N64 look. */
ELHOST_API void ElHost_SetDump(const char *dir, const char *spec);

#ifdef __cplusplus
}
#endif

#endif
