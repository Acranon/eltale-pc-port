// The pad (host.h): the keyboard and the first game controller, mapped to N64 buttons and stick.
// The bindings are fixed for now (the Controls page lists them); remapping comes later.
#include "host.h"

namespace host {
namespace {

SDL_Gamepad *pad;

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

void input_event(const SDL_Event &e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
    if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
}

// Keyboard: arrows or WASD the stick, X or Space A, C or Left Shift B, Z Z, Enter Start, Q L, E R,
// I/K/J/L the C buttons, T/G/F/H the D-pad. Controller: left stick and D-pad, A (south) A, X (west) B,
// left trigger Z, Start, shoulders L/R, right stick the C buttons.
void input_pad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    if (menu::is_open()) {
        *buttons = 0;
        *stick_x = *stick_y = 0;
        return;
    }
    const bool *k = SDL_GetKeyboardState(nullptr);
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
        auto btn = [](SDL_GamepadButton c) { return SDL_GetGamepadButton(pad, c); };
        if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) b |= A;
        if (btn(SDL_GAMEPAD_BUTTON_WEST)) b |= B;
        if (btn(SDL_GAMEPAD_BUTTON_START)) b |= START;
        if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= L;
        if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= R;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= DU;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= DD;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= DL;
        if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= DR;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= Z;
        int8_t rx = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), false);
        int8_t ry = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), true);
        if (ry > 40) b |= CU;
        if (ry < -40) b |= CD;
        if (rx < -40) b |= CL;
        if (rx > 40) b |= CR;
        int8_t lx = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), false);
        int8_t ly = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), true);
        if (lx || ly) { x = lx; y = ly; }
    }
    *buttons = b;
    *stick_x = (int8_t)x;
    *stick_y = (int8_t)y;
}

void input_shutdown() {
    if (pad) SDL_CloseGamepad(pad);
    pad = nullptr;
}

} // namespace host
