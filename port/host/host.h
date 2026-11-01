// The host's insides (eltale_host.h is its face to the game): settings, the window, input, sound,
// the F1 menu, and the renderer interface both renderers implement.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "eltale_host.h"

struct ImDrawData;

namespace host {

// ---- settings.cpp: the data folder and settings.json ---------------------------------------------
// The picture's switches. The presets only set them; each is its own switch in the F1 menu.
struct Graphics {
    int resolution = 1;        // 0 original (320x240), 1 match the window, 2 a multiple
    int resolution_scale = 4;  // 1..8, for a multiple
    int msaa = 4;              // samples: 1 (off), 2, 4, 8
    int frame_rate = 2;        // 0 original (30 fps), 1 match the display, 2 fixed
    int frame_rate_target = 240;
    bool three_point = true;   // N64 three-point texture filtering
    int scaling = 2;           // to the window: 0 sharp pixels, 1 smooth, 2 smooth pixel edges
    bool vsync = true;
    int widescreen = 1;        // the picture's shape (R7): 0 off (4:3), 1 16:9, 2 the window's (at least 4:3)
    bool border = false;       // the N64's 8-pixel border (the game's 304x224 scissor)
    int draw_distance = 1;     // 0 original (1x), 1 far (2x), 2 farther (3x): render_native.cpp distance_now
    bool n64_fog = false;      // the N64's fog (else per-pixel, modern)
    bool n64_dither = false;   // the N64's dither and its noise
    bool shader_warmup = true; // pipelines made at start (no hitch on a first visit)
    bool operator==(const Graphics &o) const;
};

enum Preset { PRESET_CLASSIC, PRESET_CLASSIC_VISUALS, PRESET_EVERYTHING, PRESET_CUSTOM, PRESET_COUNT };
extern const char *const preset_names[PRESET_COUNT];
Graphics preset_graphics(int preset);
int matching_preset(const Graphics &g); // PRESET_CUSTOM when none matches

// The controls (input.cpp, docs/CONTROLS.md): each action has three bindings, two keyboard/mouse slots and a
// controller one. A binding is a code: 0 none, else BIND_* | the SDL number.
enum Action {
    ACT_STICK_UP, ACT_STICK_DOWN, ACT_STICK_LEFT, ACT_STICK_RIGHT, ACT_WALK,
    ACT_A, ACT_B, ACT_Z, ACT_START, ACT_L, ACT_R,
    ACT_C_UP, ACT_C_DOWN, ACT_C_LEFT, ACT_C_RIGHT,
    ACT_D_UP, ACT_D_DOWN, ACT_D_LEFT, ACT_D_RIGHT,
    ACT_LOOK_HOLD, ACT_LOOK_TOGGLE, ACT_CAM_LEFT, ACT_CAM_RIGHT, ACT_CAM_UP, ACT_CAM_DOWN, ACT_ZOOM_IN, ACT_ZOOM_OUT,
    ACT_CAM_RESET,
    ACT_MAP, // the area map (automap.cpp)
    ACT_MAP_ZOOM_IN, ACT_MAP_ZOOM_OUT, // the minimap's reach (automap.cpp)
    ACT_COUNT
};
enum { BIND_SLOTS = 3, BIND_PAD_SLOT = 2 };
enum : int {
    BIND_KEY = 0x10000,       // | SDL_Scancode
    BIND_MOUSE = 0x20000,     // | SDL mouse button (1 left .. 5 X2)
    BIND_PAD_BUTTON = 0x30000, // | SDL_GamepadButton
    BIND_PAD_AXIS = 0x40000,  // | SDL_GamepadAxis << 1 | 1 for the negative way
    BIND_KIND = 0xFF0000,
};
struct Controls {
    int bind[ACT_COUNT][BIND_SLOTS];
    int mouse_sensitivity = 100; // percent
    bool invert_y = false;       // mouse and camera keys: up looks down
    Controls();                  // the defaults
};

// The area map (automap.cpp, docs/AUTOMAP.md): settings.json "map"
struct MapSettings {
    bool minimap = true;
    int corner = 1;       // the minimap's: 0 top right, 1 bottom right, 2 top left, 3 bottom left
    int size = 24;        // the minimap's side, percent of the picture's height (10..45)
    int reach = 800;      // the minimap's reach outdoors (an area with a sky), units from its centre to its edge (100..3000)
    int reach_in = 150;   // ... indoors (no sky: houses, castles, caves) (25..1500)
    bool fit_rooms = true; // a room smaller than the reach fills the minimap (and stays put while you walk in it)
    bool doors = true;    // the marks: doors and ways out
    bool people = true;   // ... people
    bool cheat_chests = false;  // the cheats: chests (what they hold, opened or not)
    bool cheat_spirits = false; // ... spirits
    bool cheat_bosses = false;  // ... the bosses standing in the field, and how near starts the fight
    int opacity = 85;     // the minimap's, percent (20..100)
    int background = 92;  // the full map's backdrop, percent (40..100)
    bool pause = false;   // the game waits while the full map is open
};

struct Settings {
    int volume = 70; // percent
    bool mute_in_background = false;
    bool fullscreen = false;
    bool english = true; // the game's text: English, or the original Japanese
    int font = 0;        // the English font: 0 the game's own, then port/text/en_fonts.h's
    int preset = PRESET_EVERYTHING;
    Graphics graphics = preset_graphics(PRESET_EVERYTHING);
    Controls controls;
    MapSettings map;
};
extern Settings settings;

// ELTALE_DATA_DIR, else %APPDATA%\eltale ($XDG_DATA_HOME/eltale, ~/.local/share/eltale), created if missing (UTF-8).
const std::string &data_dir();
void load_settings();
void save_settings();

// ---- host.cpp: the window ------------------------------------------------------------------------
SDL_Window *window();
void set_fullscreen(bool on);
bool focused();
// The window's size in pixels.
void window_pixels(int *w, int *h);
// The game cannot go on (the GPU device was lost): `message` to stderr and the data folder's error.log, a message
// box (not with ELTALE_HIDDEN_WINDOW), and the game asked to quit at its next poll, as the window's close does.
// Once only; true after it was called.
void fatal(const char *message);
bool fatal_called();

// ---- input.cpp ----------------------------------------------------------------------------------
// Every event: the keys, mouse buttons and controllers held, the mouse camera's motion, a rebinding. True
// when a rebinding took it (the menu must not see it).
bool input_event(const SDL_Event &e);
void input_pad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);
void input_camera(float *dx, float *dy, int *wheel, int *flags);
void input_shutdown();
// ELTALE_INPUT_TEST: queue the script's events that are due (SDL's own queue, this process only)
void input_test_pump();
// The Controls page: names, and rebinding (the next key, button or stick push goes to action's slot)
const char *action_label(int action);
const char *action_key(int action); // settings.json's name
std::string binding_name(int code);  // settings.json's
std::string binding_label(int code); // the menu's (friendlier controller names)
int binding_from_name(const char *name);
void input_capture(int action, int slot);
bool input_capturing(int *action = nullptr, int *slot = nullptr);

