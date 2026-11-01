// The window's presenter for the native renderer (present_vk.cpp), from King's Field IV's
// port/platform/present_vk.cpp: a swapchain on the renderer's own Vulkan device, the picture blitted
// into it fitted to the window, Dear ImGui (the F1 menu) drawn over it.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct SDL_Window;
struct ImDrawData;

namespace render {

// The renderer's device, and its queue lock (Granite's external_queue_lock: the presenter submits to
// the same queue as the renderer)
struct PresentDevice {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t api_version;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    void *lock_context;
};

// vsync: 0 off (immediate, else mailbox), 1 FIFO, 2 mailbox when there is one (else FIFO)
bool present_open(const PresentDevice &device, SDL_Window *window, int vsync);
bool present_imgui_init();
void present_set_vsync(int mode);
// 0 sharp pixels (nearest), else smooth (linear). "Smooth pixel edges" (2) is scaled by the renderer before
// (render_native.cpp, present_area.frag) to present_fit_size's size, which the presenter then copies as it is
void present_set_scaling(int scaling);
// The size a w x h picture is shown at in the window now (false: no window to show it in)
bool present_fit_size(uint32_t w, uint32_t h, uint32_t *fit_w, uint32_t *fit_h);

// Where a w x h picture is shown in the window now, in pixels (false: no window to show it in)
bool present_fit_rect(uint32_t w, uint32_t h, int *x, int *y, int *fit_w, int *fit_h);

// Show `source` (a w x h image in GENERAL layout that the queue wrote before this) fitted 4:3 into
// the window, black around it, with `overlay` (null: none) over it. With `window_rgba`, the window
// as shown is read back into it (RGBA, R in the low byte; *width and *height its size).
bool present_frame(VkImage source, uint32_t w, uint32_t h, ImDrawData *overlay, std::vector<uint32_t> *window_rgba,
                   uint32_t *width, uint32_t *height);

// The GPU device was lost (VK_ERROR_DEVICE_LOST from the presenter's own calls, or the window's surface was lost
// and could not be made again): a line saying so, else null. Nothing more is presented once it is set; the
// renderer ends the game (host::fatal). A lost surface alone is made again in place (a new surface and swapchain).
// ELTALE_SIM_GPU_LOST=device:N or surface:N (a test): the Nth picture presented acts as if the GPU said so.
const char *present_lost();

void present_close();

} // namespace render
