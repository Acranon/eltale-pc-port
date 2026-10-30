// The in-game menu (F1 or the controller's Back button) and the settings it edits, kept in
// settings.json in the data folder. The game pauses while the menu is open.
#pragma once

#include <filesystem>

#include <SDL.h>

namespace RT64 { struct Application; }

namespace menu {

// What the player asked the menu for; ElRt64_Poll hands it to the game.
enum Request { NONE, QUIT, RESET, RESET_PAK_MANAGER };

// The data folder: ELTALE_DATA_DIR, else %APPDATA%\eltale (created if missing).
const std::filesystem::path &data_dir();

// Read settings.json and apply what can be applied before RT64 is set up (graphics options).
void load_settings(RT64::Application *app);

// Set up ImGui on RT64's device (after RT64's setup); applies the window settings.
void init(RT64::Application *app, SDL_Window *window);
void shutdown();

// A window event: returns true when the menu used it (the game should not see it).
bool handle_event(const SDL_Event &e);

bool is_open();

// Build this frame's menu (if anything is shown); call before RT64 presents.
void frame();

// The sound's volume now, 0..1 (setting, muted in the background).
float volume();

// Take the pending request (NONE if there is none).
Request take_request();

} // namespace menu
