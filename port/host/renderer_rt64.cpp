// --renderer rt64 (host.h): RT64 in eltale_rt64.dll (port/rt64, clang-cl), loaded at run time and
// drawing on the host's window. The menu is the host's ImGui with no renderer backend of its own
// (the atlas built up front, ImGui's "legacy" path); its draw lists are flattened into plain data
// (ElRt64Overlay) and RT64's own ImGui backend draws them over its picture.
#include "host.h"

#include <cstdio>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "port/rt64/eltale_rt64.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace host {
namespace {

class Rt64Renderer final : public Renderer {
public:
    const char *name() const override { return "RT64"; }
    uint32_t window_flags() const override { return 0; }

    bool init(SDL_Window *window) override {
#ifdef _WIN32
        lib = LoadLibraryA("eltale_rt64.dll");
        if (!lib) {
            std::fprintf(stderr, "[host] cannot load eltale_rt64.dll (tools/build_rt64.cmd)\n");
            return false;
        }
#define SYM(var, name) var = reinterpret_cast<decltype(var)>(reinterpret_cast<void *>(GetProcAddress(lib, name)))
        SYM(rt_init, "ElRt64_Init");
        SYM(rt_atlas, "ElRt64_SetOverlayAtlas");
        SYM(rt_draw, "ElRt64_DrawTask");
        SYM(rt_keep, "ElRt64_KeepRange");
        SYM(rt_present, "ElRt64_Present");
        SYM(rt_graphics, "ElRt64_SetGraphics");
        SYM(rt_msaa, "ElRt64_MsaaSupported");
        SYM(rt_shutdown, "ElRt64_Shutdown");
#undef SYM
        if (!rt_init || !rt_atlas || !rt_draw || !rt_keep || !rt_present || !rt_graphics || !rt_msaa || !rt_shutdown) {
            std::fprintf(stderr, "[host] eltale_rt64.dll is not the one this host needs (rebuild it)\n");
            return false;
        }
        void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        ElRt64Graphics g = options();
        if (!hwnd || rt_init(hwnd, GetCurrentThreadId(), &g) != 0) {
            std::fprintf(stderr, "[host] RT64: setup failed\n");
            return false;
        }
        up = true;
        return true;
#else
        (void)window;
        std::fprintf(stderr, "[host] --renderer rt64: only the Windows build is done\n");
        return false;
#endif
    }

    bool imgui_init() override {
        if (!ImGui_ImplSDL3_InitForOther(window())) return false;
        // the atlas made now, once, and handed to RT64's ImGui as its font texture
        ImGuiIO &io = ImGui::GetIO();
        unsigned char *pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.Fonts->SetTexID((ImTextureID)1);
        rt_atlas(reinterpret_cast<const uint32_t *>(pixels), w, h);
        return true;
    }

    void imgui_new_frame() override {}

    void draw_task(const uint8_t *rdram, uint32_t ucode, uint32_t ucode_data, uint32_t dl) override {
        if (up) rt_draw(rdram, ucode, ucode_data, dl);
    }

    void keep_range(uint32_t start, uint32_t bytes) override {
        if (up) rt_keep(start, bytes);
    }

    void present_game(const ElHostVi *vi, ImDrawData *overlay) override {
        if (!up) return;
        static_assert(sizeof(ElHostVi) == sizeof(ElRt64Vi), "the VI registers, in the same order");
        rt_present(reinterpret_cast<const ElRt64Vi *>(vi), flatten(overlay), 0);
    }

    // RT64 draws CPU screens from the exe's copy display list (port/gfx/host_bridge.c)
    void present_cpu(const uint16_t *, int, int, ImDrawData *) override {}

    void present_again(ImDrawData *overlay) override {
        if (up) rt_present(nullptr, flatten(overlay), 1);
    }

    uint32_t wires() const override {
        return FEATURE_RESOLUTION | FEATURE_MSAA | FEATURE_FRAME_RATE | FEATURE_THREE_POINT | FEATURE_SCALING;
    }

