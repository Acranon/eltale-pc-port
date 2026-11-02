// The F1 menu (F1 or the controller's Back button), the start-up hint and notices (host.h). Dear
// ImGui with its SDL3 backend; the renderer draws the lists (Renderer::imgui_init / present_*).
// The core pages are here (Game, Audio, Controls); the Graphics page is menu_graphics.cpp plus what
// each renderer adds. The game pauses while the menu is open (host.cpp's present loop).
#include "host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace host {
namespace menu {
namespace {

bool up;
bool open;
Request request = NONE;
uint64_t hint_until;  // the "F1: menu" hint at start-up, in SDL ticks
const char *confirm;  // the question a confirmation popup is asking, or null
Request confirm_request;
int quick_key;        // F2 (1) or F4 (2), until the game takes it
bool flash_key;       // F8, until the renderer takes it
std::mutex notice_mutex;
std::string notice_text;
uint64_t notice_until; // in SDL ticks

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
    help("Applies from the next text the game shows.");
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
    ImGui::TextWrapped("%s", data_dir().c_str());
#ifdef _WIN32
    if (ImGui::Button("Open the save folder", button))
        ShellExecuteW(nullptr, L"open", std::filesystem::u8path(data_dir()).wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
#endif
    ImGui::Spacing();
    if (ImGui::Button("Quit", button)) ask("Quit the game? Anything not saved is lost.", QUIT);
}

void audio_page() {
    bool changed = ImGui::SliderInt("Volume", &settings.volume, 0, 100, "%d%%");
    changed |= ImGui::Checkbox("Mute while the window is in the background", &settings.mute_in_background);
    if (changed) save_settings();
}

void controls_page() {
    int cap_action = -1, cap_slot = 0;
    const bool capturing = input_capturing(&cap_action, &cap_slot);
    if (capturing)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s: press a %s (Esc: cancel, Backspace: clear)",
                           action_label(cap_action), cap_slot == BIND_PAD_SLOT ? "controller button or stick" : "key or mouse button");
    else
        help("Click a binding, then press the key, mouse button or controller button to use. A key does one thing: "
             "binding it here takes it from where it was.");
    Controls &c = settings.controls;
    if (ImGui::SliderInt("Mouse camera speed", &c.mouse_sensitivity, 10, 400, "%d%%")) save_settings();
    if (ImGui::Checkbox("Invert the camera's up and down", &c.invert_y)) save_settings();
    if (ImGui::Checkbox("Mouse in menus", &c.mouse_menus)) save_settings();
    if (ImGui::Checkbox("Spell panel and quick bar", &c.spell_ui)) save_settings();
    help("While you put a spell together: the spell on a plate (click it to cast, right click takes the last element "
         "back), the four crystals (click or 1-4) and under each the spell it would make. The quick bar at the bottom "
         "keeps spells for each save: click an empty slot (or drag the plate onto one) to keep the spell you made, "
         "click a slot or press 5-0 to cast it; the wheel over the bar, , and . change its page. Off: the game's own "
         "spell panel.");
    if (ImGui::Checkbox("Click to aim spells", &c.spell_aim)) save_settings();
    help("In a battle, with a spell ready: point at a monster or the ground to see where the spell goes (its area, and a "
         "ring under every monster it would hit), click to turn that way and cast. The spell's reach is the game's own: "
         "move inside your ring first to get closer. Right click takes the last element back. Off: the game's own aim "
         "(the way you face).");
    if (!c.spell_aim) ImGui::BeginDisabled();
    ImGui::Indent();
    if (ImGui::Checkbox("Raise the camera while aiming", &c.spell_aim_camera)) save_settings();
    help("The view rises over the battlefield while you aim, so the areas on the ground are easy to read, and comes back "
         "down when you cast or take the spell back. Only the picture changes.");
    ImGui::Unindent();
    if (!c.spell_aim) ImGui::EndDisabled();
    if (ImGui::Checkbox("Unified pause menu", &c.pause_menu)) save_settings();
    help("Start (or Esc) opens one screen: the map of a region on the left (drag or the D-pad and stick to move it, "
         "the wheel, + and - or C up and down to zoom, the arrows, Z, L and R or C left and right for the other "
         "regions you have been to), your Status and Elements on the right, and Options (O, the controller's Y). "
         "Right click on the map (or A at the cross in the middle) puts your marker there, and again on it takes it "
         "away; while you are in its region the compass shows a cyan dot toward it. Off: the game's own four "
         "pages.");
    help("Point at a menu's options, items and answers to move the game's cursor there; a left click picks (A), a "
         "right click backs out (B). Click an element crystal at the bottom left for its spell list, then a spell's "
         "name to add it and the middle name to cast.");
    help("Mouse camera: hold the right button and move the mouse (or turn mouse look on) to turn the view, the "
         "wheel zooms, the middle button gives the camera back to the game. Movement follows the view. Not in "
         "scenes the game films itself.");
    if (ImGui::Button("Reset the controls to the defaults")) ask("Reset every control to its default?", RESET_CONTROLS);
    const float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 0.25f;
    if (ImGui::BeginTable("bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int a = 0; a < ACT_COUNT; a++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action_label(a));
            for (int slot = 0; slot < BIND_SLOTS; slot++) {
                ImGui::TableSetColumnIndex(slot + 1);
                ImGui::PushID(a * BIND_SLOTS + slot);
                const bool this_one = capturing && cap_action == a && cap_slot == slot;
                std::string label = this_one ? "..." : binding_label(c.bind[a][slot]);
                if (label.empty()) label = "-";
                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)) && !capturing) input_capture(a, slot);
                ImGui::PopID();
            }
        }
        // the port's own keys, fixed
        const char *fixed[][2] = {
            { "Menu", "F1 (controller: Back)" },
            { "Quick save / load", "F2 / F4" },
            { "Fullscreen", "F11 or Alt+Enter" },
            { "Flash recorder (debug)", "F8" },
            { "Move (controller)", "Left stick" },
        };
        for (auto &row : fixed) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", row[0]);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", row[1]);
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
            // ELTALE_MENU_PAGE=controls: open on that page (tests: ELTALE_MENU_TEST with ELTALE_PRESENT_DUMP)
            static const char *page = std::getenv("ELTALE_MENU_PAGE");
            static bool page_set;
            ImGuiTabItemFlags controls_flags = 0;
            if (page && !page_set && !std::strcmp(page, "controls")) {
                controls_flags = ImGuiTabItemFlags_SetSelected;
                page_set = true;
            }
            if (ImGui::BeginTabItem("Controls", nullptr, controls_flags)) { controls_page(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Map")) { automap::settings_page(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (confirm) ImGui::OpenPopup("Are you sure?");
    if (ImGui::BeginPopupModal("Are you sure?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(confirm ? confirm : "");
        if (ImGui::Button("Yes")) {
            if (confirm_request == RESET_CONTROLS) {
                settings.controls = Controls();
                save_settings();
            } else {
                request = confirm_request;
            }
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

// The start-up hint and the latest notice (quick save and load), top left.
void build_hint(bool hint, const std::string &note) {
    const float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (hint) ImGui::TextUnformatted("F1 or Back: menu   F2: quick save   F4: quick load");
    if (!note.empty()) ImGui::TextUnformatted(note.c_str());
    ImGui::End();
}

// ELTALE_MENU_TEST=N, for unattended runs (with ELTALE_HIDDEN_WINDOW): open the menu at the Nth
// picture, choose Reset to the Controller Pak manager 30 menu pictures later, Quit 300 after that.
// ELTALE_MENU_TEST=N:reset[:Q]: the plain Reset instead (the game boots again; an input script starts again
// too, so tests/title-new-game.input goes back into play), the flash recorder (F8) 24 pictures after it (the
// pictures shown around the reset, for tools/flash_find.py), and Quit Q pictures after it (300).
void test_tick() {
    static const char *env = std::getenv("ELTALE_MENU_TEST");
    static const char *plain = env ? std::strstr(env, ":reset") : nullptr;
    static const int quit_after = plain && plain[6] == ':' ? std::atoi(plain + 7) : 300;
    static int phase, n;
    if (!env || phase == 3) return;
    n++;
    if (phase == 0 && n >= std::atoi(env)) { open = true; phase = 1; n = 0; std::fprintf(stderr, "MENU_TEST open\n"); }
    else if (phase == 1 && n >= 30) {
        request = plain ? RESET : RESET_PAK_MANAGER;
        phase = 2;
        n = 0;
        std::fprintf(stderr, "MENU_TEST reset%s\n", plain ? " game" : "");
    } else if (phase == 2 && plain && n == 24) {
        flash_key = true;
    } else if (phase == 2 && n >= quit_after) { request = QUIT; phase = 3; std::fprintf(stderr, "MENU_TEST quit\n"); }
}

} // namespace

bool init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    int w = 0, h = 0;
    window_pixels(&w, &h);
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
    if (!renderer()->imgui_init()) {
        std::fprintf(stderr, "[host] menu: ImGui's renderer side could not be set up\n");
        ImGui::DestroyContext();
        return false;
    }
    up = true;
    hint_until = SDL_GetTicks() + 6000;
    return true;
}

void shutdown() {
    if (!up) return;
    up = false;
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool handle_event(const SDL_Event &e) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_F1 || (open && k == SDLK_ESCAPE && !confirm)) {
            open = !open;
            return true;
        }
        if (!open && k == SDLK_F8) {
            flash_key = true;
            return true;
        }
        if (!open && (k == SDLK_F2 || k == SDLK_F4)) {
            quick_key = k == SDLK_F2 ? 1 : 2;
            return true;
        }
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            set_fullscreen(!settings.fullscreen);
            save_settings();
            return true;
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
        open = !open;
        return true;
    }
    if (up && open) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        return true;
    }
    return false;
}

