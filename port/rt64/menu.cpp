// The in-game menu (menu.h): ImGui drawn by RT64 over the game, through RT64's own ImGui setup
// (its Inspector) and its present hook.
#define NOMINMAX
#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>


#include "hle/rt64_application.h"
#include "gui/rt64_inspector.h"
#include "rhi/rt64_render_hooks.h"
#include "imgui/imgui.h"
#include <json/json.hpp>

#ifdef _WIN32
#include <shellapi.h>
#endif

using namespace plume;

namespace menu {
namespace {

using json = nlohmann::json;

struct Settings {
    int volume = 70; // percent
    bool mute_in_background = false;
    bool fullscreen = false;
    bool english = true; // the game's text: English, or the original Japanese
    int font = 0;        // the English font: 0 the game's own, then port/text/en_fonts.h's
};

Settings settings;
RT64::Application *app;
SDL_Window *window;
std::unique_ptr<RT64::Inspector> imgui;
std::atomic<bool> shown{false}; // the present thread draws the menu only while set
bool open;
bool focused = true;
Request request = NONE;
uint64_t hint_until; // the "F1: menu" hint at start-up, in SDL ticks
const char *confirm;  // the question a confirmation popup is asking, or null
Request confirm_request;

std::filesystem::path settings_path() { return data_dir() / "settings.json"; }

void save_settings() {
    json j;
    j["audio"] = { { "volume", settings.volume }, { "mute_in_background", settings.mute_in_background } };
    j["window"] = { { "fullscreen", settings.fullscreen } };
    j["game"] = { { "language", settings.english ? "en" : "jp" }, { "font", settings.font } };
    j["graphics"] = app->userConfig;
    std::ofstream f(settings_path());
    f << j.dump(4) << "\n";
}

// RT64 draws the menu after the game's picture, on the frame it presents.
void draw_hook(RenderCommandList *list, RenderFramebuffer *) {
    if (shown && imgui) imgui->draw(list);
}

void deinit_hook() { imgui.reset(); }

void set_fullscreen(bool on) {
    settings.fullscreen = on;
    SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void ask(const char *question, Request r) {
    confirm = question;
    confirm_request = r;
}

void help(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void game_page() {
    const ImVec2 button(ImGui::GetContentRegionAvail().x * 0.5f, 0);
    if (ImGui::Button("Resume", button)) open = false;
    ImGui::Spacing();
    int lang = settings.english ? 0 : 1;
    const char *langs[] = { "English", "Japanese (original)" };
    if (ImGui::Combo("Text", &lang, langs, IM_ARRAYSIZE(langs))) {
        settings.english = lang == 0;
        save_settings();
    }
    help("Applies from the next text the game shows. Pictures with text in them are still Japanese.");
    // the order of port/text/en_fonts.h (tools/text/gen_fonts.py), after the game's own
    const char *fonts[] = { "Original (the game's own)", "Royalty Mono", "Quest Square", "Pixel Sans", "Retro Sans",
                            "Rune Mono" };
    ImGui::BeginDisabled(!settings.english);
    if (ImGui::Combo("English font", &settings.font, fonts, IM_ARRAYSIZE(fonts))) save_settings();
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button("Reset game", button)) ask("Reset the game? Anything not saved is lost.", RESET);
    if (ImGui::Button("Reset to the Controller Pak manager", button))
        ask("Reset into the Controller Pak manager? Anything not saved is lost.", RESET_PAK_MANAGER);
    help("The game's own save manager (on the N64, Start held at power-on): lists the notes on the "
         "Controller Pak and deletes them.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Saves are kept in:");
    ImGui::TextWrapped("%s", data_dir().u8string().c_str());
#ifdef _WIN32
    if (ImGui::Button("Open the save folder", button))
        ShellExecuteW(nullptr, L"open", data_dir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    ImGui::Spacing();
    if (ImGui::Button("Quit", button)) ask("Quit the game? Anything not saved is lost.", QUIT);
}

void audio_page() {
    bool changed = ImGui::SliderInt("Volume", &settings.volume, 0, 100, "%d%%");
    changed |= ImGui::Checkbox("Mute while the window is in the background", &settings.mute_in_background);
    if (changed) save_settings();
}

void graphics_page() {
    RT64::UserConfiguration &cfg = app->userConfig;
    using RC = RT64::UserConfiguration;
    bool changed = false, resized = false;
    const uint32_t old_msaa = cfg.msaaSampleCount();

    bool full = settings.fullscreen;
    if (ImGui::Checkbox("Fullscreen (F11 or Alt+Enter)", &full)) {
        set_fullscreen(full);
        save_settings();
    }

    const char *resolutions[] = { "Original (320x240)", "Match the window", "Multiple of the original" };
    int res = int(cfg.resolution);
    if (ImGui::Combo("Resolution", &res, resolutions, IM_ARRAYSIZE(resolutions))) {
        cfg.resolution = RC::Resolution(res);
        resized = true;
    }
    if (cfg.resolution == RC::Resolution::Manual) {
        int mult = int(cfg.resolutionMultiplier + 0.5);
        if (ImGui::SliderInt("Multiple", &mult, 1, 8, "%dx")) {
            cfg.resolutionMultiplier = mult;
            resized = true;
        }
    }

    const char *aa_names[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
    const bool hdr = app->shaderLibrary->usesHDR;
    const RenderSampleCounts supported =
        app->device->getSampleCountsSupported(RT64::RenderTarget::colorBufferFormat(hdr)) &
        app->device->getSampleCountsSupported(RT64::RenderTarget::depthBufferFormat());
    ImGui::BeginDisabled(!app->device->getCapabilities().sampleLocations);
    if (ImGui::BeginCombo("Anti-aliasing", aa_names[int(cfg.antialiasing)])) {
        for (int i = 0; i < int(RC::Antialiasing::OptionCount); i++) {
            const RC::Antialiasing option = RC::Antialiasing(i);
            if (!(supported & RC::msaaSampleCount(option))) continue;
            if (ImGui::Selectable(aa_names[i], cfg.antialiasing == option)) {
                cfg.antialiasing = option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    const char *filters[] = { "Sharp pixels", "Smooth", "Smooth pixel edges" };
    int filter = int(cfg.filtering);
    if (ImGui::Combo("Scaling to the window", &filter, filters, IM_ARRAYSIZE(filters))) {
        cfg.filtering = RC::Filtering(filter);
        changed = true;
    }
    changed |= ImGui::Checkbox("N64 texture filtering (three-point)", &cfg.threePointFiltering);

    const char *rates[] = { "Original (30 fps)", "Match the display (smoothed)", "Fixed" };
    int rate = int(cfg.refreshRate);
    if (ImGui::Combo("Frame rate", &rate, rates, IM_ARRAYSIZE(rates))) {
        cfg.refreshRate = RC::RefreshRate(rate);
        changed = true;
    }
    if (cfg.refreshRate == RC::RefreshRate::Manual)
        changed |= ImGui::SliderInt("Frames per second", &cfg.refreshRateTarget, 30, 240);
    if (cfg.refreshRate != RC::RefreshRate::Original)
        help("Smoothing draws frames in between the game's own. It can look wrong in places: "
             "switch back to Original if something moves oddly.");

    if (changed || resized) {
        cfg.validate();
        if (cfg.msaaSampleCount() != old_msaa) app->updateMultisampling();
        app->updateUserConfig(resized);
        save_settings();
    }
}

void controls_page() {
    help("Remapping comes later. For now the controls are fixed:");
    if (ImGui::BeginTable("controls", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("N64");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        const char *rows[][3] = {
            { "Stick", "Arrow keys or WASD", "Left stick" },
            { "A", "X or Space", "A" },
            { "B", "C or Left Shift", "X" },
            { "Z", "Z", "Left trigger" },
            { "Start", "Enter", "Start" },
            { "L / R", "Q / E", "Shoulder buttons" },
            { "C buttons", "I J K L", "Right stick" },
            { "D-pad", "T F G H", "D-pad" },
            { "Menu", "F1", "Back" },
        };
        for (auto &row : rows) {
            ImGui::TableNextRow();
            for (int c = 0; c < 3; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(row[c]);
            }
        }
        ImGui::EndTable();
    }
}

void build_menu() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const ImVec2 size(std::min(io.DisplaySize.x * 0.9f, std::max(io.DisplaySize.x * 0.55f, ImGui::GetFontSize() * 34)),
                      io.DisplaySize.y * 0.75f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Eltale Monsters", &open, flags)) {
        if (ImGui::BeginTabBar("pages")) {
            if (ImGui::BeginTabItem("Game")) { game_page(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Audio")) { audio_page(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Graphics")) { graphics_page(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Controls")) { controls_page(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (confirm) ImGui::OpenPopup("Are you sure?");
    if (ImGui::BeginPopupModal("Are you sure?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(confirm ? confirm : "");
        if (ImGui::Button("Yes")) {
            request = confirm_request;
            confirm = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            confirm = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void build_hint() {
    const float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted("F1 or Back: menu");
    ImGui::End();
}

// ELTALE_MENU_TEST=N, for unattended runs (with ELTALE_HIDDEN_WINDOW): open the menu at the Nth
// frame, choose Reset to the Controller Pak manager 30 menu frames later, Quit 300 frames after.
void test_tick() {
    static const char *env = std::getenv("ELTALE_MENU_TEST");
    static int phase, n;
    if (!env || phase == 3) return;
    n++;
    if (phase == 0 && n >= std::atoi(env)) { open = true; phase = 1; n = 0; std::fprintf(stderr, "MENU_TEST open\n"); }
    else if (phase == 1 && n >= 30) { request = RESET_PAK_MANAGER; phase = 2; n = 0; std::fprintf(stderr, "MENU_TEST reset\n"); }
    else if (phase == 2 && n >= 300) { request = QUIT; phase = 3; std::fprintf(stderr, "MENU_TEST quit\n"); }
}

} // namespace

const std::filesystem::path &data_dir() {
    static std::filesystem::path dir;
    if (dir.empty()) {
        const char *env = std::getenv("ELTALE_DATA_DIR");
        const char *appdata = std::getenv("APPDATA");
        if (env && *env) dir = std::filesystem::u8path(env);
        else if (appdata && *appdata) dir = std::filesystem::u8path(appdata) / "eltale";
        else dir = "eltale-data";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    return dir;
}

void load_settings(RT64::Application *application) {
    app = application;
    std::ifstream f(settings_path());
    if (!f) return;
    try {
        json j = json::parse(f);
        settings.volume = std::clamp(j.value("/audio/volume"_json_pointer, settings.volume), 0, 100);
        settings.mute_in_background = j.value("/audio/mute_in_background"_json_pointer, settings.mute_in_background);
        settings.fullscreen = j.value("/window/fullscreen"_json_pointer, settings.fullscreen);
        settings.english = j.value("/game/language"_json_pointer, std::string("en")) != "jp";
        settings.font = std::clamp(j.value("/game/font"_json_pointer, 0), 0, 5);
        if (j.contains("graphics")) {
            RT64::UserConfiguration cfg = j["graphics"].get<RT64::UserConfiguration>();
            cfg.graphicsAPI = app->userConfig.graphicsAPI;
            cfg.developerMode = false;
            cfg.validate();
            app->userConfig = cfg;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "menu: ignoring %s: %s\n", settings_path().u8string().c_str(), e.what());
    }
}

void init(RT64::Application *application, SDL_Window *win) {
    app = application;
    window = win;
    imgui = std::make_unique<RT64::Inspector>(app->device.get(), app->swapChain.get(), app->chosenGraphicsAPI, window);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const float scale = std::clamp(h / 720.0f, 1.0f, 3.0f);
#ifdef _WIN32
    const char *font = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(font, 20.0f * scale);
    else io.FontGlobalScale = scale;
#else
    io.FontGlobalScale = scale;
#endif
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().WindowRounding = 6.0f * scale;
    RT64::SetRenderHooks(nullptr, draw_hook, deinit_hook);
    if (settings.fullscreen) set_fullscreen(true);
    hint_until = SDL_GetTicks64() + 6000;
}

void shutdown() {
    RT64::SetRenderHooks(nullptr, nullptr, nullptr);
    shown = false;
}

bool handle_event(const SDL_Event &e) {
    if (e.type == SDL_WINDOWEVENT) {
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = false;
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_F1 || (open && k == SDLK_ESCAPE && !confirm)) {
            open = !open;
            return true;
        }
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
            set_fullscreen(!settings.fullscreen);
            if (app) save_settings();
            return true;
        }
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
        open = !open;
        return true;
    }
    if (imgui && open) {
        imgui->handleSdlEvent(const_cast<SDL_Event *>(&e));
        return true;
    }
    return false;
}

bool is_open() { return open && request == NONE; }

void frame() {
    test_tick();
    const bool hint = SDL_GetTicks64() < hint_until && !open;
    if (!imgui || (!open && !hint)) {
        shown = false;
        return;
    }
    imgui->newFrame(app->framebufferGraphicsWorker.get());
    if (open) build_menu();
    else build_hint();
    imgui->endFrame();
    shown = true;
}

bool english() { return settings.english; }

int text_font() { return settings.font; }

float volume() {
    if (settings.mute_in_background && !focused) return 0.0f;
    return settings.volume / 100.0f;
}

Request take_request() {
    Request r = request;
    request = NONE;
    if (r != NONE) open = false;
    return r;
}

} // namespace menu
