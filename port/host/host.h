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
// The picture's switches. The presets only set them; each is its own switch in the F1 menu. A
// renderer wires what it can do (Renderer::wires); the rest are kept, shown, and wait for theirs.
struct Graphics {
    int resolution = 1;        // 0 original (320x240), 1 match the window, 2 a multiple
    int resolution_scale = 4;  // 1..8, for a multiple
    int msaa = 4;              // samples: 1 (off), 2, 4, 8
    int frame_rate = 2;        // 0 original (30 fps), 1 match the display, 2 fixed
    int frame_rate_target = 240;
    bool three_point = true;   // N64 three-point texture filtering
    int scaling = 2;           // to the window: 0 sharp pixels, 1 smooth, 2 smooth pixel edges
    bool vsync = true;
    bool widescreen = true;
    bool border = false;       // the N64's 8-pixel border (the game's 304x224 scissor)
    int draw_distance = 1;     // 0 original, 1 far
    bool n64_fog = false;      // the N64's fog (else per-pixel, modern)
    bool n64_dither = false;   // the N64's dither and its noise
    bool shader_warmup = true; // pipelines made at start (no hitch on a first visit)
    bool operator==(const Graphics &o) const;
};

enum Preset { PRESET_CLASSIC, PRESET_CLASSIC_VISUALS, PRESET_EVERYTHING, PRESET_CUSTOM, PRESET_COUNT };
extern const char *const preset_names[PRESET_COUNT];
Graphics preset_graphics(int preset);
int matching_preset(const Graphics &g); // PRESET_CUSTOM when none matches

struct Settings {
    int volume = 70; // percent
    bool mute_in_background = false;
    bool fullscreen = false;
    bool english = true; // the game's text: English, or the original Japanese
    int font = 0;        // the English font: 0 the game's own, then port/text/en_fonts.h's
    int preset = PRESET_EVERYTHING;
    Graphics graphics = preset_graphics(PRESET_EVERYTHING);
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

// ---- input.cpp ----------------------------------------------------------------------------------
void input_event(const SDL_Event &e); // controllers plugged in and out
void input_pad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);
void input_shutdown();

// ---- audio.cpp ----------------------------------------------------------------------------------
void audio_queue(const int16_t *samples, uint32_t bytes, uint32_t rate);
void audio_shutdown();

// ---- The renderers (renderer_rt64.cpp, port/render/render_native.cpp) ----------------------------
// Features a Graphics page may wire (Renderer::wires)
enum Feature : uint32_t {
    FEATURE_RESOLUTION = 1 << 0,
    FEATURE_MSAA = 1 << 1,
    FEATURE_FRAME_RATE = 1 << 2,
    FEATURE_THREE_POINT = 1 << 3,
    FEATURE_SCALING = 1 << 4,
    FEATURE_VSYNC = 1 << 5,
    FEATURE_WIDESCREEN = 1 << 6,
    FEATURE_BORDER = 1 << 7,
    FEATURE_DRAW_DISTANCE = 1 << 8,
    FEATURE_N64_FOG = 1 << 9,
    FEATURE_N64_DITHER = 1 << 10,
    FEATURE_SHADER_WARMUP = 1 << 11,
};

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
    virtual void draw_task(const uint8_t *, uint32_t, uint32_t, uint32_t) {}
    virtual void keep_range(uint32_t, uint32_t) {}
    // A picture: the game's frame, a CPU-drawn screen, or (the game paused under the menu) the last
    // picture again; `overlay` (null: none) goes over it.
    virtual void present_game(const ElHostVi *vi, ImDrawData *overlay) = 0;
    virtual void present_cpu(const uint16_t *pixels, int width, int height, ImDrawData *overlay) = 0;
    virtual void present_again(ImDrawData *overlay) = 0;
    // The game's thread is about to sleep `ns` nanoseconds until its next event (R6): a renderer that
    // shows pictures between the game's shows those that come due meanwhile, and returns in time.
    virtual void wait(uint64_t ns) { (void)ns; }
    // The Graphics switches this renderer acts on (Feature bits), and the MSAA counts it can do.
    virtual uint32_t wires() const = 0;
    virtual uint32_t msaa_supported() const { return 0; }
    // settings.graphics changed: apply it.
    virtual void graphics_changed() = 0;
    // Lines of its own at the end of the Graphics page (ImGui), if any.
    virtual void graphics_page_extra() {}
    virtual void shutdown() = 0;
};

Renderer *make_rt64_renderer();
Renderer *make_native_renderer();
Renderer *renderer();

// ---- menu.cpp: the F1 menu, the start-up hint and notices ---------------------------------------
namespace menu {
enum Request { NONE, QUIT, RESET, RESET_PAK_MANAGER };
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
// The Graphics page's shared rows (menu_graphics.cpp): presets and every switch, the ones the
// renderer doesn't wire yet shown as such.
void graphics_page();
} // namespace menu

} // namespace host
