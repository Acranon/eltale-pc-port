// The native renderer's presenter (present_vk.h), from King's Field IV's port/platform/present_vk.cpp
// (its swapchain, vsync, fit and ImGui pass; without its CPU upload path, flash recorder and counter,
// which Eltale's renderer doesn't need yet).
//
// The swapchain is made on the renderer's own Vulkan device. Each picture is the renderer's image,
// blitted into the swapchain's image (scaled and placed 4:3 with black bars, sharp or smooth), with
// Dear ImGui's draw lists (the F1 menu, the hint and notices) over it in a render pass drawn by
// ImGui's own Vulkan backend. The presenter submits to the renderer's queue under its lock; the
// queue's order and a barrier at the start of each command buffer are all the synchronisation the
// renderer's picture needs. Everything here runs on the game's thread.
#include "present_vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

namespace render {
namespace {

// ---- Vulkan's functions, from the renderer's instance and device ----------------------------------
#define INSTANCE_FUNCTIONS(X)                                                                              \
    X(vkGetDeviceProcAddr)                                                                                 \
    X(vkDestroySurfaceKHR)                                                                                 \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                           \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                           \
    X(vkGetPhysicalDeviceFormatProperties)                                                                 \
    X(vkGetPhysicalDeviceMemoryProperties)
#define DEVICE_FUNCTIONS(X)                                                                                \
    X(vkCreateSwapchainKHR)                                                                                \
    X(vkDestroySwapchainKHR)                                                                               \
    X(vkGetSwapchainImagesKHR)                                                                             \
    X(vkAcquireNextImageKHR)                                                                               \
    X(vkQueuePresentKHR)                                                                                   \
    X(vkQueueSubmit)                                                                                       \
    X(vkQueueWaitIdle)                                                                                     \
    X(vkCreateCommandPool)                                                                                 \
    X(vkDestroyCommandPool)                                                                                \
    X(vkAllocateCommandBuffers)                                                                            \
    X(vkBeginCommandBuffer)                                                                                \
    X(vkEndCommandBuffer)                                                                                  \
    X(vkCreateFence)                                                                                       \
    X(vkDestroyFence)                                                                                      \
    X(vkWaitForFences)                                                                                     \
    X(vkResetFences)                                                                                       \
    X(vkCreateSemaphore)                                                                                   \
    X(vkDestroySemaphore)                                                                                  \
    X(vkCreateImageView)                                                                                   \
    X(vkDestroyImageView)                                                                                  \
    X(vkCreateFramebuffer)                                                                                 \
    X(vkDestroyFramebuffer)                                                                                \
    X(vkCreateRenderPass)                                                                                  \
    X(vkDestroyRenderPass)                                                                                 \
    X(vkCmdBeginRenderPass)                                                                                \
    X(vkCmdEndRenderPass)                                                                                  \
    X(vkCmdPipelineBarrier)                                                                                \
    X(vkCmdClearColorImage)                                                                                \
    X(vkCmdBlitImage)                                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                              \
    X(vkCreateBuffer)                                                                                      \
    X(vkDestroyBuffer)                                                                                     \
    X(vkAllocateMemory)                                                                                    \
    X(vkFreeMemory)                                                                                        \
    X(vkBindBufferMemory)                                                                                  \
    X(vkGetBufferMemoryRequirements)                                                                       \
    X(vkMapMemory)

#define DECLARE(name) PFN_##name name = nullptr;
struct Functions {
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
};
#undef DECLARE
Functions vk;

const uint32_t FRAMES = 2; // command buffers in flight

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;         // its command buffer has run (made signalled)
    VkSemaphore acquired = VK_NULL_HANDLE; // the swapchain's image is free to draw into
};

struct SwapImage {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkSemaphore rendered; // drawn: the presentation engine may show it
};

struct Presenter {
    bool up = false;
    PresentDevice dev = {};
    SDL_Window *window = nullptr;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format = {};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    bool can_read = false; // the swapchain's images can be copied from (the window read back)
    std::vector<SwapImage> images;
    bool stale = true;     // the swapchain is to be made again (a new size, vsync, or out of date)
    int vsync_made = -1;
    int vsync_wanted = 1;
    int scaling = 2;

