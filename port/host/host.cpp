// eltale_host.dll's C interface (eltale_host.h): the window, events and poll codes, and the present
// flow, with the game paused here while the F1 menu is open (its own loop).
#include "host.h" // (ELTALE_HOST_BUILD: set by CMake, so the entry points are exported)
#include "nd_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

namespace host {
namespace {

SDL_Window *win;
std::unique_ptr<Renderer> current;
bool quit_requested;
bool has_focus = true; // (from the focus events: what a hidden window, a test's, goes by)
int test_system_focus = -1, test_system_mouse = -1; // ELTALE_INPUT_TEST's system_focus / system_mouse rows

// What SDL says of the window now: 1 or 0, or -1 when it cannot be asked (no window, or a hidden one: a test's,
// which the system never focuses; the events decide there)
int system_flag(int test, SDL_WindowFlags flag) {
    if (test >= 0) return test;
    if (!win) return -1;
    const SDL_WindowFlags f = SDL_GetWindowFlags(win);
    if (f & SDL_WINDOW_HIDDEN) return -1;
    return (f & flag) ? 1 : 0;
}

void pump_events() {
    input_test_pump();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit_requested = true;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) has_focus = true;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) has_focus = false;
        if (!menu::is_open() && automap::event(e)) continue; // (the open map's mouse; the menu comes first)
        if (!input_event(e)) menu::handle_event(e);
    }
}

// While the menu is open (or the area map, with its pause on) the game waits here: the last picture again, with the
// menu or the map over it
void menu_pause() {
    while ((menu::is_open() || automap::holds_game()) && !quit_requested) {
        SDL_Delay(15);
        pump_events();
        current->present_again(menu::frame());
    }
}

} // namespace

SDL_Window *window() { return win; }
Renderer *renderer() { return current.get(); }
// The window's focus as SDL has it now (its window flags), not only as the focus events said: a fullscreen switch
// can send a focus lost with no focus gained after it, and a flag kept from the events alone then stayed false (the
// mouse's clicks, its wheel and mouse look dead while the keys worked)
bool focused() {
    const int s = system_flag(test_system_focus, SDL_WINDOW_INPUT_FOCUS);
    return s >= 0 ? s != 0 : has_focus;
}

int system_mouse_inside() { return system_flag(test_system_mouse, SDL_WINDOW_MOUSE_FOCUS); }

void set_test_system(int focus, int mouse) {
    if (focus != -2) test_system_focus = focus;
    if (mouse != -2) test_system_mouse = mouse;
}

void window_pixels(int *w, int *h) {
    *w = *h = 0;
    if (win) SDL_GetWindowSizeInPixels(win, w, h);
}

namespace {
bool fatal_done;
}

void fatal(const char *message) {
    if (fatal_done) return;
    fatal_done = true;
    quit_requested = true;
    std::fprintf(stderr, "[host] fatal: %s The game ends.\n", message);
    std::fflush(stderr);
    if (FILE *f = std::fopen((data_dir() + "/error.log").c_str(), "a")) {
        char when[32] = "?";
        const std::time_t t = std::time(nullptr);
        if (const std::tm *lt = std::localtime(&t)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", lt);
        std::fprintf(f, "%s %s\n", when, message);
        std::fclose(f);
    }
    if (!std::getenv("ELTALE_HIDDEN_WINDOW")) {
        const std::string text = std::string(message) + "\n\nThe game has to close. Anything not saved is lost.\n(This was also written to " +
                                 data_dir() + "/error.log.)";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Eltale Monsters", text.c_str(), nullptr);
    }
}

bool fatal_called() { return fatal_done; }

void set_fullscreen(bool on) {
    settings.fullscreen = on;
    // (a hidden window, ELTALE_HIDDEN_WINDOW, is a test's: it stays hidden and windowed)
    if (win && !std::getenv("ELTALE_HIDDEN_WINDOW")) SDL_SetWindowFullscreen(win, on);
}

} // namespace host

using namespace host;

