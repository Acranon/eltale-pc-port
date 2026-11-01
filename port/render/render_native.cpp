// --renderer native (port/host/host.h's Renderer): our own renderer on Granite (Vulkan), in
// eltale_host.dll: the Granite device (made as King's Field IV's gsn.cpp makes its own: Context then
// Device, the window's surface and swapchain extensions on), one picture image of the game's size, and
// the presenter (present_vk.cpp). In gameplay the picture is the newest recording the game made
// (nd_renderer.cpp, docs/renderer/NATIVE_DRAW.md: the kinds drawn natively so far); a screen the CPU
// drew (the Controller Pak manager) is uploaded into it as one texture. The F1 menu is drawn over it
// by ImGui's Vulkan backend. Headless (ElHost_InitHeadless) it has no window: only the pictures the
// compare tools ask for (--dump-frames).
//
// Pictures between ticks (R6, docs/renderer/NATIVE_DRAW.md "Pictures between ticks"): with the Graphics
// page's frame rate other than Original, pictures are shown on a grid of due times (the fixed rate, or
// the display's), each the recordings' picture at its time (NativeDraw::picture). The game's own timing
// does not change: the pictures are made while the game's thread waits for its next event (wait(), from
// port/p0/host.c's pacing) and at the retraces. A picture late by more than one interval is left out.
//
// ELTALE_PRESENT_LOG=file: a line per picture shown ("P wall v kind": kind n newest, b between, f as is
// for a reason) and per retrace ("V wall vi"), for tools/pace_test.py; ELTALE_PRESENT_SIM=1: the pacing
// alone (nothing drawn or presented). F8: the flash recorder (the last pictures shown, to a folder).
//
// R7 (modern options, docs/renderer/NATIVE_DRAW.md "Modern options"): the picture is 320x240 times the resolution
// scale (Original 1x, a multiple, or Match the window: the window's height over 240, rounded up, so the presenter
// only ever scales down), drawn with MSAA when asked (resolved into the picture), the fog and far plane pushed out
// by the draw distance, the N64 dither for Classic. A CPU-drawn screen has a 320x240 image of its own. The
// pipelines are made ahead at the start (nd_pipeline_warm.cpp). ELTALE_RES_SCALE=n / ELTALE_MSAA=n (tests)
// override the settings.
//
// ELTALE_PRESENT_DUMP=folder (a test): each new CPU screen presented is read back from the GPU and
// written as folder/native_cpu_<n>.png, with P1_NATIVE_CPU_FRAME n hash (the hash Port_NoteCpuFrame
// prints for the same pixels, worked out from what the GPU holds), and the window as shown (menu
// included) as folder/native_window_<n>.png; the first picture with the menu open as
// folder/native_menu.png, and the first gameplay picture as folder/native_clear.png.
// Granite first (it brings Vulkan's headers through volk, and wants to be first)
#include "context.hpp"
#include "device.hpp"

#include "host.h"
#include "nd_renderer.h"
#include "nd_interp.h"
#include "nd_pipeline_warm.h"
#include "present_vk.h"
#include "png.h"

#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

namespace host {
namespace {

using namespace Vulkan;

constexpr uint32_t PICTURE_W = 320, PICTURE_H = 240;
constexpr double VI_SECONDS = 781250.0 / 46875000.0; // a retrace: the game's VI (port/os/kernel.c), 1/60 s
constexpr int FLASH_PICTURES = 64;                    // the flash recorder keeps this many

double wall() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Sleeps until `until` (wall()): a high-resolution timer to half a millisecond before, then a yield loop
void sleep_until(double until) {
    const double left = until - wall();
#ifdef _WIN32
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (left > 0.0007 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.0005) * 1e7);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    while (wall() < until) SwitchToThread();
#else
    if (left > 0.0007) SDL_DelayNS(Uint64((left - 0.0005) * 1e9));
    while (wall() < until) {}
#endif
}
// the gameplay picture until the game's draws come (R2): a dark slate, so it is clearly ours
const VkClearColorValue CLEAR = {{0.10f, 0.12f, 0.16f, 1.0f}};

class NativeRenderer final : public Renderer {
public:
    const char *name() const override { return "native"; }
    uint32_t window_flags() const override { return SDL_WINDOW_VULKAN; }