    VkRenderPass pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    Frame frames[FRAMES];
    uint32_t frame = 0;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void *readback_map = nullptr;
    VkDeviceSize readback_size = 0;

    bool imgui = false;

    // a lost device (or a surface lost for good): what happened, for present_lost(); nothing is presented after
    const char *lost = nullptr;
    uint64_t presents = 0;     // present_frame calls that got as far as acquiring (ELTALE_SIM_GPU_LOST counts them)
    uint32_t surfaces_made = 0; // surfaces made again after VK_ERROR_SURFACE_LOST_KHR
};
Presenter pr;

// A Vulkan result: errors are logged; a lost device stops the presenter (present_lost)
void check(VkResult result) {
    if (result >= 0) return;
    std::fprintf(stderr, "[present] Vulkan error %d\n", int(result));
    if (result == VK_ERROR_DEVICE_LOST && !pr.lost) pr.lost = "The graphics device was lost (VK_ERROR_DEVICE_LOST).";
}

// ELTALE_SIM_GPU_LOST=device:N or surface:N (a test): the Nth acquire (from 1) answers as the GPU would
VkResult simulated(VkResult result) {
    static long at = -1;
    static VkResult as = VK_SUCCESS;
    if (at < 0) {
        at = 0;
        if (const char *e = std::getenv("ELTALE_SIM_GPU_LOST")) {
            if (!std::strncmp(e, "device:", 7)) as = VK_ERROR_DEVICE_LOST, at = std::atol(e + 7);
            else if (!std::strncmp(e, "surface:", 8)) as = VK_ERROR_SURFACE_LOST_KHR, at = std::atol(e + 8);
        }
    }
    if (at > 0 && pr.presents == uint64_t(at)) {
        std::fprintf(stderr, "[present] ELTALE_SIM_GPU_LOST: picture %llu acts as Vulkan error %d\n",
                     (unsigned long long)pr.presents, int(as));
        return as;
    }
    return result;
}

void lock() { pr.dev.lock(pr.dev.lock_context); }
void unlock() { pr.dev.unlock(pr.dev.lock_context); }

int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vk.vkGetPhysicalDeviceMemoryProperties(pr.dev.gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}

// A buffer the CPU reads, mapped for good
bool make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory, void **map) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.vkCreateBuffer(pr.dev.device, &info, nullptr, buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(pr.dev.device, *buffer, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    if (type < 0 || vk.vkAllocateMemory(pr.dev.device, &alloc, nullptr, memory) != VK_SUCCESS) {
        vk.vkDestroyBuffer(pr.dev.device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    vk.vkBindBufferMemory(pr.dev.device, *buffer, *memory, 0);
    vk.vkMapMemory(pr.dev.device, *memory, 0, VK_WHOLE_SIZE, 0, map);
    return true;
}

void free_buffer(VkBuffer *buffer, VkDeviceMemory *memory) {
    if (*buffer != VK_NULL_HANDLE) {
        vk.vkDestroyBuffer(pr.dev.device, *buffer, nullptr);
        vk.vkFreeMemory(pr.dev.device, *memory, nullptr); // (unmapped with it)
    }
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
}

void wait_frames() {
    for (Frame &f : pr.frames)
        if (f.done != VK_NULL_HANDLE) vk.vkWaitForFences(pr.dev.device, 1, &f.done, VK_TRUE, UINT64_MAX);
}

void drop_swapchain_images() {
    for (SwapImage &s : pr.images) {
        vk.vkDestroyFramebuffer(pr.dev.device, s.framebuffer, nullptr);
        vk.vkDestroyImageView(pr.dev.device, s.view, nullptr);
        vk.vkDestroySemaphore(pr.dev.device, s.rendered, nullptr);
    }
    pr.images.clear();
}

// Waits for the queue to be done with the swapchain (its images, their semaphores)
void wait_queue() {
    wait_frames();
    lock();
    vk.vkQueueWaitIdle(pr.dev.queue);
    unlock();
}

// The swapchain for the window's size and the vsync setting: false if there is none now (minimised)
bool remake_surface();

bool make_swapchain() {
    int drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSizeInPixels(pr.window, &drawable_w, &drawable_h);
    VkSurfaceCapabilitiesKHR caps;
    const VkResult caps_result = vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pr.dev.gpu, pr.surface, &caps);
    if (caps_result == VK_ERROR_SURFACE_LOST_KHR) {
        remake_surface(); // (the next picture makes the swapchain on the new surface)
        return false;
    }
    check(caps_result);
    if (caps_result != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = uint32_t(drawable_w < 0 ? 0 : drawable_w);
        extent.height = uint32_t(drawable_h < 0 ? 0 : drawable_h);
        extent.width = extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : extent.width;
        extent.width = extent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : extent.width;
        extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : extent.height;
        extent.height = extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
    }
    if (extent.width == 0 || extent.height == 0) return false; // minimised

    // vsync: FIFO (1), or mailbox when there is one (2: pictures between ticks, which must never wait for
    // the display: no tearing, the newest picture shown at each refresh); without (0): immediate, else
    // mailbox (no tearing, never waits), else FIFO after all
    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(pr.dev.gpu, pr.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(pr.dev.gpu, pr.surface, &count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (pr.vsync_wanted != 1) {
        const VkPresentModeKHR off[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
        for (int i = pr.vsync_wanted == 2 ? 1 : 0; i < 2; i++) {
            const VkPresentModeKHR want = off[i];
            bool found = false;
            for (VkPresentModeKHR m : modes) found |= m == want;
            if (found) {
                mode = want;
                break;
            }
        }
    }

    uint32_t images = caps.minImageCount + 1 < 2 ? 2 : caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) images = caps.maxImageCount;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    pr.can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (pr.can_read) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & usage) != usage) {
        std::fprintf(stderr, "[present] the swapchain's images cannot be drawn into by a copy\n");
        return false;
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                alpha = VkCompositeAlphaFlagBitsKHR(bit);
                break;
            }
        }
    }

    wait_queue(); // (the old one's images may still be in use)
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = pr.surface;
    info.minImageCount = images;
    info.imageFormat = pr.format.format;
    info.imageColorSpace = pr.format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = pr.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = vk.vkCreateSwapchainKHR(pr.dev.device, &info, nullptr, &swapchain);
    drop_swapchain_images();
    if (pr.swapchain != VK_NULL_HANDLE) {
        vk.vkDestroySwapchainKHR(pr.dev.device, pr.swapchain, nullptr);
        pr.swapchain = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[present] no swapchain (%d)\n", int(result));
        return false;
    }
    pr.swapchain = swapchain;
    pr.extent = extent;

    vk.vkGetSwapchainImagesKHR(pr.dev.device, swapchain, &count, nullptr);
    std::vector<VkImage> list(count);
    vk.vkGetSwapchainImagesKHR(pr.dev.device, swapchain, &count, list.data());
    for (VkImage image : list) {
        SwapImage s = {image, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = pr.format.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.vkCreateImageView(pr.dev.device, &view, nullptr, &s.view);
        VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = pr.pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &s.view;
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        vk.vkCreateFramebuffer(pr.dev.device, &fb, nullptr, &s.framebuffer);
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk.vkCreateSemaphore(pr.dev.device, &sem, nullptr, &s.rendered);
        pr.images.push_back(s);
    }
    pr.vsync_made = pr.vsync_wanted;
    pr.stale = false;
    static const char *const mode_names[] = {"immediate", "mailbox", "FIFO", "relaxed FIFO"};
    std::fprintf(stderr, "[present] swapchain %ux%u, %u images, %s\n", extent.width, extent.height, count,
                 mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? mode_names[mode] : "other");
    return true;
}

// The swapchain's format: 8-bit UNORM (the picture's values as they are), one a blit can scale into
// and ImGui can draw on
bool choose_format() {
    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(pr.dev.gpu, pr.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(pr.dev.gpu, pr.surface, &count, formats.data());
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32}) {
        for (const VkSurfaceFormatKHR &f : formats) {
            if (f.format != want || f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
            VkFormatProperties props;
            vk.vkGetPhysicalDeviceFormatProperties(pr.dev.gpu, want, &props);
            VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if ((props.optimalTilingFeatures & need) == need) {
                pr.format = f;
                return true;
            }
        }
    }
    return false;
}

// VK_ERROR_SURFACE_LOST_KHR: the window's surface made again (the swapchain with it, at the next picture). The
// render pass and ImGui's pipeline are for the old format: a new surface without it is lost for good.
bool remake_surface() {
    std::fprintf(stderr, "[present] the window's surface was lost: making it again\n");
    wait_queue();
    drop_swapchain_images();
    if (pr.swapchain != VK_NULL_HANDLE) vk.vkDestroySwapchainKHR(pr.dev.device, pr.swapchain, nullptr);
    pr.swapchain = VK_NULL_HANDLE;
    if (pr.surface != VK_NULL_HANDLE) vk.vkDestroySurfaceKHR(pr.dev.instance, pr.surface, nullptr);
    pr.surface = VK_NULL_HANDLE;
    pr.stale = true;
    const VkFormat format = pr.format.format;
    VkBool32 supported = VK_FALSE;
    if (++pr.surfaces_made > 16) { // (lost again and again: give up rather than spin)
        pr.lost = "The window's Vulkan surface kept being lost.";
    } else if (!SDL_Vulkan_CreateSurface(pr.window, pr.dev.instance, nullptr, &pr.surface)) {
        pr.surface = VK_NULL_HANDLE;
        pr.lost = "The window's Vulkan surface was lost and could not be made again.";
    } else if (vk.vkGetPhysicalDeviceSurfaceSupportKHR(pr.dev.gpu, pr.dev.queue_family, pr.surface, &supported) != VK_SUCCESS ||
               !supported || !choose_format() || pr.format.format != format) {
        pr.lost = "The window's Vulkan surface was lost, and the new one cannot be presented to the same way.";
    }
    if (pr.lost) std::fprintf(stderr, "[present] %s\n", pr.lost);
    return pr.lost == nullptr;
}

bool make_render_pass() {
    // ImGui's pass: over the picture (LOAD), left in COLOR_ATTACHMENT_OPTIMAL for the barrier after it
    VkAttachmentDescription colour = {};
    colour.format = pr.format.format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &colour;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    return vk.vkCreateRenderPass(pr.dev.device, &info, nullptr, &pr.pass) == VK_SUCCESS;
}

bool make_frames() {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = pr.dev.queue_family;
    if (vk.vkCreateCommandPool(pr.dev.device, &pool, nullptr, &pr.pool) != VK_SUCCESS) return false;
    for (Frame &f : pr.frames) {
        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pr.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk.vkAllocateCommandBuffers(pr.dev.device, &alloc, &f.cmd) != VK_SUCCESS ||
            vk.vkCreateFence(pr.dev.device, &fence, nullptr, &f.done) != VK_SUCCESS ||
            vk.vkCreateSemaphore(pr.dev.device, &sem, nullptr, &f.acquired) != VK_SUCCESS)
            return false;
    }
    return true;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
                   VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ImGui's textures (the font atlas) are made with a submit of ImGui's own and a wait for the queue:
// those go under the queue lock
bool imgui_textures_pending(ImDrawData *data) {
    if (data->Textures == nullptr) return false;
    for (ImTextureData *tex : *data->Textures)
        if (tex->Status != ImTextureStatus_OK) return true;
    return false;
}

// The picture's place in the window: its own shape (4:3, or wider with R7's widescreen), as large as
// fits, centred
void fit(int width, int height, int src_w, int src_h, int *x, int *y, int *w, int *h) {
    *w = width;
    *h = int(int64_t(width) * src_h / src_w);
    if (*h > height) {
        *h = height;
        *w = int(int64_t(height) * src_w / src_h);
    }
    *x = (width - *w) / 2;
    *y = (height - *h) / 2;
}

void close_all() {
    if (pr.dev.device != VK_NULL_HANDLE && vk.vkQueueWaitIdle != nullptr) {
        wait_queue();
        drop_swapchain_images();
        if (pr.swapchain != VK_NULL_HANDLE) vk.vkDestroySwapchainKHR(pr.dev.device, pr.swapchain, nullptr);
        for (Frame &f : pr.frames) {
            if (f.done != VK_NULL_HANDLE) vk.vkDestroyFence(pr.dev.device, f.done, nullptr);
            if (f.acquired != VK_NULL_HANDLE) vk.vkDestroySemaphore(pr.dev.device, f.acquired, nullptr);
        }
        free_buffer(&pr.readback, &pr.readback_memory);
        if (pr.pool != VK_NULL_HANDLE) vk.vkDestroyCommandPool(pr.dev.device, pr.pool, nullptr);
        if (pr.pass != VK_NULL_HANDLE) vk.vkDestroyRenderPass(pr.dev.device, pr.pass, nullptr);
    }
    if (pr.surface != VK_NULL_HANDLE) vk.vkDestroySurfaceKHR(pr.dev.instance, pr.surface, nullptr);
    pr = Presenter();
}

} // namespace

