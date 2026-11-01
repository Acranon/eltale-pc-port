// eltale_rt64.dll (eltale_rt64.h): RT64's HLE renderer on the host's window, driven like the Quest 64
// recomp drives it (its src/main/rt64_render_context.cpp), from frame images built by the port.
// The window, input, sound and menu are the host's (port/host); the menu's draw lists are drawn here
// over RT64's picture, through RT64's own ImGui setup (its Inspector) and present hook.
#define ELTALE_RT64_BUILD
#define NOMINMAX // windows.h min/max macros break RT64 headers
#include "eltale_rt64.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define HLSL_CPU
#include "hle/rt64_application.h"
#include "gui/rt64_inspector.h"
#include "rhi/rt64_render_hooks.h"
#include "imgui/imgui.h"

using namespace plume;

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

struct { uint32_t start, end; } keep[8];
unsigned keep_count;
std::unique_ptr<RT64::Application> app;
std::unique_ptr<RT64::Inspector> imgui; // only ever draws the host's overlay
std::atomic<bool> shown{false};         // RT64's present thread draws the overlay only while set

void check_interrupts() {}

// RT64 draws the overlay after the game's picture, on the frame it presents.
void draw_hook(RenderCommandList *list, RenderFramebuffer *) {
    if (shown && imgui) imgui->draw(list);
}

void deinit_hook() { imgui.reset(); }

void apply(RT64::UserConfiguration &cfg, const ElRt64Graphics &g) {
    using RC = RT64::UserConfiguration;
    cfg.resolution = RC::Resolution(std::clamp(g.resolution, 0, 2));
    cfg.resolutionMultiplier = std::clamp(g.multiplier, 1, 8);
    cfg.antialiasing = g.msaa >= 8 ? RC::Antialiasing::MSAA8X : g.msaa >= 4 ? RC::Antialiasing::MSAA4X
                     : g.msaa >= 2 ? RC::Antialiasing::MSAA2X : RC::Antialiasing::None;
    cfg.filtering = RC::Filtering(std::clamp(g.filtering, 0, 2));
    cfg.threePointFiltering = g.three_point != 0;
    cfg.refreshRate = RC::RefreshRate(std::clamp(g.refresh, 0, 2));
    cfg.refreshRateTarget = std::clamp(g.refresh_target, 30, 240);
    cfg.developerMode = false;
}

// The host's draw lists into this ImGui's foreground list (scaled from the host's window size to the
// one RT64's ImGui sees, which should be the same window)
void replay(const ElRt64Overlay &ov) {
    ImGuiIO &io = ImGui::GetIO();
    const float sx = ov.width > 0 ? io.DisplaySize.x / ov.width : 1.0f;
    const float sy = ov.height > 0 ? io.DisplaySize.y / ov.height : 1.0f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    for (uint32_t c = 0; c < ov.command_count; c++) {
        const ElRt64OverlayCommand &cmd = ov.commands[c];
        if (cmd.first_index + cmd.index_count > ov.index_count) break;
        dl->PushClipRect(ImVec2(cmd.clip[0] * sx, cmd.clip[1] * sy), ImVec2(cmd.clip[2] * sx, cmd.clip[3] * sy), false);
        const uint32_t n = cmd.index_count;
        dl->PrimReserve((int)n, (int)n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t at = ov.indices[cmd.first_index + i];
            if (at >= ov.vertex_count) continue;
            const ElRt64OverlayVertex &v = ov.vertices[at];
            dl->PrimWriteIdx((ImDrawIdx)dl->_VtxCurrentIdx);
            dl->PrimWriteVtx(ImVec2(v.x * sx, v.y * sy), ImVec2(v.u, v.v), v.rgba);
        }
        dl->PopClipRect();
    }
}

} // namespace