    bool init(SDL_Window *window) override {
        if (!Context::init_loader(nullptr)) {
            std::fprintf(stderr, "[render] no Vulkan loader\n");
            return false;
        }
        Uint32 count = 0;
        const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&count);
        std::vector<const char *> instance_ext(sdl_ext, sdl_ext + count);
        const char *device_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        context.set_num_thread_indices(1);
        if (!context.init_instance_and_device(instance_ext.data(), uint32_t(instance_ext.size()), device_ext, 1, 0)) {
            std::fprintf(stderr, "[render] no Vulkan device with a swapchain\n");
            return false;
        }
        device.set_context(context);
        device.init_frame_contexts(2);
        const VkPhysicalDeviceProperties &props = context.get_gpu_props();
        std::fprintf(stderr, "[render] Granite on %s (Vulkan %u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                     VK_API_VERSION_MINOR(props.apiVersion));

        // the CPU-drawn screens' picture (320x240 always), and the game's (the resolution scale: ensure_picture)
        ImageCreateInfo info = ImageCreateInfo::render_target(PICTURE_W, PICTURE_H, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        cpu_picture = device.create_image(info);
        if (!cpu_picture) {
            std::fprintf(stderr, "[render] no picture image\n");
            return false;
        }
        shown_image = cpu_picture.get();
        // the pipeline cache and the pipelines made ahead (before the renderer makes any)
        render::pipeline_warm_open(device, data_dir(), settings.graphics.shader_warmup);
        if (!render::native_draw().init(device)) {
            std::fprintf(stderr, "[render] the native renderer's resources could not be made\n");
            return false;
        }

        const QueueInfo &queues = context.get_queue_info();
        render::PresentDevice pd = {};
        pd.instance = context.get_instance();
        pd.gpu = context.get_gpu();
        pd.device = context.get_device();
        pd.queue = queues.queues[QUEUE_INDEX_GRAPHICS];
        pd.queue_family = queues.family_indices[QUEUE_INDEX_GRAPHICS];
        pd.api_version = context.get_enabled_device_features().instance_api_core_version;
        pd.get_instance_proc_addr = Context::get_instance_proc_addr();
        pd.lock = [](void *d) { static_cast<Device *>(d)->external_queue_lock(); };
        pd.unlock = [](void *d) { static_cast<Device *>(d)->external_queue_unlock(); };
        pd.lock_context = &device;
        if (!render::present_open(pd, window, vsync_now())) return false;
        render::present_set_scaling(settings.graphics.scaling);
        apply_options();
        if (!ensure_picture()) return false;
        render::pipeline_warm_start(samples_now());
        const char *dir = std::getenv("ELTALE_PRESENT_DUMP");
        if (dir && *dir) dump_dir = dir;
        if (const char *l = std::getenv("ELTALE_PRESENT_LOG")) log = std::fopen(l, "w");
        sim = std::getenv("ELTALE_PRESENT_SIM") != nullptr;
        return true;
    }

    // Without a window (--dump-frames headless): the device and the renderer's resources only
    bool init_headless() override {
        if (!Context::init_loader(nullptr)) {
            std::fprintf(stderr, "[render] no Vulkan loader\n");
            return false;
        }
        context.set_num_thread_indices(1);
        if (!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
            std::fprintf(stderr, "[render] no Vulkan device\n");
            return false;
        }
        device.set_context(context);
        device.init_frame_contexts(2);
        headless = true;
        return render::native_draw().init(device);
    }

    bool imgui_init() override {
        return ImGui_ImplSDL3_InitForVulkan(window()) && render::present_imgui_init();
    }

    void imgui_new_frame() override { ImGui_ImplVulkan_NewFrame(); }

    void present_game(const ElHostVi *, ImDrawData *overlay) override {
        // a retrace: its time (on the schedule when it is close to it: no jitter from the wake-up)
        const double now = wall();
        vi++;
        render::native_draw().set_vi(vi);
        vi_wall = (vi_wall > 0 && std::fabs(now - (vi_wall + VI_SECONDS)) < 0.004) ? vi_wall + VI_SECONDS : now;
        cpu_shown = false;
        if (log) std::fprintf(log, "V %.6f %llu\n", now, (unsigned long long)vi);
        if (!between_on()) {
            // original timing: the newest recording at each retrace (none yet: the clear colour)
            draw_and_show(render::native_draw().latest(), overlay, 'n', double(vi));
            return;
        }
        // pictures between: shown on the grid of due times; a retrace shows one only when one is due
        const double interval = 1.0 / rate();
        if (next_due <= 0 || now - next_due > 0.25) next_due = now; // (a start, after a pause)
        if (now + 0.0003 >= next_due) {
            show_between(now, overlay);
            advance(now, interval);
        }
    }

    void wait(uint64_t ns) override {
        if (!between_on() || cpu_shown) return;
        const double deadline = wall() + double(ns) * 1e-9, interval = 1.0 / rate();
        for (;;) {
            const double now = wall();
            if (next_due <= 0) next_due = now;
            if (now - next_due > interval) advance(now, interval); // late: the ones missed are left out
            if (next_due > deadline - 0.0003) return; // the next is the retrace's or later: the caller sleeps
            sleep_until(next_due);
            show_between(next_due, menu::frame(false));
            next_due += interval;
        }
    }

    void present_cpu(const uint16_t *pixels, int width, int height, ImDrawData *overlay) override {
        if (!pixels || width != int(PICTURE_W) || height != int(PICTURE_H)) return;
        vi++;
        render::native_draw().set_vi(vi);
        vi_wall = wall();
        next_due = 0;
        cpu_shown = true; // (no pictures between over it: wait() waits for the game's frames again)
        // the RGBA5551 screen into the picture, as RGBA8 (each 5 bits widened, so they come back exact)
        CommandBufferHandle cmd = begin();
        uint32_t *out = static_cast<uint32_t *>(cmd->update_image(*cpu_picture));
        uint64_t hash = UINT64_C(0xCBF29CE484222325);
        for (uint32_t i = 0; i < PICTURE_W * PICTURE_H; i++) {
            const uint16_t p = pixels[i];
            hash = (hash ^ p) * UINT64_C(0x100000001B3);
            const uint32_t r = p >> 11 & 31, g = p >> 6 & 31, b = p >> 1 & 31;
            out[i] = (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (p & 1 ? 0xFF000000u : 0);
        }
        finish(cmd);
        shown_image = cpu_picture.get();
        cleared = false;
        const bool fresh = hash != cpu_hash;
        cpu_hash = hash;
        if (fresh && !dump_dir.empty()) {
            cpu_count++;
            dump_picture();
            show(overlay, ("native_window_" + std::to_string(cpu_count)).c_str());
        } else {
            show(overlay, nullptr);
        }
    }

    void present_again(ImDrawData *overlay) override {
        const bool first = !menu_dumped && overlay && !dump_dir.empty();
        menu_dumped |= first;
        show(overlay, first ? "native_menu" : nullptr);
    }

    uint32_t wires() const override {
        return FEATURE_SCALING | FEATURE_VSYNC | FEATURE_BORDER | FEATURE_THREE_POINT | FEATURE_N64_FOG | FEATURE_FRAME_RATE |
               FEATURE_RESOLUTION | FEATURE_MSAA | FEATURE_DRAW_DISTANCE | FEATURE_N64_DITHER | FEATURE_SHADER_WARMUP;
    }

    // the MSAA counts the GPU can draw (colour and depth) as bits 2, 4, 8
    uint32_t msaa_supported() const override {
        if (headless || !picture) return 0;
        const VkPhysicalDeviceLimits &l = context.get_gpu_props().limits;
        return uint32_t(l.framebufferColorSampleCounts & l.framebufferDepthSampleCounts) & (2u | 4u | 8u);
    }

    void graphics_changed() override {
        render::present_set_vsync(vsync_now());
        render::present_set_scaling(settings.graphics.scaling);
        apply_options();
        // (other samples: their pipelines made ahead too, on the warm-up's threads)
        render::pipeline_warm_start(samples_now());
        next_due = 0;
    }

    void graphics_page_extra() override {
        ImGui::Spacing();
        ImGui::TextDisabled("Drawn by the native renderer (--renderer native).");
        // the pictures between ticks: what was shown, and what could not be made (no hidden safety nets)
        const render::PictureStats &p = render::native_draw().picture_stats();
        const render::InterpStats &st = render::native_draw().interp_stats();
        if (between_on()) {
            ImGui::TextDisabled("Pictures between ticks: %.0f a second (%llu left out late)", shown_rate,
                                (unsigned long long)left_out);
            ImGui::TextDisabled("  %llu between, %llu newest, %llu as is (no earlier %llu, gap %llu, mode %llu, camera %llu, cut %llu)",
                                (unsigned long long)p.between, (unsigned long long)p.newest, (unsigned long long)p.fallback,
                                (unsigned long long)p.early, (unsigned long long)p.gap, (unsigned long long)p.mode,
                                (unsigned long long)p.camera, (unsigned long long)p.cut);
            ImGui::TextDisabled("  a pair's plan %.0f us, a picture's %.0f us (worst %.0f / %.0f); items snapped %llu, unmatched %llu",
                                st.plans ? st.plan_us / double(st.plans) : 0.0, st.pictures ? st.build_us / double(st.pictures) : 0.0,
                                st.plan_max_us, st.build_max_us, (unsigned long long)st.snapped, (unsigned long long)st.unmatched);
        }
        ImGui::TextDisabled("Picture %ux%u, MSAA %ux; draw distance %.1fx", picture ? picture->get_width() : 0,
                            picture ? picture->get_height() : 0, samples_now(), render::native_draw().draw_distance());
        render::pipeline_warm_page();
        ImGui::TextDisabled("F8: the flash recorder (the last %d pictures shown, into the data folder)", FLASH_PICTURES);
    }

    void shutdown() override {
        if (log) std::fclose(log), log = nullptr;
        if (headless) {
            device.wait_idle();
            render::native_draw().shutdown();
            return;
        }
        if (!cpu_picture) return;
        device.wait_idle();
        render::pipeline_warm_close();
        render::native_draw().shutdown();
        render::present_close();
        flash.clear();
        picture.reset();
        cpu_picture.reset();
    }

private:
    // ---- R7: the picture's size and the options ----
    // the resolution scale: Original 1, a multiple, or the window's height over 240 rounded up (at most 9: 4K);
    // ELTALE_RES_SCALE=n overrides (tests)
    static uint32_t scale_now() {
        if (const char *e = std::getenv("ELTALE_RES_SCALE")) return uint32_t(std::min(std::max(std::atoi(e), 1), 9));
        const Graphics &g = settings.graphics;
        if (g.resolution == 0) return 1;
        if (g.resolution == 2) return uint32_t(std::min(std::max(g.resolution_scale, 1), 8));
        int w = 0, h = 0;
        window_pixels(&w, &h);
        return uint32_t(std::min(std::max(int(std::ceil(double(h) / PICTURE_H - 0.001)), 1), 9));
    }
    // MSAA samples (ELTALE_MSAA=n overrides), as the GPU can
    uint32_t samples_now() const {
        int n = settings.graphics.msaa;
        if (const char *e = std::getenv("ELTALE_MSAA")) n = std::atoi(e);
        uint32_t s = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
        const uint32_t ok = msaa_supported();
        while (s > 1 && ok && !(ok & s)) s /= 2;
        return s;
    }
    // the draw distance: Original 1x, Far 2x, Farther 3x (ELTALE_DRAW_DISTANCE overrides, nd_renderer.cpp)
    static float distance_now() {
        static const float k[] = { 1.0f, 2.0f, 3.0f };
        return k[std::min(std::max(settings.graphics.draw_distance, 0), 2)];
    }
    void apply_options() {
        render::native_draw().set_draw_distance(distance_now());
    }
    render::DrawOptions options() const {
        render::DrawOptions opt;
        opt.border = settings.graphics.border;
        opt.three_point = settings.graphics.three_point;
        opt.n64_fog = settings.graphics.n64_fog;
        opt.samples = samples_now();
        opt.draw_distance = render::native_draw().draw_distance();
        opt.n64_dither = settings.graphics.n64_dither;
        return opt;
    }
    // the game's picture at this size (made again when the scale or the window changes)
    bool ensure_picture() {
        const uint32_t s = scale_now(), w = PICTURE_W * s, h = PICTURE_H * s;
        if (picture && picture->get_width() == w && picture->get_height() == h) return true;
        device.wait_idle();
        flash.clear();
        flash_count = 0;
        if (shown_image == picture.get()) shown_image = cpu_picture.get();
        ImageCreateInfo info = ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        picture = device.create_image(info);
        if (!picture) {
            std::fprintf(stderr, "[render] no %ux%u picture image\n", w, h);
            return false;
        }
        std::fprintf(stderr, "[render] picture %ux%u (%ux)\n", w, h, s);
        return true;
    }

    // a hidden window (the tests) never waits for the display; pictures between ticks never wait for it
    // either (mailbox: the newest at each refresh, no tearing), else vsync is FIFO
    static int vsync_now() {
        if (!settings.graphics.vsync || std::getenv("ELTALE_HIDDEN_WINDOW")) return 0;
        return settings.graphics.frame_rate != 0 ? 2 : 1;
    }

    // pictures between ticks: the frame rate switch (Original = the game's timing)
    bool between_on() const { return !headless && settings.graphics.frame_rate != 0; }
    double rate() const {
        double hz = settings.graphics.frame_rate_target;
        if (settings.graphics.frame_rate == 1) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window()));
            hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.0;
        }
        return hz < 30 ? 30 : hz > 1000 ? 1000 : hz;
    }