bool present_open(const PresentDevice &device, SDL_Window *window, int vsync) {
    pr = Presenter();
    pr.dev = device;
    pr.window = window;
    bool loaded = device.get_instance_proc_addr != nullptr;
#define LOAD_INSTANCE(name)                                                                                        \
    vk.name = loaded ? reinterpret_cast<PFN_##name>(device.get_instance_proc_addr(device.instance, #name)) : nullptr; \
    loaded = loaded && vk.name != nullptr;
#define LOAD_DEVICE(name)                                                                                          \
    vk.name = loaded ? reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device.device, #name)) : nullptr;       \
    loaded = loaded && vk.name != nullptr;
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_INSTANCE
#undef LOAD_DEVICE
    if (!loaded) {
        std::fprintf(stderr, "[present] Vulkan's swapchain functions are missing\n");
        pr = Presenter();
        return false;
    }
    if (!SDL_Vulkan_CreateSurface(window, device.instance, nullptr, &pr.surface)) {
        std::fprintf(stderr, "[present] no Vulkan surface for the window: %s\n", SDL_GetError());
        pr.surface = VK_NULL_HANDLE;
        close_all();
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vk.vkGetPhysicalDeviceSurfaceSupportKHR(device.gpu, device.queue_family, pr.surface, &supported);
    if (!supported) {
        std::fprintf(stderr, "[present] the renderer's queue cannot present to the window\n");
        close_all();
        return false;
    }
    if (!choose_format()) {
        std::fprintf(stderr, "[present] the window has no 8-bit format to present in\n");
        close_all();
        return false;
    }
    pr.vsync_wanted = vsync;
    if (!make_render_pass() || !make_frames() || !make_swapchain()) {
        std::fprintf(stderr, "[present] the swapchain could not be made\n");
        close_all();
        return false;
    }
    pr.up = true;
    return true;
}

bool present_imgui_init() {
    if (!pr.up) return false;
    auto loader = [](const char *name, void *) -> PFN_vkVoidFunction {
        return pr.dev.get_instance_proc_addr(pr.dev.instance, name);
    };
    uint32_t api = pr.dev.api_version ? pr.dev.api_version : VK_API_VERSION_1_1;
    if (!ImGui_ImplVulkan_LoadFunctions(api, loader)) return false;
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = api;
    info.Instance = pr.dev.instance;
    info.PhysicalDevice = pr.dev.gpu;
    info.Device = pr.dev.device;
    info.QueueFamily = pr.dev.queue_family;
    info.Queue = pr.dev.queue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = FRAMES;
    info.ImageCount = FRAMES;
    info.PipelineInfoMain.RenderPass = pr.pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check;
    pr.imgui = ImGui_ImplVulkan_Init(&info);
    return pr.imgui;
}

void present_set_vsync(int mode) { pr.vsync_wanted = mode; }

void present_set_scaling(int scaling) { pr.scaling = scaling; }

bool present_fit_size(uint32_t w, uint32_t h, uint32_t *fit_w, uint32_t *fit_h) {
    if (!pr.up || w == 0 || h == 0) return false;
    int drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSizeInPixels(pr.window, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0) return false;
    int x, y, fw, fh;
    fit(drawable_w, drawable_h, int(w), int(h), &x, &y, &fw, &fh);
    if (fw <= 0 || fh <= 0) return false;
    *fit_w = uint32_t(fw);
    *fit_h = uint32_t(fh);
    return true;
}

bool present_frame(VkImage source, uint32_t source_w, uint32_t source_h, ImDrawData *overlay,
                   std::vector<uint32_t> *window_rgba, uint32_t *width, uint32_t *height) {
    if (!pr.up || pr.lost) return false;
    if (SDL_GetWindowFlags(pr.window) & SDL_WINDOW_MINIMIZED) return false;
    int drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSizeInPixels(pr.window, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0) return false;
    if (pr.stale || pr.swapchain == VK_NULL_HANDLE || pr.vsync_made != pr.vsync_wanted || pr.extent.width != uint32_t(drawable_w) ||
        pr.extent.height != uint32_t(drawable_h)) {
        if (!make_swapchain()) return false;
    }

    Frame &f = pr.frames[pr.frame];
    check(vk.vkWaitForFences(pr.dev.device, 1, &f.done, VK_TRUE, UINT64_MAX));
    if (pr.lost) return false;
    uint32_t index = 0;
    pr.presents++;
    // (a simulated loss answers instead of the acquire: no image is taken, no semaphore signalled)
    VkResult result = simulated(VK_SUCCESS);
    if (result == VK_SUCCESS)
        result = vk.vkAcquireNextImageKHR(pr.dev.device, pr.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        pr.stale = true;
        return false;
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        remake_surface();
        return false;
    }
    if (result == VK_SUBOPTIMAL_KHR) pr.stale = true; // (drawn all the same, and made again for the next)
    else if (result != VK_SUCCESS) {
        check(result);
        return false;
    }
    SwapImage &target = pr.images[index];

    int x, y, w, h;
    fit(int(pr.extent.width), int(pr.extent.height), int(source_w), int(source_h), &x, &y, &w, &h);

    bool shot = window_rgba != nullptr && pr.can_read;
    if (shot) {
        VkDeviceSize size = VkDeviceSize(pr.extent.width) * pr.extent.height * 4;
        if (pr.readback_size < size) {
            free_buffer(&pr.readback, &pr.readback_memory);
            pr.readback_size = 0;
            if (make_host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &pr.readback, &pr.readback_memory, &pr.readback_map))
                pr.readback_size = size;
        }
        shot = pr.readback_size >= size;
    }

    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &begin);
    // what the queue wrote before (the renderer's picture) before it is read
    VkMemoryBarrier memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0,
                            nullptr, 0, nullptr);
    image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {};
    black.float32[3] = 1.0f;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (source != VK_NULL_HANDLE && w > 0 && h > 0) {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(source_w), int32_t(source_h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vk.vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                          pr.scaling == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (overlay != nullptr && pr.imgui) {
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = pr.pass;
        pass.framebuffer = target.framebuffer;
        pass.renderArea.extent = pr.extent;
        vk.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        bool textures = imgui_textures_pending(overlay);
        if (textures) lock();
        ImGui_ImplVulkan_RenderDrawData(overlay, cmd);
        if (textures) unlock();
        vk.vkCmdEndRenderPass(cmd);
    }
    if (shot) {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {pr.extent.width, pr.extent.height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pr.readback, 1, &copy);
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                                nullptr);
    } else {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    vk.vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &target.rendered;
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &target.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &pr.swapchain;
    present.pImageIndices = &index;
    vk.vkResetFences(pr.dev.device, 1, &f.done);
    lock();
    check(vk.vkQueueSubmit(pr.dev.queue, 1, &submit, f.done));
    result = vk.vkQueuePresentKHR(pr.dev.queue, &present);
    unlock();
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) pr.stale = true;
    else if (result == VK_ERROR_SURFACE_LOST_KHR) remake_surface();
    else check(result);
    if (shot && !pr.lost) {
        check(vk.vkWaitForFences(pr.dev.device, 1, &f.done, VK_TRUE, UINT64_MAX));
        const uint32_t pw = pr.extent.width, ph = pr.extent.height;
        window_rgba->resize(size_t(pw) * ph);
        const uint32_t *in = static_cast<const uint32_t *>(pr.readback_map);
        const bool bgra = pr.format.format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i < window_rgba->size(); i++) {
            uint32_t v = in[i];
            (*window_rgba)[i] = bgra ? (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16) : v;
        }
        *width = pw;
        *height = ph;
    } else if (window_rgba) {
        window_rgba->clear();
    }
    pr.frame = (pr.frame + 1) % FRAMES;
    return true;
}

const char *present_lost() { return pr.lost; }

void present_close() {
    if (pr.imgui) {
        wait_queue();
        ImGui_ImplVulkan_Shutdown();
        pr.imgui = false;
    }
    close_all();
}

} // namespace render