    uint32_t msaa_supported() const override { return up ? rt_msaa() : 0; }

    void graphics_changed() override {
        if (!up) return;
        ElRt64Graphics g = options();
        rt_graphics(&g);
    }

    void graphics_page_extra() override {
        ImGui::Spacing();
        ImGui::TextDisabled("Drawn by RT64 (--renderer rt64).");
    }

    void shutdown() override {
        if (up) rt_shutdown();
        up = false;
    }

private:
    static ElRt64Graphics options() {
        const Graphics &s = settings.graphics;
        ElRt64Graphics g = {};
        g.resolution = s.resolution;
        g.multiplier = s.resolution_scale;
        g.msaa = s.msaa;
        g.filtering = s.scaling;
        g.three_point = s.three_point;
        g.refresh = s.frame_rate;
        g.refresh_target = s.frame_rate_target;
        return g;
    }

    // ImGui's draw lists as one vertex and index array, in window pixels
    const ElRt64Overlay *flatten(ImDrawData *data) {
        if (!data || data->CmdListsCount == 0) return nullptr;
        vertices.clear();
        indices.clear();
        commands.clear();
        const ImVec2 scale = data->FramebufferScale, origin = data->DisplayPos;
        for (const ImDrawList *list : data->CmdLists) {
            const uint32_t base = (uint32_t)vertices.size();
            for (const ImDrawVert &v : list->VtxBuffer)
                vertices.push_back({ (v.pos.x - origin.x) * scale.x, (v.pos.y - origin.y) * scale.y, v.uv.x, v.uv.y, v.col });
            for (const ImDrawCmd &c : list->CmdBuffer) {
                if (c.UserCallback || c.ElemCount == 0) continue;
                ElRt64OverlayCommand oc;
                oc.clip[0] = (c.ClipRect.x - origin.x) * scale.x;
                oc.clip[1] = (c.ClipRect.y - origin.y) * scale.y;
                oc.clip[2] = (c.ClipRect.z - origin.x) * scale.x;
                oc.clip[3] = (c.ClipRect.w - origin.y) * scale.y;
                oc.first_index = (uint32_t)indices.size();
                oc.index_count = c.ElemCount;
                for (uint32_t i = 0; i < c.ElemCount; i++)
                    indices.push_back(base + c.VtxOffset + list->IdxBuffer[(int)(c.IdxOffset + i)]);
                commands.push_back(oc);
            }
        }
        overlay.width = data->DisplaySize.x * scale.x;
        overlay.height = data->DisplaySize.y * scale.y;
        overlay.vertices = vertices.data();
        overlay.indices = indices.data();
        overlay.commands = commands.data();
        overlay.vertex_count = (uint32_t)vertices.size();
        overlay.index_count = (uint32_t)indices.size();
        overlay.command_count = (uint32_t)commands.size();
        return &overlay;
    }

#ifdef _WIN32
    HMODULE lib = nullptr;
#endif
    bool up = false;
    int (*rt_init)(void *, uint32_t, const ElRt64Graphics *) = nullptr;
    void (*rt_atlas)(const uint32_t *, int, int) = nullptr;
    void (*rt_draw)(const uint8_t *, uint32_t, uint32_t, uint32_t) = nullptr;
    void (*rt_keep)(uint32_t, uint32_t) = nullptr;
    void (*rt_present)(const ElRt64Vi *, const ElRt64Overlay *, int) = nullptr;
    void (*rt_graphics)(const ElRt64Graphics *) = nullptr;
    uint32_t (*rt_msaa)(void) = nullptr;
    void (*rt_shutdown)(void) = nullptr;
    std::vector<ElRt64OverlayVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ElRt64OverlayCommand> commands;
    ElRt64Overlay overlay = {};
};

} // namespace

Renderer *make_rt64_renderer() { return new Rt64Renderer(); }

} // namespace host
