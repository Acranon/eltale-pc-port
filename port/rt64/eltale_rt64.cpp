// eltale_rt64.dll (eltale_rt64.h): RT64's HLE renderer on an SDL window, driven like the Quest 64
// recomp drives it (its src/main/rt64_render_context.cpp), from frame images built by the port.
#define ELTALE_RT64_BUILD
#define NOMINMAX // windows.h (via SDL_syswm.h) min/max macros break RT64 headers
#include "eltale_rt64.h"

#include <cstdio>
#include <cstring>
#include <memory>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include <SDL.h>
#include <SDL_syswm.h>

namespace {

constexpr uint32_t RDRAM_BYTES = 8 << 20;

uint8_t rdram[RDRAM_BYTES]; // RT64 reads RDRAM as host-order 32-bit words (byte n at n ^ 3)
uint8_t dmem[0x1000];
uint8_t imem[0x1000];
uint8_t rom_header[0x40];

unsigned int MI_INTR_REG, DPC_START_REG, DPC_END_REG, DPC_CURRENT_REG, DPC_STATUS_REG, DPC_CLOCK_REG,
    DPC_BUFBUSY_REG, DPC_PIPEBUSY_REG, DPC_TMEM_REG;
unsigned int VI_STATUS_REG, VI_ORIGIN_REG, VI_WIDTH_REG, VI_INTR_REG, VI_V_CURRENT_LINE_REG, VI_TIMING_REG,
    VI_V_SYNC_REG, VI_H_SYNC_REG, VI_LEAP_REG, VI_H_START_REG, VI_V_START_REG, VI_V_BURST_REG, VI_X_SCALE_REG,
    VI_Y_SCALE_REG;

SDL_Window *window;
struct { uint32_t start, end; } keep[8];
unsigned keep_count;
std::unique_ptr<RT64::Application> app;

void check_interrupts() {}

} // namespace

extern "C" int ElRt64_Init(const char *title, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "RT64: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) {
        std::fprintf(stderr, "RT64: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);

    RT64::Application::Core core{};
#ifdef _WIN32
    core.window = wm.info.win.window;
#else
    core.window.display = wm.info.x11.display;
    core.window.window = wm.info.x11.window;
#endif
    core.checkInterrupts = check_interrupts;
    core.HEADER = rom_header;
    core.RDRAM = rdram;
    core.DMEM = dmem;
    core.IMEM = imem;
    core.MI_INTR_REG = &MI_INTR_REG;
    core.DPC_START_REG = &DPC_START_REG;
    core.DPC_END_REG = &DPC_END_REG;
    core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    core.DPC_STATUS_REG = &DPC_STATUS_REG;
    core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    core.DPC_TMEM_REG = &DPC_TMEM_REG;
    core.VI_STATUS_REG = &VI_STATUS_REG;
    core.VI_ORIGIN_REG = &VI_ORIGIN_REG;
    core.VI_WIDTH_REG = &VI_WIDTH_REG;
    core.VI_INTR_REG = &VI_INTR_REG;
    core.VI_V_CURRENT_LINE_REG = &VI_V_CURRENT_LINE_REG;
    core.VI_TIMING_REG = &VI_TIMING_REG;
    core.VI_V_SYNC_REG = &VI_V_SYNC_REG;
    core.VI_H_SYNC_REG = &VI_H_SYNC_REG;
    core.VI_LEAP_REG = &VI_LEAP_REG;
    core.VI_H_START_REG = &VI_H_START_REG;
    core.VI_V_START_REG = &VI_V_START_REG;
    core.VI_V_BURST_REG = &VI_V_BURST_REG;
    core.VI_X_SCALE_REG = &VI_X_SCALE_REG;
    core.VI_Y_SCALE_REG = &VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration config;
    config.useConfigurationFile = false;
    app = std::make_unique<RT64::Application>(core, config);
    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
    app->enhancementConfig.f3dex.forceBranch = true;          // as the Quest 64 recomp sets it
    app->emulatorConfig.dither.postBlendNoiseNegative = true;
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = GetCurrentThreadId();
#endif
    if (app->setup(thread_id) != RT64::Application::SetupResult::Success) {
        std::fprintf(stderr, "RT64: setup failed\n");
        app.reset();
        return -1;
    }
    return 0;
}

extern "C" void ElRt64_DrawTask(const uint8_t *image, uint32_t ucode, uint32_t ucode_data, uint32_t dl) {
    if (!app) return;
    // big-endian bytes -> host-order words
    for (uint32_t i = 0; i < RDRAM_BYTES; i += 4) {
        bool kept = false;
        for (unsigned k = 0; k < keep_count; k++)
            if (i >= keep[k].start && i < keep[k].end) { i = keep[k].end - 4; kept = true; break; }
        if (kept) continue;
        uint32_t w = (uint32_t)image[i] << 24 | (uint32_t)image[i + 1] << 16 | (uint32_t)image[i + 2] << 8 | image[i + 3];
        std::memcpy(rdram + i, &w, 4);
    }
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(ucode & 0x3FFFFFF, ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, dl & 0x3FFFFFF, 0, true);
}

extern "C" void ElRt64_KeepRange(uint32_t start, uint32_t bytes) {
    if (keep_count < 8) keep[keep_count++] = { start & 0x00FFFFFC, (start & 0x00FFFFFC) + ((bytes + 3) & ~3u) };
}

extern "C" void ElRt64_Present(const ElRt64Vi *vi) {
    if (!app) return;
    VI_STATUS_REG = vi->status;
    VI_ORIGIN_REG = vi->origin;
    VI_WIDTH_REG = vi->width;
    VI_INTR_REG = vi->intr;
    VI_V_CURRENT_LINE_REG = vi->v_current;
    VI_TIMING_REG = vi->timing;
    VI_V_SYNC_REG = vi->v_sync;
    VI_H_SYNC_REG = vi->h_sync;
    VI_LEAP_REG = vi->leap;
    VI_H_START_REG = vi->h_start;
    VI_V_START_REG = vi->v_start;
    VI_V_BURST_REG = vi->v_burst;
    VI_X_SCALE_REG = vi->x_scale;
    VI_Y_SCALE_REG = vi->y_scale;
    app->updateScreen();
}

extern "C" int ElRt64_Poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
    }
    return 1;
}

extern "C" void ElRt64_Shutdown(void) {
    if (app) app->end();
    app.reset();
    if (window) SDL_DestroyWindow(window);
    window = nullptr;
    SDL_Quit();
}
