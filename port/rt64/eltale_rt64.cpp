// eltale_rt64.dll (eltale_rt64.h): RT64's HLE renderer on an SDL window, driven like the Quest 64
// recomp drives it (its src/main/rt64_render_context.cpp), from frame images built by the port.
#define ELTALE_RT64_BUILD
#define NOMINMAX // windows.h (via SDL_syswm.h) min/max macros break RT64 headers
#include "eltale_rt64.h"
#include "menu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include <SDL.h>
#include <SDL_syswm.h>

namespace {

constexpr uint32_t RDRAM_BYTES = 8 << 20;

uint8_t rdram[RDRAM_BYTES]; // RT64 reads RDRAM as host-order 32-bit words (byte n at n ^ 3)
uint8_t dmem[0x1000];
uint8_t imem[0x1000];
uint8_t rom_header[0x40];

unsigned int MI_INTR_REG, DPC_START_REG, DPC_END_REG, DPC_CURRENT_REG, DPC_STATUS_REG, DPC_CLOCK_REG,
    DPC_BUFBUSY_REG, DPC_PIPEBUSY_REG, DPC_TMEM_REG;
unsigned int VI_STATUS_REG, VI_ORIGIN_REG, VI_WIDTH_REG, VI_INTR_REG, VI_V_CURRENT_LINE_REG, VI_TIMING_REG,
    VI_V_SYNC_REG, VI_H_SYNC_REG, VI_LEAP_REG, VI_H_START_REG, VI_V_START_REG, VI_V_BURST_REG, VI_X_SCALE_REG,
    VI_Y_SCALE_REG;

SDL_Window *window;
SDL_GameController *pad;
SDL_AudioDeviceID audio_dev;
uint32_t audio_rate;
struct { uint32_t start, end; } keep[8];
unsigned keep_count;
std::unique_ptr<RT64::Application> app;

bool quit_requested;

void check_interrupts() {}

void pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit_requested = true;
        if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad &&
            e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
        menu::handle_event(e);
    }
}

} // namespace

