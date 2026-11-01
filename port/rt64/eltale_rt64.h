/* eltale_rt64.dll: RT64 behind a C interface, for the host (eltale_host.dll, port/host).
 *
 * RT64 is C++ built with clang-cl (the MSVC ABI); the host is built with MinGW Clang. The DLL keeps
 * the two apart: it owns RT64 and an RDRAM buffer, draws on the host's window (its native handle),
 * and is handed one frame image at a time (port/gfx/frame_image.c) plus the VI registers to show.
 * The window, input, sound and the F1 menu are the host's: the menu's draw lists come in as plain
 * data (ElRt64Overlay) and are drawn over RT64's picture with RT64's own ImGui backend. */
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

/* RT64's own picture options (RT64::UserConfiguration), as the host's Graphics page sets them. */
typedef struct ElRt64Graphics {
    int resolution;     /* 0 original (320x240), 1 match the window, 2 a multiple */
    int multiplier;     /* 1..8, for a multiple */
    int msaa;           /* samples: 1 (off), 2, 4, 8 */
    int filtering;      /* scaling to the window: 0 sharp pixels, 1 smooth, 2 smooth pixel edges */
    int three_point;    /* N64 three-point texture filtering */
    int refresh;        /* 0 original (30 fps), 1 match the display, 2 fixed */
    int refresh_target; /* 30..240, for fixed */
} ElRt64Graphics;

/* The menu's picture: Dear ImGui's draw lists flattened. Positions in pixels of a width x height
 * window, uv into the atlas given to ElRt64_SetOverlayAtlas, colours RGBA (ImGui's IM_COL32). */
typedef struct ElRt64OverlayVertex {
    float x, y, u, v;
    uint32_t rgba;
} ElRt64OverlayVertex;

typedef struct ElRt64OverlayCommand {
    float clip[4];        /* x0, y0, x1, y1 */
    uint32_t first_index; /* into indices */
    uint32_t index_count; /* a multiple of 3 (triangles) */
} ElRt64OverlayCommand;

typedef struct ElRt64Overlay {
    float width, height;
    const ElRt64OverlayVertex *vertices;
    const uint32_t *indices; /* into vertices */
    const ElRt64OverlayCommand *commands;
    uint32_t vertex_count, index_count, command_count;
} ElRt64Overlay;

/* Set RT64 up on the host's window: `window` is its HWND (Windows), `thread_id` the thread that
 * owns it. Returns 0 on success. */
ELRT64_API int ElRt64_Init(void *window, uint32_t thread_id, const ElRt64Graphics *graphics);

/* The menu's font atlas, RGBA8 (ImGui's), copied. Call once after ElRt64_Init. */
ELRT64_API void ElRt64_SetOverlayAtlas(const uint32_t *rgba, int width, int height);

/* Draw one graphics task. `rdram` is an 8 MiB big-endian N64 RDRAM image; `ucode`, `ucode_data`
 * and `dl` are the task's physical addresses (OSTask t.ucode, t.ucode_data, t.data_ptr). */
ELRT64_API void ElRt64_DrawTask(const uint8_t *rdram, uint32_t ucode, uint32_t ucode_data, uint32_t dl);

/* Leave [start, start + bytes) of RT64's RDRAM alone in later DrawTask calls: framebuffers and the
 * depth buffer, which RT64 renders itself. Copying the CPU's (empty) copy over them looks to RT64
 * like the CPU wrote them, and it shows that instead. Up to 8 ranges, physical addresses. */
ELRT64_API void ElRt64_KeepRange(uint32_t start, uint32_t bytes);

/* Show the frame the VI registers point at (null: the last ones again), with the overlay over it
 * (null: none). `paused`: the game is paused (the menu), so RT64 shows its last picture again. */
ELRT64_API void ElRt64_Present(const ElRt64Vi *vi, const ElRt64Overlay *overlay, int paused);

/* Change the picture options. */
ELRT64_API void ElRt64_SetGraphics(const ElRt64Graphics *graphics);

/* The MSAA sample counts the GPU can do with RT64's targets, as bits (2, 4, 8); 0 for none. */
ELRT64_API uint32_t ElRt64_MsaaSupported(void);

ELRT64_API void ElRt64_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