extern "C" ELHOST_API int ElHost_Init(const char *title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    load_settings();
    current.reset(make_native_renderer());
    const bool hidden = std::getenv("ELTALE_HIDDEN_WINDOW") != nullptr;
    // R7: a 16:9 picture (widescreen) starts in a 16:9 window as high as the 4:3 one would be, made smaller if
    // the display's usable area is not that large
    if (settings.graphics.widescreen == 1) {
        width = height * 16 / 9;
        SDL_Rect area;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area) && area.w > 0 && area.h > 0) {
            const float k = std::min({ 1.0f, area.w * 0.95f / float(width), area.h * 0.9f / float(height) });
            width = int(float(width) * k);
            height = int(float(height) * k);
        }
    }
    // ELTALE_WINDOW_SIZE=WxH (a test, with ELTALE_HIDDEN_WINDOW): the window's size in pixels
    if (const char *e = std::getenv("ELTALE_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) width = w, height = h;
    }
    win = SDL_CreateWindow(title, width, height,
                           SDL_WINDOW_RESIZABLE | current->window_flags() | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) {
        std::fprintf(stderr, "[host] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    if (!current->init(win)) {
        std::fprintf(stderr, "[host] the %s renderer could not be set up\n", current->name());
        current.reset();
        return -1;
    }
    if (!menu::init()) std::fprintf(stderr, "[host] no menu\n");
    if (settings.fullscreen) set_fullscreen(true);
    std::fprintf(stderr, "[host] %s renderer, data folder %s\n", current->name(), data_dir().c_str());
    return 0;
}

extern "C" ELHOST_API void ElHost_Present(const ElHostVi *vi) {
    if (!current) return;
    current->present_game(vi, menu::frame());
    menu_pause();
}

extern "C" ELHOST_API void ElHost_Wait(uint64_t ns) {
    if (current) current->wait(ns);
}

extern "C" ELHOST_API void ElHost_PresentCpuFrame(const uint16_t *pixels, int width, int height) {
    if (!current) return;
    current->present_cpu(pixels, width, height, menu::frame());
    menu_pause();
}

extern "C" ELHOST_API int ElHost_Poll(void) {
    pump_events();
    if (quit_requested) return EL_POLL_QUIT;
    switch (menu::take_request()) {
    case menu::QUIT: quit_requested = true; return EL_POLL_QUIT;
    case menu::RESET: return EL_POLL_RESET;
    case menu::RESET_PAK_MANAGER: return EL_POLL_RESET_PAK_MANAGER;
    default: return EL_POLL_RUN;
    }
}

extern "C" ELHOST_API const char *ElHost_DataDir(void) { return data_dir().c_str(); }

extern "C" ELHOST_API int ElHost_Language(void) { return settings.english ? 1 : 0; }

extern "C" ELHOST_API int ElHost_TextFont(void) { return settings.font; }

extern "C" ELHOST_API int ElHost_TakeQuickKey(void) { return menu::take_quick_key(); }

extern "C" ELHOST_API void ElHost_Notice(const char *text) { menu::notice(text); }

extern "C" ELHOST_API void ElHost_GetPad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    input_pad(buttons, stick_x, stick_y);
}

extern "C" ELHOST_API void ElHost_GetCamera(float *dx, float *dy, int *wheel, int *flags) {
    input_camera(dx, dy, wheel, flags);
}

extern "C" ELHOST_API void ElHost_GetPointer(float *x, float *y, int *width, int *flags) {
    input_pointer(x, y, width, flags);
}

extern "C" ELHOST_API void ElHost_GetSpellKeys(unsigned *keys, int *on) { input_spell_keys(keys, on); }

extern "C" ELHOST_API void ElHost_SetScreen(int open) { input_screen(open); }

extern "C" ELHOST_API void ElHost_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate) {
    audio_queue(samples, bytes, rate);
}

extern "C" ELHOST_API int ElHost_InitHeadless(void) {
    current.reset(make_native_renderer());
    if (!current->init_headless()) {
        std::fprintf(stderr, "[host] the native renderer could not be set up without a window\n");
        current.reset();
        return -1;
    }
    std::fprintf(stderr, "[host] native renderer, headless\n");
    return 0;
}

extern "C" ELHOST_API const void *ElHost_RenderApi(void) {
    return current ? render::native_draw().api() : nullptr;
}

extern "C" ELHOST_API void ElHost_SetDump(const char *dir, const char *spec) {
    render::native_draw().set_dump(dir ? dir : "", spec ? spec : "");
}

extern "C" ELHOST_API void ElHost_Shutdown(void) {
    if (!win) { /* headless: only the renderer was started */
        if (current) current->shutdown();
        current.reset();
        return;
    }
    audio_shutdown();
    if (current) current->shutdown();
    menu::shutdown();
    current.reset();
    input_shutdown();
    SDL_DestroyWindow(win);
    win = nullptr;
    SDL_Quit();
}