extern "C" int ElRt64_Init(const char *title, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "RT64: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN |
                                  (std::getenv("ELTALE_HIDDEN_WINDOW") ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        std::fprintf(stderr, "RT64: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);

    RT64::Application::Core core{};
#ifdef _WIN32
    core.window = wm.info.win.window;
#else
    core.window.display = wm.info.x11.display;
    core.window.window = wm.info.x11.window;
#endif
    core.checkInterrupts = check_interrupts;
    core.HEADER = rom_header;
    core.RDRAM = rdram;
    core.DMEM = dmem;
    core.IMEM = imem;
    core.MI_INTR_REG = &MI_INTR_REG;
    core.DPC_START_REG = &DPC_START_REG;
    core.DPC_END_REG = &DPC_END_REG;
    core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    core.DPC_STATUS_REG = &DPC_STATUS_REG;
    core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    core.DPC_TMEM_REG = &DPC_TMEM_REG;
    core.VI_STATUS_REG = &VI_STATUS_REG;
    core.VI_ORIGIN_REG = &VI_ORIGIN_REG;
    core.VI_WIDTH_REG = &VI_WIDTH_REG;
    core.VI_INTR_REG = &VI_INTR_REG;
    core.VI_V_CURRENT_LINE_REG = &VI_V_CURRENT_LINE_REG;
    core.VI_TIMING_REG = &VI_TIMING_REG;
    core.VI_V_SYNC_REG = &VI_V_SYNC_REG;
    core.VI_H_SYNC_REG = &VI_H_SYNC_REG;
    core.VI_LEAP_REG = &VI_LEAP_REG;
    core.VI_H_START_REG = &VI_H_START_REG;
    core.VI_V_START_REG = &VI_V_START_REG;
    core.VI_V_BURST_REG = &VI_V_BURST_REG;
    core.VI_X_SCALE_REG = &VI_X_SCALE_REG;
    core.VI_Y_SCALE_REG = &VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration config;
    config.useConfigurationFile = false;
    app = std::make_unique<RT64::Application>(core, config);
    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
    app->enhancementConfig.f3dex.forceBranch = true;          // as the Quest 64 recomp sets it
    app->emulatorConfig.dither.postBlendNoiseNegative = true;
    menu::load_settings(app.get());
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = GetCurrentThreadId();
#endif
    if (app->setup(thread_id) != RT64::Application::SetupResult::Success) {
        std::fprintf(stderr, "RT64: setup failed\n");
        app.reset();
        return -1;
    }
    menu::init(app.get(), window);
    return 0;
}

extern "C" void ElRt64_DrawTask(const uint8_t *image, uint32_t ucode, uint32_t ucode_data, uint32_t dl) {
    if (!app) return;
    // big-endian bytes -> host-order words
    for (uint32_t i = 0; i < RDRAM_BYTES; i += 4) {
        bool kept = false;
        for (unsigned k = 0; k < keep_count; k++)
            if (i >= keep[k].start && i < keep[k].end) { i = keep[k].end - 4; kept = true; break; }
        if (kept) continue;
        uint32_t w = (uint32_t)image[i] << 24 | (uint32_t)image[i + 1] << 16 | (uint32_t)image[i + 2] << 8 | image[i + 3];
        std::memcpy(rdram + i, &w, 4);
    }
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(ucode & 0x3FFFFFF, ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, dl & 0x3FFFFFF, 0, true);
}

extern "C" void ElRt64_KeepRange(uint32_t start, uint32_t bytes) {
    if (keep_count < 8) keep[keep_count++] = { start & 0x00FFFFFC, (start & 0x00FFFFFC) + ((bytes + 3) & ~3u) };
}

extern "C" void ElRt64_Present(const ElRt64Vi *vi) {
    if (!app) return;
    VI_STATUS_REG = vi->status;
    VI_ORIGIN_REG = vi->origin;
    VI_WIDTH_REG = vi->width;
    VI_INTR_REG = vi->intr;
    VI_V_CURRENT_LINE_REG = vi->v_current;
    VI_TIMING_REG = vi->timing;
    VI_V_SYNC_REG = vi->v_sync;
    VI_H_SYNC_REG = vi->h_sync;
    VI_LEAP_REG = vi->leap;
    VI_H_START_REG = vi->h_start;
    VI_V_START_REG = vi->v_start;
    VI_V_BURST_REG = vi->v_burst;
    VI_X_SCALE_REG = vi->x_scale;
    VI_Y_SCALE_REG = vi->y_scale;
    menu::frame();
    app->updateScreen();
    // The game is paused while the menu is open. RT64 only presents when the picture changed, so it
    // is told it is paused (its debugger's pause), which makes it re-present the last frame each
    // time: the menu is redrawn over it.
    if (menu::is_open() && !quit_requested) {
        app->state->debuggerInspector.paused = true;
        while (menu::is_open() && !quit_requested) {
            SDL_Delay(15);
            pump_events();
            menu::frame();
            app->updateScreen();
        }
        app->state->debuggerInspector.paused = false;
    }
}

extern "C" int ElRt64_Poll(void) {
    pump_events();
    if (quit_requested) return 0;
    switch (menu::take_request()) {
    case menu::QUIT: quit_requested = true; return 0;
    case menu::RESET: return 2;
    case menu::RESET_PAK_MANAGER: return 3;
    default: return 1;
    }
}

extern "C" const char *ElRt64_DataDir(void) {
    static std::string dir = menu::data_dir().u8string();
    return dir.c_str();
}

extern "C" int ElRt64_Language(void) { return menu::english() ? 1 : 0; }

extern "C" int ElRt64_TextFont(void) { return menu::text_font(); }

extern "C" int ElRt64_TakeQuickKey(void) { return menu::take_quick_key(); }

extern "C" void ElRt64_Notice(const char *text) { menu::notice(text); }

namespace {
// N64 button bits (OSContPad.button)
enum : uint16_t { A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000, DU = 0x0800, DD = 0x0400, DL = 0x0200,
                  DR = 0x0100, L = 0x0020, R = 0x0010, CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001 };

int8_t axis(Sint16 v, bool invert) {
    int x = invert ? -(int)v : (int)v;
    if (x > -7000 && x < 7000) return 0; // dead zone
    x = x * 80 / 32767;
    return (int8_t)(x < -80 ? -80 : x > 80 ? 80 : x);
}
} // namespace

// Keyboard: arrows or WASD the stick, X or Space A, C or Left Shift B, Z Z, Enter Start, Q L, E R,
// I/K/J/L the C buttons, T/G/F/H the D-pad. Controller: left stick and D-pad, A A, X B, left
// trigger Z, Start, shoulders L/R, right stick the C buttons.
extern "C" void ElRt64_GetPad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    if (menu::is_open()) {
        *buttons = 0;
        *stick_x = *stick_y = 0;
        return;
    }
    const Uint8 *k = SDL_GetKeyboardState(nullptr);
    uint16_t b = 0;
    int x = 0, y = 0;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) x -= 80;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) x += 80;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) y += 80;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) y -= 80;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE]) b |= A;
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) b |= B;
    if (k[SDL_SCANCODE_Z]) b |= Z;
    if (k[SDL_SCANCODE_RETURN]) b |= START;
    if (k[SDL_SCANCODE_Q]) b |= L;
    if (k[SDL_SCANCODE_E]) b |= R;
    if (k[SDL_SCANCODE_I]) b |= CU;
    if (k[SDL_SCANCODE_K]) b |= CD;
    if (k[SDL_SCANCODE_J]) b |= CL;
    if (k[SDL_SCANCODE_L]) b |= CR;
    if (k[SDL_SCANCODE_T]) b |= DU;
    if (k[SDL_SCANCODE_G]) b |= DD;
    if (k[SDL_SCANCODE_F]) b |= DL;
    if (k[SDL_SCANCODE_H]) b |= DR;
    if (pad) {
        auto btn = [](SDL_GameControllerButton c) { return SDL_GameControllerGetButton(pad, c) != 0; };
        if (btn(SDL_CONTROLLER_BUTTON_A)) b |= A;
        if (btn(SDL_CONTROLLER_BUTTON_X)) b |= B;
        if (btn(SDL_CONTROLLER_BUTTON_START)) b |= START;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= R;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= DU;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= DD;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= DL;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= DR;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= Z;
        int8_t rx = axis(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), false);
        int8_t ry = axis(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY), true);
        if (ry > 40) b |= CU;
        if (ry < -40) b |= CD;
        if (rx < -40) b |= CL;
        if (rx > 40) b |= CR;
        int8_t lx = axis(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), false);
        int8_t ly = axis(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), true);
        if (lx || ly) { x = lx; y = ly; }
    }
    *buttons = b;
    *stick_x = (int8_t)x;
    *stick_y = (int8_t)y;
}

