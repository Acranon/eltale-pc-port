// --renderer native (port/host/host.h's Renderer): our own renderer on Granite (Vulkan), in
// eltale_host.dll: the Granite device (made as King's Field IV's gsn.cpp makes its own: Context then
// Device, the window's surface and swapchain extensions on), one picture image of the game's size, and
// the presenter (present_vk.cpp). In gameplay the picture is the newest recording the game made
// (nd_renderer.cpp, docs/renderer/NATIVE_DRAW.md: the kinds drawn natively so far); a screen the CPU
// drew (the Controller Pak manager) is uploaded into it as one texture. The F1 menu is drawn over it
// by ImGui's Vulkan backend. Headless (ElHost_InitHeadless) it has no window: only the pictures the
// compare tools ask for (--dump-frames).
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
#include "present_vk.h"
#include "png.h"

#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

namespace host {
namespace {

using namespace Vulkan;

constexpr uint32_t PICTURE_W = 320, PICTURE_H = 240;
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

        ImageCreateInfo info = ImageCreateInfo::render_target(PICTURE_W, PICTURE_H, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        picture = device.create_image(info);
        if (!picture) {
            std::fprintf(stderr, "[render] no picture image\n");
            return false;
        }
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
        const char *dir = std::getenv("ELTALE_PRESENT_DUMP");
        if (dir && *dir) dump_dir = dir;
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
        // the newest recording (none yet: the clear colour)
        CommandBufferHandle cmd = begin();
        const render::Recording *rec = render::native_draw().latest();
        if (rec) {
            render::DrawOptions opt;
            opt.border = settings.graphics.border;
            opt.three_point = settings.graphics.three_point;
            opt.n64_fog = settings.graphics.n64_fog;
            render::native_draw().render(*cmd, *picture, rec, opt);
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
        finish(cmd);
        const bool first = !cleared;
        cleared = true;
        show(overlay, first && !dump_dir.empty() ? "native_clear" : nullptr);
    }

    void present_cpu(const uint16_t *pixels, int width, int height, ImDrawData *overlay) override {
        if (!pixels || width != int(PICTURE_W) || height != int(PICTURE_H)) return;
        // the RGBA5551 screen into the picture, as RGBA8 (each 5 bits widened, so they come back exact)
        CommandBufferHandle cmd = begin();
        uint32_t *out = static_cast<uint32_t *>(cmd->update_image(*picture));
        uint64_t hash = UINT64_C(0xCBF29CE484222325);
        for (uint32_t i = 0; i < PICTURE_W * PICTURE_H; i++) {
            const uint16_t p = pixels[i];
            hash = (hash ^ p) * UINT64_C(0x100000001B3);
            const uint32_t r = p >> 11 & 31, g = p >> 6 & 31, b = p >> 1 & 31;
            out[i] = (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (p & 1 ? 0xFF000000u : 0);
        }
        finish(cmd);
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

    uint32_t wires() const override { return FEATURE_SCALING | FEATURE_VSYNC | FEATURE_BORDER | FEATURE_THREE_POINT | FEATURE_N64_FOG; }

    void graphics_changed() override {
        render::present_set_vsync(vsync_now());
        render::present_set_scaling(settings.graphics.scaling);
    }

    void graphics_page_extra() override {
        ImGui::Spacing();
        ImGui::TextDisabled("Drawn by the native renderer (--renderer native): map objects so far (R2).");
    }

    void shutdown() override {
        if (headless) {
            device.wait_idle();
            render::native_draw().shutdown();
            return;
        }
        if (!picture) return;
        device.wait_idle();
        render::native_draw().shutdown();
        render::present_close();
        picture.reset();
    }

private:
    // a hidden window (the tests) never waits for the display
    static bool vsync_now() { return settings.graphics.vsync && !std::getenv("ELTALE_HIDDEN_WINDOW"); }

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
        render::present_frame(picture->get_image(), PICTURE_W, PICTURE_H, overlay, dump_name ? &rgba : nullptr, &w, &h);
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
        cmd->copy_image_to_buffer(*buffer, *picture, 0, {0, 0, 0}, {PICTURE_W, PICTURE_H, 1}, 0, 0,
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
    ImageHandle picture;
    bool cleared = false;
    uint64_t cpu_hash = 0;
    unsigned cpu_count = 0;
    std::string dump_dir;
    bool menu_dumped = false;
    bool headless = false;
};

} // namespace

Renderer *make_native_renderer() { return new NativeRenderer(); }

} // namespace host