bool is_open() { return open && request == NONE; }

ImDrawData *frame(bool tick) {
    if (tick) test_tick();
    const uint64_t now = SDL_GetTicks();
    // (not over the area map's test pictures, ELTALE_AUTOMAP_SHOTS and ELTALE_AUTOMAP_DUMP: no hint, no notices)
    static const bool no_hint = std::getenv("ELTALE_AUTOMAP_SHOTS") != nullptr || std::getenv("ELTALE_AUTOMAP_DUMP") != nullptr;
    const bool hint = now < hint_until && !open && !no_hint;
    std::string note;
    {
        std::lock_guard<std::mutex> lock(notice_mutex);
        if (now < notice_until && !open && !no_hint) note = notice_text;
    }
    const bool map = automap::wants_overlay();
    if (!up || (!open && !hint && note.empty() && !map)) return nullptr;
    renderer()->imgui_new_frame();
    // the cursor is ImGui's only while the menu is open (closed: input.cpp shows and hides it)
    if (open) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (map) automap::draw(); // (under the menu: its own draw lists)
    if (open) build_menu();
    else if (hint || !note.empty()) build_hint(hint, note);
    ImGui::Render();
    return ImGui::GetDrawData();
}

bool take_flash_key() {
    const bool k = flash_key;
    flash_key = false;
    return k;
}

int take_quick_key() {
    int k = quick_key;
    quick_key = 0;
    return k;
}

void notice(const char *text) {
    std::lock_guard<std::mutex> lock(notice_mutex);
    notice_text = text ? text : "";
    notice_until = SDL_GetTicks() + 4000;
}

bool notice_shown() {
    std::lock_guard<std::mutex> lock(notice_mutex);
    return up && !open && SDL_GetTicks() < notice_until && !notice_text.empty();
}

Request take_request() {
    Request r = request;
    request = NONE;
    if (r != NONE) open = false;
    return r;
}

} // namespace menu
} // namespace host