extern "C" int ElRt64_Init(void *window, uint32_t thread_id, const ElRt64Graphics *graphics) {
    RT64::Application::Core core{};
#ifdef _WIN32
    core.window = (HWND)window;
#else
#error "eltale_rt64: only the Windows build is done"
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
    if (graphics) {
        apply(app->userConfig, *graphics);
        app->userConfig.validate();
    }
    if (app->setup(thread_id) != RT64::Application::SetupResult::Success) {
        std::fprintf(stderr, "RT64: setup failed\n");
        app.reset();
        return -1;
    }
    // ImGui on RT64's device and swapchain, with no SDL window: it only draws the host's lists
    imgui = std::make_unique<RT64::Inspector>(app->device.get(), app->swapChain.get(), app->chosenGraphicsAPI, nullptr);
    ImGui::GetIO().IniFilename = nullptr;
    RT64::SetRenderHooks(nullptr, draw_hook, deinit_hook);
    if (graphics) ElRt64_SetGraphics(graphics); // (MSAA off where the GPU can't do it)
    return 0;
}

extern "C" void ElRt64_SetOverlayAtlas(const uint32_t *rgba, int width, int height) {
    if (!imgui || !rgba || width <= 0 || height <= 0) return;
    // The font texture RT64's ImGui backend makes (at its first frame) is the host's atlas: keep a
    // font so ImGui runs, then hand it the host's pixels instead of its own
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    atlas->Clear();
    atlas->AddFontDefault();
    atlas->Build();
    atlas->ClearTexData(); // (TexReady stays set)
    const size_t bytes = size_t(width) * size_t(height) * 4;
    atlas->TexPixelsRGBA32 = (unsigned int *)IM_ALLOC(bytes);
    std::memcpy(atlas->TexPixelsRGBA32, rgba, bytes);
    atlas->TexWidth = width;
    atlas->TexHeight = height;
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

extern "C" void ElRt64_Present(const ElRt64Vi *vi, const ElRt64Overlay *overlay, int paused) {
    if (!app) return;
    if (vi) {
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
    }
    if (imgui && overlay && overlay->command_count) {
        imgui->newFrame(app->framebufferGraphicsWorker.get());
        replay(*overlay);
        imgui->endFrame();
        shown = true;
        static bool told;
        if (!told) { // (the hidden-window test reads this)
            told = true;
            const ImGuiIO &io = ImGui::GetIO();
            std::fprintf(stderr, "[rt64] overlay drawn: %u vertices, %u commands, window %.0fx%.0f (host %.0fx%.0f)\n",
                         overlay->vertex_count, overlay->command_count, io.DisplaySize.x, io.DisplaySize.y, overlay->width,
                         overlay->height);
        }
    } else {
        shown = false;
    }
    // RT64 only presents when the picture changed: while the game is paused it is told so (its
    // debugger's pause), which makes it present the last frame again, with the overlay over it
    app->state->debuggerInspector.paused = paused != 0;
    app->updateScreen();
}

extern "C" void ElRt64_SetGraphics(const ElRt64Graphics *graphics) {
    if (!app || !graphics) return;
    RT64::UserConfiguration &cfg = app->userConfig;
    const RT64::UserConfiguration old = cfg;
    apply(cfg, *graphics);
    if (!(app->device->getCapabilities().sampleLocations && (ElRt64_MsaaSupported() & cfg.msaaSampleCount())))
        cfg.antialiasing = RT64::UserConfiguration::Antialiasing::None;
    cfg.validate();
    const bool resized = cfg.resolution != old.resolution || cfg.resolutionMultiplier != old.resolutionMultiplier;
    if (cfg.msaaSampleCount() != old.msaaSampleCount()) app->updateMultisampling();
    app->updateUserConfig(resized);
}

extern "C" uint32_t ElRt64_MsaaSupported(void) {
    if (!app || !app->device->getCapabilities().sampleLocations) return 0;
    const bool hdr = app->shaderLibrary->usesHDR;
    const RenderSampleCounts supported =
        app->device->getSampleCountsSupported(RT64::RenderTarget::colorBufferFormat(hdr)) &
        app->device->getSampleCountsSupported(RT64::RenderTarget::depthBufferFormat());
    return uint32_t(supported) & (2 | 4 | 8);
}

extern "C" void ElRt64_Shutdown(void) {
    shown = false;
    if (app) app->end(); // RT64's deinit hook takes the overlay's ImGui down with it
    app.reset();
    RT64::SetRenderHooks(nullptr, nullptr, nullptr);
    imgui.reset();
}
