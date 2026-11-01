// eltale_host.dll's C interface (eltale_host.h): the window, events and poll codes, and the present
// flow, with the game paused here while the F1 menu is open (its own loop, whichever renderer draws).
#include "host.h" // (ELTALE_HOST_BUILD: set by CMake, so the entry points are exported)
#include "nd_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace host {
namespace {

SDL_Window *win;
std::unique_ptr<Renderer> current;
int renderer_kind = -1;
bool quit_requested;
bool has_focus = true;

void pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit_requested = true;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) has_focus = true;
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) has_focus = false;
        input_event(e);
        menu::handle_event(e);
    }
}

// While the menu is open the game waits here: the last picture again, with the menu over it
void menu_pause() {
    while (menu::is_open() && !quit_requested) {
        SDL_Delay(15);
        pump_events();
        current->present_again(menu::frame());
    }
}

} // namespace

SDL_Window *window() { return win; }
Renderer *renderer() { return current.get(); }
bool focused() { return has_focus; }

void window_pixels(int *w, int *h) {
    *w = *h = 0;
    if (win) SDL_GetWindowSizeInPixels(win, w, h);
}

void set_fullscreen(bool on) {
    settings.fullscreen = on;
    if (win) SDL_SetWindowFullscreen(win, on);
}

} // namespace host

using namespace host;

extern "C" ELHOST_API int ElHost_Init(const char *title, int width, int height, int renderer) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    load_settings();
    current.reset(renderer == EL_RENDERER_NATIVE ? make_native_renderer() : make_rt64_renderer());
    renderer_kind = renderer == EL_RENDERER_NATIVE ? EL_RENDERER_NATIVE : EL_RENDERER_RT64;
    const bool hidden = std::getenv("ELTALE_HIDDEN_WINDOW") != nullptr;
    // R7: a 16:9 picture (the native renderer's widescreen) starts in a 16:9 window as high as the 4:3 one
    // would be, made smaller if the display's usable area is not that large
    if (renderer_kind == EL_RENDERER_NATIVE && settings.graphics.widescreen == 1) {
        width = height * 16 / 9;
        SDL_Rect area;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area) && area.w > 0 && area.h > 0) {
            const float k = std::min({ 1.0f, area.w * 0.95f / float(width), area.h * 0.9f / float(height) });
            width = int(float(width) * k);
            height = int(float(height) * k);
        }
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

extern "C" ELHOST_API int ElHost_Renderer(void) { return renderer_kind; }

extern "C" ELHOST_API void ElHost_DrawTask(const uint8_t *rdram, uint32_t ucode, uint32_t ucode_data, uint32_t dl) {
    if (current) current->draw_task(rdram, ucode, ucode_data, dl);
}

extern "C" ELHOST_API void ElHost_KeepRange(uint32_t start, uint32_t bytes) {
    if (current) current->keep_range(start, bytes);
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

extern "C" ELHOST_API void ElHost_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate) {
    audio_queue(samples, bytes, rate);
}

extern "C" ELHOST_API int ElHost_InitHeadless(void) {
    current.reset(make_native_renderer());
    renderer_kind = EL_RENDERER_NATIVE;
    if (!current->init_headless()) {
        std::fprintf(stderr, "[host] the native renderer could not be set up without a window\n");
        current.reset();
        return -1;
    }
    std::fprintf(stderr, "[host] native renderer, headless\n");
    return 0;
}

extern "C" ELHOST_API const void *ElHost_RenderApi(void) {
    return current && renderer_kind == EL_RENDERER_NATIVE ? render::native_draw().api() : nullptr;
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