    // the next due time past `now` (those skipped: left out)
    void advance(double now, double interval) {
        next_due += interval;
        if (now - next_due > interval) {
            const double skip = std::floor((now - next_due) / interval);
            left_out += uint64_t(skip);
            next_due += skip * interval;
        }
    }

    // the picture at wall time `at`: the recordings' picture at that time in retraces
    void show_between(double at, ImDrawData *overlay) {
        const double v = double(vi) + std::fmin(std::fmax((at - vi_wall) / VI_SECONDS, 0.0), 1.0);
        render::NativeDraw &nd = render::native_draw();
        const render::PictureStats before = nd.picture_stats();
        const render::Recording *rec = nd.picture(v, true);
        const render::PictureStats &after = nd.picture_stats();
        const char kind = after.between != before.between ? 'b' : after.fallback != before.fallback ? 'f' : 'n';
        draw_and_show(rec, overlay, kind, v);
    }

    // draws `rec` (null: the clear colour) and shows it; kind and v for the log and the flash recorder
    void draw_and_show(const render::Recording *rec, ImDrawData *overlay, char kind, double v) {
        const double now = wall();
        if (log) std::fprintf(log, "P %.6f %.4f %c\n", now, v, kind);
        count_rate(now);
        if (sim) return;
        // ELTALE_OPTIONS_CYCLE=N (a test): every N pictures the next MSAA count, resolution and draw distance, as
        // changing them in the F1 menu would (the targets and pipelines made again in play)
        static const long cycle = std::getenv("ELTALE_OPTIONS_CYCLE") ? std::atol(std::getenv("ELTALE_OPTIONS_CYCLE")) : 0;
        static uint64_t pictures = 0;
        if (cycle > 0 && ++pictures % uint64_t(cycle) == 0) {
            const uint64_t k = pictures / uint64_t(cycle);
            static const int counts[] = { 1, 2, 4, 8 };
            Graphics &g = settings.graphics;
            g.msaa = counts[k % 4];
            g.resolution = int(k % 3);
            g.resolution_scale = int(2 + k % 3);
            g.draw_distance = int(k % 3);
            std::fprintf(stderr, "[render] options cycle %llu: msaa %d resolution %d draw distance %d\n", (unsigned long long)k, g.msaa,
                         g.resolution, g.draw_distance);
            graphics_changed();
        }
        if (!ensure_picture()) return;
        if (rec) render::pipeline_warm_wait();
        CommandBufferHandle cmd = begin();
        if (rec) {
            render::native_draw().render(*cmd, *picture, rec, options());
        } else {
            RenderPassInfo rp;
            rp.num_color_attachments = 1;
            rp.color_attachments[0] = &picture->get_view();
            rp.clear_attachments = 1;
            rp.store_attachments = 1;
            rp.clear_color[0] = CLEAR;
            cmd->begin_render_pass(rp);
            cmd->end_render_pass();
        }
        flash_keep(*cmd, v);
        finish(cmd);
        shown_image = picture.get();
        render::pipeline_warm_tick();
        const bool first = !cleared;
        cleared = true;
        show(overlay, first && !dump_dir.empty() ? "native_clear" : nullptr);
        // F8, or ELTALE_FLASH_AT=N (a test): the flash recorder at the Nth picture
        static const long flash_at = std::getenv("ELTALE_FLASH_AT") ? std::atol(std::getenv("ELTALE_FLASH_AT")) : 0;
        if (menu::take_flash_key() || (flash_at > 0 && flash_count == uint64_t(flash_at))) flash_write();
    }