// ---- audio.cpp ----------------------------------------------------------------------------------
void audio_queue(const int16_t *samples, uint32_t bytes, uint32_t rate);
void audio_shutdown();

// ---- The renderer (port/render/render_native.cpp) ------------------------------------------------
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual const char *name() const = 0;
    // Set up on the window (made with window_flags()); false if it can't.
    virtual uint32_t window_flags() const = 0;
    virtual bool init(SDL_Window *window) = 0;
    // Without a window (the native renderer's headless pictures, ElHost_InitHeadless); false if it can't
    virtual bool init_headless() { return false; }
    // Dear ImGui's renderer side, after the context and fonts are made.
    virtual bool imgui_init() = 0;
    virtual void imgui_new_frame() = 0;
    // A picture: the game's frame, a CPU-drawn screen, or (the game paused under the menu) the last
    // picture again; `overlay` (null: none) goes over it.
    virtual void present_game(const ElHostVi *vi, ImDrawData *overlay) = 0;
    virtual void present_cpu(const uint16_t *pixels, int width, int height, ImDrawData *overlay) = 0;
    virtual void present_again(ImDrawData *overlay) = 0;
    // The game's thread is about to sleep `ns` nanoseconds until its next event (R6): a renderer that
    // shows pictures between the game's shows those that come due meanwhile, and returns in time.
    virtual void wait(uint64_t ns) { (void)ns; }
    // The MSAA counts the GPU can do (bits: 1, 2, 4, 8), 0 if not known.
    virtual uint32_t msaa_supported() const { return 0; }
    // settings.graphics changed: apply it.
    virtual void graphics_changed() = 0;
    // Lines of its own at the end of the Graphics page (ImGui), if any.
    virtual void graphics_page_extra() {}
    // The area map (automap.cpp): where the game's picture is in the window, in pixels (x0, y0, x1, y1; false: not
    // known), and a test's request to write the next picture shown as a PNG named `name` (ELTALE_PRESENT_DUMP's
    // folder)
    virtual bool picture_rect(float *rect) { (void)rect; return false; }
    virtual void request_dump(const char *name) { (void)name; }
    virtual bool dump_pending() const { return false; } // (asked, not written yet)
    virtual void shutdown() = 0;
};

Renderer *make_native_renderer();
Renderer *renderer();

// ---- automap.cpp: the area map ------------------------------------------------------------------
namespace automap {
// An event before the game's input sees it: true when the map took it (the open map's mouse, Esc)
bool event(const SDL_Event &e);
void toggle();            // the map key
bool full_open();         // the full map is shown (it takes the mouse)
void minimap_zoom(int steps); // the minimap's zoom keys: + in, - out (the reach of the area kind shown)
bool holds_game();        // ... and the game waits (settings.map.pause)
bool wants_overlay();     // something to draw this picture (the full map or the minimap)
void draw();              // into ImGui's frame (menu::frame calls it, under the menu)
void settings_page();     // the F1 menu's Map page
} // namespace automap

// ---- menu.cpp: the F1 menu, the start-up hint and notices ---------------------------------------
namespace menu {
enum Request { NONE, QUIT, RESET, RESET_PAK_MANAGER, RESET_CONTROLS /* (the menu's own) */ };
bool init();     // the ImGui context, fonts and style; the renderer's side (Renderer::imgui_init)
void shutdown();
bool handle_event(const SDL_Event &e); // true when the menu used it
bool is_open();
// This picture's overlay (the menu, the hint or a notice), or null when nothing is shown. tick false:
// a picture between two of the game's (R6), which ELTALE_MENU_TEST's picture count leaves out.
ImDrawData *frame(bool tick = true);
// F8 (the flash recorder) pressed since the last call
bool take_flash_key();
Request take_request();
int take_quick_key();
void notice(const char *text);
// a notice is on screen (ELTALE_PRESENT_DUMP's native_notice.png)
bool notice_shown();
// The Graphics page (menu_graphics.cpp): the preset and every switch.
void graphics_page();
} // namespace menu

} // namespace host
