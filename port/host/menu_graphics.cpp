// The F1 menu's Graphics page (host.h): the preset, then every switch. The presets only set the
// switches (docs/renderer/PLAN.md, decision 3); each switch changes the picture at once and is kept in
// settings.json. The renderer adds lines of its own at the end (Renderer::graphics_page_extra).
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
        const char *names[] = { "Original (320x240)", "Match the window", "Multiple of the original" };
        ImGui::Combo("Resolution", &g.resolution, names, IM_ARRAYSIZE(names));
    }
    if (g.resolution == 2) {
        ImGui::SliderInt("Multiple", &g.resolution_scale, 1, 8, "%dx");
    }
    {
        const char *names[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
        const int counts[] = { 1, 2, 4, 8 };
        const uint32_t supported = renderer()->msaa_supported();
        int at = g.msaa >= 8 ? 3 : g.msaa >= 4 ? 2 : g.msaa >= 2 ? 1 : 0;
        if (ImGui::BeginCombo("Anti-aliasing", names[at])) {
            for (int i = 0; i < 4; i++) {
                // (a count the GPU can't do is left out)
                if (i && supported && !(supported & counts[i])) continue;
                if (ImGui::Selectable(names[i], at == i)) g.msaa = counts[i];
            }
            ImGui::EndCombo();
        }
    }
    {
        const char *names[] = { "Sharp pixels", "Smooth", "Smooth pixel edges" };
        ImGui::Combo("Scaling to the window", &g.scaling, names, IM_ARRAYSIZE(names));
    }
    {
        ImGui::Checkbox("N64 texture filtering (three-point)", &g.three_point);
    }
    {
        const char *names[] = { "Off (4:3)", "16:9", "Match the window" };
        ImGui::Combo("Widescreen", &g.widescreen, names, IM_ARRAYSIZE(names));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The 3D picture as wide as this; the HUD at the picture's edges; text, menus and full-screen "
                              "pictures stay 4:3 in the middle. Match the window: its shape, from 4:3 to 32:9.");
    }
    {
        ImGui::Checkbox("N64 border (8 pixels)", &g.border);
    }
    {
        const char *names[] = { "Original", "Far (2x)", "Farther (3x)" };
        ImGui::Combo("Draw distance", &g.draw_distance, names, IM_ARRAYSIZE(names));
    }
    if (g.draw_distance != 0)
        help("The fog and the far plane pushed out, and the map's objects drawn that far (drawing only: the game "
             "itself is unchanged).");
    {
        ImGui::Checkbox("N64 fog", &g.n64_fog);
    }
    {
        ImGui::Checkbox("N64 dither", &g.n64_dither);
    }

    ImGui::SeparatorText("Timing");
    {
        const char *names[] = { "Original (30 fps)", "Match the display (smoothed)", "Fixed" };
        ImGui::Combo("Frame rate", &g.frame_rate, names, IM_ARRAYSIZE(names));
    }
    if (g.frame_rate == 2) {
        ImGui::SliderInt("Frames per second", &g.frame_rate_target, 30, 240);
    }
    if (g.frame_rate != 0)
        help("Smoothing draws frames in between the game's own. It can look wrong in places: "
             "switch back to Original if something moves oddly.");
    {
        ImGui::Checkbox("Vsync", &g.vsync);
    }
    {
        ImGui::Checkbox("Build shaders at start (no hitch on a first visit)", &g.shader_warmup);
    }
    help("Takes effect the next time the game starts.");

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