    void count_rate(double now) {
        rate_count++;
        if (now - rate_start >= 0.5) {
            shown_rate = rate_start > 0 ? double(rate_count) / (now - rate_start) : 0;
            rate_start = now;
            rate_count = 0;
        }
    }

    // ---- F8: the flash recorder (KF4's): the last pictures shown, kept on the GPU ----
    // (kept at most at twice the N64's size: a bigger picture is scaled down into it)
    uint32_t flash_div() const { return std::max(1u, picture->get_height() / (2 * PICTURE_H)); }
    void flash_keep(CommandBuffer &cmd, double v) {
        const uint32_t fw = picture->get_width() / flash_div(), fh = picture->get_height() / flash_div();
        if (flash.empty()) {
            ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(fw, fh, VK_FORMAT_R8G8B8A8_UNORM);
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.layout = ImageLayout::General;
            info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
            for (int i = 0; i < FLASH_PICTURES; i++) flash.push_back(device.create_image(info));
            flash_v.assign(FLASH_PICTURES, -1.0);
        }
        const size_t slot = size_t(flash_count % FLASH_PICTURES);
        if (!flash[slot]) return;
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (flash_div() == 1) {
            cmd.copy_image(*flash[slot], *picture);
        } else {
            cmd.blit_image(*flash[slot], *picture, {}, { int32_t(fw), int32_t(fh), 1 }, {},
                           { int32_t(picture->get_width()), int32_t(picture->get_height()), 1 }, 0, 0, 0, 0, 1, VK_FILTER_LINEAR);
        }
        flash_v[slot] = v;
        flash_count++;
    }

