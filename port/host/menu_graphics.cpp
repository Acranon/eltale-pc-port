// The F1 menu's Graphics page (host.h): the preset, then every switch. The presets only set the
// switches (docs/renderer/PLAN.md, decision 3). A switch the renderer in use acts on (its
// Renderer::wires) changes the picture at once; the others are kept in settings.json and shown as
// waiting for the native renderer's phase that brings them. The renderer adds lines of its own at the
// end (Renderer::graphics_page_extra).
#include "host.h"

#include "imgui.h"

namespace host {
namespace menu {
namespace {

void help(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// One switch's row: greyed and marked when the renderer doesn't act on it yet (it can still be set)
struct Row {
    bool wired;
    explicit Row(uint32_t feature) : wired((renderer()->wires() & feature) != 0) {
        if (!wired) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ~Row() {
        if (!wired) {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(later)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Kept, and used once the native renderer draws this (not with %s yet).", renderer()->name());
        }
    }
};

} // namespace

void graphics_page() {
    Graphics &g = settings.graphics;
    const Graphics before = g;

    bool full = settings.fullscreen;
    if (ImGui::Checkbox("Fullscreen (F11 or Alt+Enter)", &full)) {
        set_fullscreen(full);
        save_settings();
    }

    int preset = settings.preset;
    if (ImGui::Combo("Preset", &preset, preset_names, PRESET_COUNT) && preset != PRESET_CUSTOM) g = preset_graphics(preset);
    help("Classic: like the N64 (30 fps, 4:3, its border, filtering, fog and dither). Classic + visuals: a "
         "sharper, smoother picture, the game's look and timing kept. Everything on: 240 fps, widescreen, the lot. "
         "Each switch below can also be set alone.");
    ImGui::SeparatorText("Picture");

    {
        Row row(FEATURE_RESOLUTION);
        const char *names[] = { "Original (320x240)", "Match the window", "Multiple of the original" };
        ImGui::Combo("Resolution", &g.resolution, names, IM_ARRAYSIZE(names));
    }
    if (g.resolution == 2) {
        Row row(FEATURE_RESOLUTION);
        ImGui::SliderInt("Multiple", &g.resolution_scale, 1, 8, "%dx");
    }
    {
        Row row(FEATURE_MSAA);
        const char *names[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
        const int counts[] = { 1, 2, 4, 8 };
        const uint32_t supported = renderer()->msaa_supported();
        int at = g.msaa >= 8 ? 3 : g.msaa >= 4 ? 2 : g.msaa >= 2 ? 1 : 0;
        if (ImGui::BeginCombo("Anti-aliasing", names[at])) {
            for (int i = 0; i < 4; i++) {
                // (a count the GPU can't do is left out where the renderer knows)
                if (i && row.wired && supported && !(supported & counts[i])) continue;
                if (ImGui::Selectable(names[i], at == i)) g.msaa = counts[i];
            }
            ImGui::EndCombo();
        }
    }
    {
        Row row(FEATURE_SCALING);
        const char *names[] = { "Sharp pixels", "Smooth", "Smooth pixel edges" };
        ImGui::Combo("Scaling to the window", &g.scaling, names, IM_ARRAYSIZE(names));
    }
    {
        Row row(FEATURE_THREE_POINT);
        ImGui::Checkbox("N64 texture filtering (three-point)", &g.three_point);
    }
    {
        Row row(FEATURE_WIDESCREEN);
        ImGui::Checkbox("Widescreen", &g.widescreen);
    }
    {
        Row row(FEATURE_BORDER);
        ImGui::Checkbox("N64 border (8 pixels)", &g.border);
    }
    {
        Row row(FEATURE_DRAW_DISTANCE);
        const char *names[] = { "Original", "Far" };
        ImGui::Combo("Draw distance", &g.draw_distance, names, IM_ARRAYSIZE(names));
    }
    {
        Row row(FEATURE_N64_FOG);
        ImGui::Checkbox("N64 fog", &g.n64_fog);
    }
    {
        Row row(FEATURE_N64_DITHER);
        ImGui::Checkbox("N64 dither", &g.n64_dither);
    }

    ImGui::SeparatorText("Timing");
    {
        Row row(FEATURE_FRAME_RATE);
        const char *names[] = { "Original (30 fps)", "Match the display (smoothed)", "Fixed" };
        ImGui::Combo("Frame rate", &g.frame_rate, names, IM_ARRAYSIZE(names));
    }
    if (g.frame_rate == 2) {
        Row row(FEATURE_FRAME_RATE);
        ImGui::SliderInt("Frames per second", &g.frame_rate_target, 30, 240);
    }
    if (g.frame_rate != 0 && (renderer()->wires() & FEATURE_FRAME_RATE))
        help("Smoothing draws frames in between the game's own. It can look wrong in places: "
             "switch back to Original if something moves oddly.");
    {
        Row row(FEATURE_VSYNC);
        ImGui::Checkbox("Vsync", &g.vsync);
    }
    {
        Row row(FEATURE_SHADER_WARMUP);
        ImGui::Checkbox("Build shaders at start (no hitch on a first visit)", &g.shader_warmup);
    }

    renderer()->graphics_page_extra();

    if (!(g == before)) {
        settings.preset = matching_preset(g);
        renderer()->graphics_changed();
        save_settings();
    }
    // (choosing Custom alone changes nothing: it is whatever the switches say)
}

} // namespace menu
} // namespace host