// Sound output. The game's AI buffers go into a ring that SDL's callback drains. Playback starts once
// the ring holds AUDIO_TARGET frames, and the callback steps through the ring a little faster or slower
// (at most 0.5%, inaudible) to keep it near that fill: the game is paced by the host clock and the
// sound card by its own, and a hitch on either side is absorbed instead of heard as a click. If the
// ring runs dry anyway, the output fades the last frame out and waits to refill.
namespace {

constexpr uint32_t AUDIO_RING = 1 << 15; // stereo frames (about 1.5 s at 22 kHz)
constexpr double AUDIO_MAX_STEP = 0.005;

struct AudioRing {
    int16_t frames[AUDIO_RING][2];
    uint64_t read_pos, write_pos; // in frames; read_pos is advanced by the callback
    double frac;                  // position between read_pos and read_pos + 1
    uint32_t target;              // fill to start at and to steer towards
    bool playing;
    float last[2];
} ring;

void audio_callback(void *, Uint8 *stream, int len) {
    int16_t *out = (int16_t *)stream;
    int count = len / 4;
    const float gain = menu::volume();
    uint64_t avail = ring.write_pos - ring.read_pos;
    if (!ring.playing && avail >= ring.target) ring.playing = true;
    double step = 1.0;
    if (ring.playing) {
        double error = ((double)avail - ring.target) / ring.target;
        step = 1.0 + std::max(-AUDIO_MAX_STEP, std::min(AUDIO_MAX_STEP, error * AUDIO_MAX_STEP));
    }
    for (int i = 0; i < count; i++) {
        if (ring.playing && ring.write_pos - ring.read_pos >= 2) {
            const int16_t *a = ring.frames[ring.read_pos % AUDIO_RING], *b = ring.frames[(ring.read_pos + 1) % AUDIO_RING];
            for (int c = 0; c < 2; c++) {
                ring.last[c] = (float)(a[c] + (b[c] - a[c]) * ring.frac) * gain;
                out[i * 2 + c] = (int16_t)ring.last[c];
            }
            ring.frac += step;
            while (ring.frac >= 1.0) { ring.frac -= 1.0; ring.read_pos++; }
        } else {
            ring.playing = false; // underrun: fade out over a few ms and refill
            for (int c = 0; c < 2; c++) {
                ring.last[c] *= 0.97f;
                out[i * 2 + c] = (int16_t)ring.last[c];
            }
        }
    }
}

} // namespace

extern "C" void ElRt64_QueueAudio(const int16_t *samples, uint32_t bytes, uint32_t rate) {
    if (rate != audio_rate) {
        if (audio_dev) SDL_CloseAudioDevice(audio_dev);
        SDL_AudioSpec want{}, have{};
        want.freq = (int)rate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = audio_callback;
        audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        audio_rate = rate;
        ring = {};
        ring.target = rate / 15; // about 67 ms: four game frames of sound
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
        else std::fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    }
    if (!audio_dev) return;
    uint32_t count = bytes / 4;
    SDL_LockAudioDevice(audio_dev);
    // far too much queued (the game ran ahead after a stall): drop the oldest down to the target
    if (ring.write_pos - ring.read_pos + count > AUDIO_RING / 2) ring.read_pos = ring.write_pos + count - ring.target;
    for (uint32_t i = 0; i < count; i++) {
        ring.frames[ring.write_pos % AUDIO_RING][0] = samples[i * 2];
        ring.frames[ring.write_pos % AUDIO_RING][1] = samples[i * 2 + 1];
        ring.write_pos++;
    }
    SDL_UnlockAudioDevice(audio_dev);
}

extern "C" void ElRt64_Shutdown(void) {
    if (app) app->end(); // RT64's deinit hook takes the menu's ImGui down with it
    app.reset();
    menu::shutdown();
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;
    if (pad) SDL_GameControllerClose(pad);
    pad = nullptr;
    if (window) SDL_DestroyWindow(window);
    window = nullptr;
    SDL_Quit();
}