    void flash_write() {
        const uint64_t n = flash_count < FLASH_PICTURES ? flash_count : FLASH_PICTURES;
        if (!n) return;
        char when[32] = "0";
        const std::time_t t = std::time(nullptr);
        if (const std::tm *lt = std::localtime(&t)) std::strftime(when, sizeof when, "%Y%m%d-%H%M%S", lt);
        const std::string folder = data_dir() + "/flash_" + std::to_string(vi) + "_" + when;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
        device.wait_idle();
        FILE *list = std::fopen((folder + "/pictures.txt").c_str(), "w");
        if (list) std::fprintf(list, "# picture retrace phase (the picture's time in retraces; phase 0 is a retrace's own)\n");
        const uint32_t fw = flash[0]->get_width(), fh = flash[0]->get_height();
        for (uint64_t k = 0; k < n; k++) {
            const size_t slot = size_t((flash_count - n + k) % FLASH_PICTURES);
            BufferCreateInfo bi = {};
            bi.size = fw * fh * 4;
            bi.domain = BufferDomain::CachedHost;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            BufferHandle buffer = device.create_buffer(bi);
            CommandBufferHandle cmd = begin();
            cmd->copy_image_to_buffer(*buffer, *flash[slot], 0, {0, 0, 0}, {fw, fh, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                         VK_ACCESS_2_HOST_READ_BIT);
            Fence fence;
            device.submit(cmd, &fence);
            fence->wait();
            const uint32_t *px = static_cast<const uint32_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
            std::vector<uint32_t> rgba(px, px + fw * fh);
            device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
            for (uint32_t &p : rgba) p |= 0xFF000000u;
            char name[32];
            std::snprintf(name, sizeof name, "%04llu.png", (unsigned long long)k);
            render_png_write((folder + "/" + name).c_str(), rgba.data(), fw, fh);
            const double v = flash_v[slot];
            if (list) std::fprintf(list, "%s %lld %.4f\n", name, (long long)std::floor(v), v - std::floor(v));
        }
        if (list) std::fclose(list);
        device.next_frame_context();
        const std::string note = "Flash recorder: " + std::to_string(n) + " pictures in " + folder;
        menu::notice(note.c_str());
        std::fprintf(stderr, "[render] %s\n", note.c_str());
    }

    // a command buffer for the picture, after everything before it on the queue (the presenter's last
    // blit read the picture)
    CommandBufferHandle begin() {
        CommandBufferHandle cmd = device.request_command_buffer();
        cmd->full_barrier();
        return cmd;
    }

    // the picture's work submitted ahead of the presenter's (same queue: the presenter's barrier orders it)
    void finish(CommandBufferHandle &cmd) {
        cmd->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        device.submit(cmd);
        device.flush_frame();
    }

    void show(ImDrawData *overlay, const char *dump_name) {
        std::vector<uint32_t> rgba;
        uint32_t w = 0, h = 0;
        const Image &img = *shown_image;
        render::present_frame(img.get_image(), img.get_width(), img.get_height(), overlay, dump_name ? &rgba : nullptr, &w, &h);
        if (dump_name && !rgba.empty()) {
            const std::string path = dump_dir + "/" + dump_name + ".png";
            if (render_png_write(path.c_str(), rgba.data(), w, h)) std::fprintf(stderr, "[render] wrote %s\n", path.c_str());
        }
        device.next_frame_context();
    }

    // The picture read back from the GPU: written as a PNG, and hashed as the game's RGBA5551
    void dump_picture() {
        BufferCreateInfo info = {};
        info.size = PICTURE_W * PICTURE_H * 4;
        info.domain = BufferDomain::CachedHost;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        BufferHandle buffer = device.create_buffer(info);
        CommandBufferHandle cmd = begin();
        cmd->copy_image_to_buffer(*buffer, *cpu_picture, 0, {0, 0, 0}, {PICTURE_W, PICTURE_H, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
        device.submit(cmd, &fence);
        fence->wait();
        const uint32_t *px = static_cast<const uint32_t *>(device.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
        uint64_t hash = UINT64_C(0xCBF29CE484222325);
        for (uint32_t i = 0; i < PICTURE_W * PICTURE_H; i++) {
            const uint32_t v = px[i];
            const uint16_t p = uint16_t((v & 0xFF) >> 3 << 11 | (v >> 8 & 0xFF) >> 3 << 6 | (v >> 16 & 0xFF) >> 3 << 1 |
                                        (v >> 31));
            hash = (hash ^ p) * UINT64_C(0x100000001B3);
        }
        std::printf("P1_NATIVE_CPU_FRAME n=%u hash=%016llx\n", cpu_count, (unsigned long long)hash);
        std::fflush(stdout);
        const std::string path = dump_dir + "/native_cpu_" + std::to_string(cpu_count) + ".png";
        if (render_png_write(path.c_str(), px, PICTURE_W, PICTURE_H)) std::fprintf(stderr, "[render] wrote %s\n", path.c_str());
        device.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
    }

    Context context;
    Device device;
    ImageHandle picture;          // the game's (320x240 times the resolution scale)
    ImageHandle cpu_picture;      // a CPU-drawn screen's (320x240)
    const Image *shown_image = nullptr; // the one shown last (present_again shows it again)
    bool cleared = false;
    uint64_t cpu_hash = 0;
    unsigned cpu_count = 0;
    std::string dump_dir;
    bool menu_dumped = false;
    bool headless = false;
    // pacing (R6)
    uint64_t vi = 0;         // retraces presented
    double vi_wall = 0;      // the last retrace's wall time
    double next_due = 0;     // the next picture's due time (0: start again)
    uint64_t left_out = 0;   // due pictures skipped (late)
    double rate_start = 0, shown_rate = 0;
    uint64_t rate_count = 0;
    FILE *log = nullptr;
    bool sim = false;
    bool cpu_shown = false;  // the last retrace showed a CPU-drawn screen (the Controller Pak manager)
    std::vector<ImageHandle> flash;
    std::vector<double> flash_v;
    uint64_t flash_count = 0;
};

} // namespace

Renderer *make_native_renderer() { return new NativeRenderer(); }

} // namespace host
