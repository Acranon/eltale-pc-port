// The pad and the mouse camera (host.h; docs/CONTROLS.md): the keyboard, the mouse and the first game
// controller, mapped to N64 buttons and stick through settings.controls (rebound on the F1 menu's Controls
// page), and the mouse camera's input (port/mouse_camera.h on the game's side).
//
// Keys and mouse buttons held are tracked from the events (not SDL's keyboard state), so ELTALE_INPUT_TEST
// can drive this whole path with events put in SDL's own queue, in a hidden window, without touching the
// desktop's input.
#include "host.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace host {
namespace {

SDL_Gamepad *pad;

// N64 button bits (OSContPad.button)
enum : uint16_t { A = 0x8000, B = 0x4000, Z = 0x2000, START = 0x1000, DU = 0x0800, DD = 0x0400, DL = 0x0200,
                  DR = 0x0100, L = 0x0020, R = 0x0010, CU = 0x0008, CD = 0x0004, CL = 0x0002, CR = 0x0001 };

struct ActionInfo {
    const char *key, *label;
    uint16_t button; // the N64 button, or 0
};
const ActionInfo actions[ACT_COUNT] = {
    { "stick_up", "Move up (stick)", 0 },
    { "stick_down", "Move down (stick)", 0 },
    { "stick_left", "Move left (stick)", 0 },
    { "stick_right", "Move right (stick)", 0 },
    { "walk", "Walk (hold: half the stick)", 0 },
    { "a", "A", A },
    { "b", "B", B },
    { "z", "Z", Z },
    { "start", "Start", START },
    { "l", "L", L },
    { "r", "R", R },
    { "c_up", "C up", CU },
    { "c_down", "C down", CD },
    { "c_left", "C left", CL },
    { "c_right", "C right", CR },
    { "d_up", "D-pad up", DU },
    { "d_down", "D-pad down", DD },
    { "d_left", "D-pad left", DL },
    { "d_right", "D-pad right", DR },
    { "look_hold", "Mouse look (hold)", 0 },
    { "look_toggle", "Mouse look (on/off)", 0 },
    { "camera_left", "Camera left", 0 },
    { "camera_right", "Camera right", 0 },
    { "camera_up", "Camera up", 0 },
    { "camera_down", "Camera down", 0 },
    { "zoom_in", "Zoom in", 0 },
    { "zoom_out", "Zoom out", 0 },
    { "camera_reset", "Camera reset", 0 },
    { "map", "Area map (open/close)", 0 },
    { "map_zoom_in", "Minimap zoom in", 0 },
    { "map_zoom_out", "Minimap zoom out", 0 },
    { "map_zones", "Map: battle zones (hold, cheat)", 0 },
    { "spell_fire", "Spell UI: fire crystal", 0 },
    { "spell_earth", "Spell UI: earth crystal", 0 },
    { "spell_water", "Spell UI: water crystal", 0 },
    { "spell_wind", "Spell UI: wind crystal", 0 },
    { "slot_1", "Quick bar: slot 1", 0 },
    { "slot_2", "Quick bar: slot 2", 0 },
    { "slot_3", "Quick bar: slot 3", 0 },
    { "slot_4", "Quick bar: slot 4", 0 },
    { "slot_5", "Quick bar: slot 5", 0 },
    { "slot_6", "Quick bar: slot 6", 0 },
    { "page_prev", "Quick bar: page before", 0 },
    { "page_next", "Quick bar: page after", 0 },
    { "spell_clear", "Spell UI: take the elements back", 0 },
};

// The camera keys' turn per tick, in mouse counts (port/p0/mouse_camera.c: 0.006 rad a count across, 0.004 up
// and down): about 100 and 55 degrees a second at the game's 30 ticks
constexpr float KEY_TURN = 10.0f;
constexpr float KEY_PITCH = 8.0f;

bool keys[SDL_SCANCODE_COUNT];
bool mouse_buttons[8];
bool look_toggled;
// No mouse look while the game shows a menu, a text window or the spell panel (the game says so each read:
// input_screen). One opening turns mouse look off (the Tab toggle too) and blocks it until the screen is closed and
// every mouse-look-hold key and button is let go: it never comes back by itself.
bool screen_open, look_block;
bool relative_on;
float acc_dx, acc_dy, acc_wheel;
int reset_clicks, zoom_steps; // edges since the last tick
int capture_action = -1, capture_slot;
bool alt_enter_held;        // Alt+Enter pressed, Enter not let go yet
uint64_t alt_enter_until;    // SDL_GetTicks: Enter left alone until then
const bool hidden_window = std::getenv("ELTALE_HIDDEN_WINDOW") != nullptr;

// The menus' pointer (input_pointer; docs/CONTROLS.md "The mouse in menus"). A mouse button bound to mouse look
// (hold) turns the view only once it is dragged (DRAG_START counts): pressed and let go where it was, the right
// button is a click (B in the game's menus). The OS cursor shows when the mouse moves or clicks and hides on a key or
// controller press, in mouse look and after CURSOR_IDLE_MS without motion.
constexpr float DRAG_START = 3.0f;
constexpr uint64_t CURSOR_IDLE_MS = 3000;
bool pointer_known, pointer_inside = true;
float pointer_x, pointer_y; // window coordinates (SDL's)
int left_clicks, right_clicks, left_ups;
unsigned spell_keys;        // the spell UI's keys pressed since the last read (port/spell_ui.h)
int drag_button;            // the look-hold mouse button pressed (0: none)
bool drag_started;
float drag_dx, drag_dy;     // its motion before the drag started
bool cursor_visible = true;
uint64_t cursor_moved_at;

int8_t axis(Sint16 v, bool invert) {
    int x = invert ? -(int)v : (int)v;
    if (x > -7000 && x < 7000) return 0; // dead zone
    x = x * 80 / 32767;
    return (int8_t)(x < -80 ? -80 : x > 80 ? 80 : x);
}

bool code_held(int code) {
    const int n = code & 0xFFFF;
    switch (code & BIND_KIND) {
    case BIND_KEY: return n < SDL_SCANCODE_COUNT && keys[n];
    case BIND_MOUSE: return n < 8 && mouse_buttons[n];
    case BIND_PAD_BUTTON: return pad && SDL_GetGamepadButton(pad, (SDL_GamepadButton)n);
    case BIND_PAD_AXIS: {
        if (!pad) return false;
        const SDL_GamepadAxis ax = (SDL_GamepadAxis)(n >> 1);
        const Sint16 v = SDL_GetGamepadAxis(pad, ax);
        if (ax == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ax == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return !(n & 1) && v > 16000;
        const int8_t a = axis(v, false);
        return (n & 1) ? a < -40 : a > 40;
    }
    default: return false;
    }
}

bool held(int action) {
    for (int s = 0; s < BIND_SLOTS; s++)
        if (settings.controls.bind[action][s] && code_held(settings.controls.bind[action][s])) return true;
    return false;
}

// mouse look (hold): a key while held, a mouse button once dragged
bool look_held() {
    for (int s = 0; s < BIND_SLOTS; s++) {
        const int code = settings.controls.bind[ACT_LOOK_HOLD][s];
        if (!code || !code_held(code)) continue;
        if ((code & BIND_KIND) != BIND_MOUSE) return true;
        if (drag_started && drag_button == (code & 0xFFFF)) return true;
    }
    return false;
}

bool look_hold_button(int button) {
    for (int s = 0; s < BIND_SLOTS; s++)
        if (settings.controls.bind[ACT_LOOK_HOLD][s] == (BIND_MOUSE | button)) return true;
    return false;
}

void show_cursor(bool on) {
    if (on == cursor_visible) return;
    cursor_visible = on;
    // (a hidden window, a test's: the desktop's cursor is not ours)
    if (hidden_window || !window()) return;
    if (on) SDL_ShowCursor();
    else SDL_HideCursor();
}

// a press of `code`: the actions it is bound to that act on the press (the camera's edges)
void pressed(int code) {
    // the spell UI's keys: their bit in PORT_SPELLKEY_* order (crystals 0-3, slots 4-9, pages 10-11, clear 12)
    for (int a = ACT_SPELL_FIRE; a <= ACT_SPELL_CLEAR; a++)
        for (int s = 0; s < BIND_SLOTS; s++)
            if (settings.controls.bind[a][s] == code && !menu::is_open() && !automap::full_open())
                spell_keys |= 1u << (a - ACT_SPELL_FIRE);
    for (int a : { ACT_LOOK_TOGGLE, ACT_CAM_RESET, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_MAP, ACT_MAP_ZOOM_IN, ACT_MAP_ZOOM_OUT })
        for (int s = 0; s < BIND_SLOTS; s++)
            if (settings.controls.bind[a][s] == code) {
                if (menu::is_open()) continue;
                if (a == ACT_MAP) { automap::toggle(); continue; }
                if (a == ACT_MAP_ZOOM_IN || a == ACT_MAP_ZOOM_OUT) { automap::minimap_zoom(a == ACT_MAP_ZOOM_IN ? 1 : -1); continue; }
                if (automap::full_open() && a != ACT_LOOK_TOGGLE) continue; // (the open map has the mouse)
                if (a == ACT_LOOK_TOGGLE && screen_open) continue; // (no mouse look over a menu or the spell panel)
                if (a == ACT_LOOK_TOGGLE) look_toggled = !look_toggled;
                else if (a == ACT_CAM_RESET) reset_clicks++;
                else zoom_steps += a == ACT_ZOOM_IN ? 1 : -1;
            }
}

bool looking() {
    return (look_toggled || look_held()) && focused() && !menu::is_open() && !automap::full_open() && !look_block;
}

// a mouse-look-hold binding is down (a button whether dragged or not)
bool look_hold_down() {
    for (int s = 0; s < BIND_SLOTS; s++) {
        const int code = settings.controls.bind[ACT_LOOK_HOLD][s];
        if (code && code_held(code)) return true;
    }
    return false;
}

// the block ends once the screen is closed and the hold is let go: the player turns mouse look on again
void update_look_block() {
    if (look_block && !screen_open && !look_hold_down()) look_block = false;
}

// The cursor is captured (relative mode) while the mouse turns the camera. Never with a hidden window (tests):
// the desktop's mouse is not ours to take.
void update_capture() {
    const bool want = looking() && !hidden_window && window();
    if (want == relative_on) return;
    relative_on = want;
    SDL_SetWindowRelativeMouseMode(window(), want);
}

void clear_held() {
    std::memset(keys, 0, sizeof keys);
    std::memset(mouse_buttons, 0, sizeof mouse_buttons);
    drag_button = 0;
    drag_started = false;
}

// A rebinding: what this event binds (0: nothing), -1 to cancel, -2 to clear
int capture_code(const SDL_Event &e, bool pad_slot) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) return -1;
        if (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE) return -2;
        if (e.key.key >= SDLK_F1 && e.key.key <= SDLK_F12) return 0; // the port's own keys
        return pad_slot ? 0 : BIND_KEY | e.key.scancode;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !pad_slot && e.button.button < 8) return BIND_MOUSE | e.button.button;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) return -1; // the menu's button
        return pad_slot ? BIND_PAD_BUTTON | e.gbutton.button : 0;
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && pad_slot && (e.gaxis.value > 20000 || e.gaxis.value < -20000)) {
        const bool trigger = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (trigger && e.gaxis.value < 0) return 0;
        return BIND_PAD_AXIS | e.gaxis.axis << 1 | (e.gaxis.value < 0 ? 1 : 0);
    }
    return 0;
}

bool take_capture(const SDL_Event &e) {
    if (capture_action < 0) return false;
    const bool consumable = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
                            e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                            e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                            e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL;
    if (!consumable) return false;
    const int code = capture_code(e, capture_slot == BIND_PAD_SLOT);
    if (code == 0) return true;
    Controls &c = settings.controls;
    if (code == -2) c.bind[capture_action][capture_slot] = 0;
    else if (code > 0) {
        // one action a key: taken from any other place it was bound
        for (int a = 0; a < ACT_COUNT; a++)
            for (int s = 0; s < BIND_SLOTS; s++)
                if (c.bind[a][s] == code) c.bind[a][s] = 0;
        c.bind[capture_action][capture_slot] = code;
    }
    capture_action = -1;
    if (code != -1) save_settings();
    return true;
}

// The menus' pointer, its clicks and the cursor, from each event
void pointer_event(const SDL_Event &e, bool was_looking) {
    if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) pointer_inside = false;
    if (e.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
        pointer_inside = true;
        // where the pointer came back in, without waiting for it to move (not with mouse look's relative mode)
        if (!relative_on && window() && SDL_GetMouseFocus() == window()) {
            SDL_GetMouseState(&pointer_x, &pointer_y);
            pointer_known = true;
        }
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && !relative_on) {
        pointer_x = e.motion.x;
        pointer_y = e.motion.y;
        pointer_known = pointer_inside = true;
        if (!was_looking) {
            show_cursor(true);
            cursor_moved_at = SDL_GetTicks();
        }
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !menu::is_open() && capture_action < 0) {
        if (!relative_on) {
            pointer_x = e.button.x;
            pointer_y = e.button.y;
            pointer_known = pointer_inside = true;
        }
        if (look_hold_button(e.button.button) && !drag_button) {
            drag_button = e.button.button;
            drag_started = false;
            drag_dx = drag_dy = 0.0f;
        }
        if (e.button.button == SDL_BUTTON_LEFT && !was_looking) {
            left_clicks++;
            show_cursor(true);
            cursor_moved_at = SDL_GetTicks();
        }
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT && !menu::is_open() &&
        !automap::full_open() && capture_action < 0) { // (the open map's right click fits the map)
        // a right click: let go without a drag (mouse look on its button), or any (the button not mouse look's)
        const bool look_button = drag_button == SDL_BUTTON_RIGHT;
        if ((look_button && !drag_started && !look_toggled) || (!look_button && !look_hold_button(SDL_BUTTON_RIGHT) && !was_looking)) {
            right_clicks++;
            show_cursor(true);
            cursor_moved_at = SDL_GetTicks();
        }
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT && !menu::is_open() && capture_action < 0)
        left_ups++; // (the spell UI's drags and clicks act when the button is let go)
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == drag_button) {
        drag_button = 0;
        drag_started = false;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION && drag_button && !drag_started && !look_block) {
        drag_dx += e.motion.xrel;
        drag_dy += e.motion.yrel;
        if (std::fabs(drag_dx) + std::fabs(drag_dy) > DRAG_START) drag_started = true;
    }
    if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.value > 16000 || e.gaxis.value < -16000)))
        if (!menu::is_open()) show_cursor(false);
}

// ---- ELTALE_INPUT_TEST ----------------------------------------------------------------------------
// A script of events by tick (the count of pad reads): "TICK key_down NAME", "key_up NAME" (SDL scancode
// names: W, Space, Left Shift, Keypad 4, ...), "mouse_down left|middle|right|x1|x2", "mouse_up ...",
// "motion DX DY", "wheel N", "pointer X Y" (window coordinates), the window's "focus_lost", "focus_gained",
// "mouse_leave", "mouse_enter", and "system_focus 1|0|-", "system_mouse 1|0|-" (what SDL's window flags say from
// then on; -: the window's own). Lines starting with # are comments.
struct TestEvent {
    long tick;
    SDL_Event e;
};
std::vector<TestEvent> test_events;
Uint32 test_user_event; // the system_focus / system_mouse rows' event (SDL_RegisterEvents)
size_t test_next;
long pad_reads;

int mouse_button_from_name(const char *s) {
    const char *names[] = { "", "left", "middle", "right", "x1", "x2" };
    for (int i = 1; i < 6; i++)
        if (!std::strcmp(s, names[i])) return i;
    return 0;
}

void load_test_script() {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    const char *path = std::getenv("ELTALE_INPUT_TEST");
    if (!path || !*path) return;
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[host] ELTALE_INPUT_TEST: cannot read %s\n", path);
        return;
    }
    std::string line;
    int lineno = 0;
    float px = 0.0f, py = 0.0f; // the pointer where "pointer" rows put it (buttons and motion keep it there)
    while (std::getline(f, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        long tick;
        std::string cmd, rest;
        in >> tick >> cmd;
        std::getline(in, rest);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        SDL_Event e;
        std::memset(&e, 0, sizeof e);
        bool ok = true;
        if (cmd == "key_down" || cmd == "key_up") {
            // "KEY | alt repeat": the event's modifiers and repeat flag (as SDL would set them)
            std::string flags;
            if (const size_t bar = rest.find('|'); bar != std::string::npos) {
                flags = rest.substr(bar + 1);
                rest.erase(bar);
                while (!rest.empty() && rest.back() == ' ') rest.pop_back();
            }
            const SDL_Scancode sc = SDL_GetScancodeFromName(rest.c_str());
            ok = sc != SDL_SCANCODE_UNKNOWN;
            e.type = cmd == "key_down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.scancode = sc;
            e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            e.key.down = cmd == "key_down";
            if (flags.find("alt") != std::string::npos) e.key.mod |= SDL_KMOD_LALT;
            e.key.repeat = flags.find("repeat") != std::string::npos;
        } else if (cmd == "mouse_down" || cmd == "mouse_up") {
            const int b = mouse_button_from_name(rest.c_str());
            ok = b != 0;
            e.type = cmd == "mouse_down" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.button = (Uint8)b;
            e.button.down = cmd == "mouse_down";
            e.button.x = px;
            e.button.y = py;
        } else if (cmd == "motion") {
            e.type = SDL_EVENT_MOUSE_MOTION;
            ok = std::sscanf(rest.c_str(), "%f %f", &e.motion.xrel, &e.motion.yrel) == 2;
            e.motion.x = px;
            e.motion.y = py;
        } else if (cmd == "pointer") {
            // "pointer X Y": the mouse moved to X, Y in the window's coordinates (the menus' pointer)
            float x = 0.0f, y = 0.0f;
            ok = std::sscanf(rest.c_str(), "%f %f", &x, &y) == 2;
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.xrel = x - px;
            e.motion.yrel = y - py;
            e.motion.x = px = x;
            e.motion.y = py = y;
        } else if (cmd == "focus_lost" || cmd == "focus_gained" || cmd == "mouse_leave" || cmd == "mouse_enter") {
            // the window's own events, as the system sends them (a fullscreen switch can send one without the other)
            e.type = cmd == "focus_lost"     ? SDL_EVENT_WINDOW_FOCUS_LOST
                     : cmd == "focus_gained" ? SDL_EVENT_WINDOW_FOCUS_GAINED
                     : cmd == "mouse_leave"  ? SDL_EVENT_WINDOW_MOUSE_LEAVE
                                             : SDL_EVENT_WINDOW_MOUSE_ENTER;
        } else if (cmd == "system_focus" || cmd == "system_mouse") {
            // "system_focus 1|0|-": what SDL's window flags say from then on (-: ask the window, which a hidden one
            // never has), so a test can have the system say focused while the events said lost
            const int v = rest == "1" ? 1 : rest == "0" ? 0 : rest == "-" ? -1 : -3;
            ok = v != -3;
            if (!test_user_event) test_user_event = SDL_RegisterEvents(1);
            e.type = test_user_event;
            e.user.code = cmd == "system_focus" ? 0 : 1;
            e.user.data1 = reinterpret_cast<void *>(intptr_t(v));
        } else if (cmd == "wheel") {
            e.type = SDL_EVENT_MOUSE_WHEEL;
            ok = std::sscanf(rest.c_str(), "%d", &e.wheel.integer_y) == 1;
            e.wheel.y = (float)e.wheel.integer_y;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "[host] ELTALE_INPUT_TEST %s:%d: not understood: %s\n", path, lineno, line.c_str());
            continue;
        }
        if (window()) e.common.timestamp = SDL_GetTicksNS();
        if (window()) {
            const SDL_WindowID id = SDL_GetWindowID(window());
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) e.key.windowID = id;
            else if (e.type == SDL_EVENT_MOUSE_MOTION) e.motion.windowID = id;
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) e.wheel.windowID = id;
            else if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) e.window.windowID = id;
            else if (e.type == test_user_event) e.user.windowID = id;
            else e.button.windowID = id;
        }
        test_events.push_back({ tick, e });
    }
    std::fprintf(stderr, "[host] ELTALE_INPUT_TEST: %zu events\n", test_events.size());
}

} // namespace

bool action_held(int action) { return action >= 0 && action < ACT_COUNT && held(action); }

Controls::Controls() {
    std::memset(bind, 0, sizeof bind);
    auto key = [](SDL_Scancode s) { return BIND_KEY | s; };
    auto set = [this](int a, int k0, int k1, int p) { bind[a][0] = k0; bind[a][1] = k1; bind[a][BIND_PAD_SLOT] = p; };
    auto axis_code = [](SDL_GamepadAxis a, bool neg) { return BIND_PAD_AXIS | a << 1 | (neg ? 1 : 0); };
    // the keyboard after Quest 64 Enhanced's; the controller as it always was
    set(ACT_STICK_UP, key(SDL_SCANCODE_W), 0, 0);
    set(ACT_STICK_DOWN, key(SDL_SCANCODE_S), 0, 0);
    set(ACT_STICK_LEFT, key(SDL_SCANCODE_A), 0, 0);
    set(ACT_STICK_RIGHT, key(SDL_SCANCODE_D), 0, 0);
    set(ACT_WALK, key(SDL_SCANCODE_LCTRL), 0, 0);
    set(ACT_A, key(SDL_SCANCODE_SPACE), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_SOUTH);
    set(ACT_B, key(SDL_SCANCODE_LSHIFT), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_WEST);
    set(ACT_Z, key(SDL_SCANCODE_Q), 0, axis_code(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, false));
    set(ACT_START, key(SDL_SCANCODE_RETURN), key(SDL_SCANCODE_KP_ENTER), BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_START);
    set(ACT_L, key(SDL_SCANCODE_E), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    set(ACT_R, key(SDL_SCANCODE_R), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    set(ACT_C_UP, key(SDL_SCANCODE_UP), 0, axis_code(SDL_GAMEPAD_AXIS_RIGHTY, true));
    set(ACT_C_DOWN, key(SDL_SCANCODE_DOWN), 0, axis_code(SDL_GAMEPAD_AXIS_RIGHTY, false));
    set(ACT_C_LEFT, key(SDL_SCANCODE_LEFT), 0, axis_code(SDL_GAMEPAD_AXIS_RIGHTX, true));
    set(ACT_C_RIGHT, key(SDL_SCANCODE_RIGHT), 0, axis_code(SDL_GAMEPAD_AXIS_RIGHTX, false));
    set(ACT_D_UP, key(SDL_SCANCODE_I), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_DPAD_UP);
    set(ACT_D_DOWN, key(SDL_SCANCODE_K), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    set(ACT_D_LEFT, key(SDL_SCANCODE_J), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    set(ACT_D_RIGHT, key(SDL_SCANCODE_L), 0, BIND_PAD_BUTTON | SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    set(ACT_LOOK_HOLD, BIND_MOUSE | SDL_BUTTON_RIGHT, 0, 0);
    set(ACT_LOOK_TOGGLE, key(SDL_SCANCODE_TAB), 0, 0);
    set(ACT_CAM_LEFT, key(SDL_SCANCODE_Z), key(SDL_SCANCODE_KP_4), 0);
    set(ACT_CAM_RIGHT, key(SDL_SCANCODE_C), key(SDL_SCANCODE_KP_6), 0);
    set(ACT_CAM_UP, key(SDL_SCANCODE_PAGEUP), key(SDL_SCANCODE_KP_8), 0);
    set(ACT_CAM_DOWN, key(SDL_SCANCODE_PAGEDOWN), key(SDL_SCANCODE_KP_2), 0);
    set(ACT_ZOOM_IN, key(SDL_SCANCODE_EQUALS), key(SDL_SCANCODE_KP_PLUS), 0);
    set(ACT_ZOOM_OUT, key(SDL_SCANCODE_MINUS), key(SDL_SCANCODE_KP_MINUS), 0);
    set(ACT_CAM_RESET, BIND_MOUSE | SDL_BUTTON_MIDDLE, key(SDL_SCANCODE_KP_5), 0);
    set(ACT_MAP, key(SDL_SCANCODE_M), 0, 0);
    set(ACT_MAP_ZOOM_IN, key(SDL_SCANCODE_RIGHTBRACKET), 0, 0);
    set(ACT_MAP_ZOOM_OUT, key(SDL_SCANCODE_LEFTBRACKET), 0, 0);
    set(ACT_MAP_ZONES, key(SDL_SCANCODE_B), 0, 0);
    // the spell UI: 1-4 the crystals in their order on screen, 5-0 the quick bar's slots, , and . its pages, Esc
    set(ACT_SPELL_FIRE, key(SDL_SCANCODE_1), 0, 0);
    set(ACT_SPELL_EARTH, key(SDL_SCANCODE_2), 0, 0);
    set(ACT_SPELL_WATER, key(SDL_SCANCODE_3), 0, 0);
    set(ACT_SPELL_WIND, key(SDL_SCANCODE_4), 0, 0);
    set(ACT_SLOT_1, key(SDL_SCANCODE_5), 0, 0);
    set(ACT_SLOT_2, key(SDL_SCANCODE_6), 0, 0);
    set(ACT_SLOT_3, key(SDL_SCANCODE_7), 0, 0);
    set(ACT_SLOT_4, key(SDL_SCANCODE_8), 0, 0);
    set(ACT_SLOT_5, key(SDL_SCANCODE_9), 0, 0);
    set(ACT_SLOT_6, key(SDL_SCANCODE_0), 0, 0);
    set(ACT_PAGE_PREV, key(SDL_SCANCODE_COMMA), 0, 0);
    set(ACT_PAGE_NEXT, key(SDL_SCANCODE_PERIOD), 0, 0);
    set(ACT_SPELL_CLEAR, key(SDL_SCANCODE_ESCAPE), 0, 0);
}

const char *action_label(int a) { return a >= 0 && a < ACT_COUNT ? actions[a].label : ""; }
const char *action_key(int a) { return a >= 0 && a < ACT_COUNT ? actions[a].key : ""; }

std::string binding_name(int code) {
    const int n = code & 0xFFFF;
    switch (code & BIND_KIND) {
    case BIND_KEY: {
        const char *s = SDL_GetScancodeName((SDL_Scancode)n);
        return s && *s ? s : "Key " + std::to_string(n);
    }
    case BIND_MOUSE: {
        const char *names[] = { "?", "Left", "Middle", "Right", "X1", "X2", "6", "7" };
        return std::string("Mouse ") + names[n & 7];
    }
    case BIND_PAD_BUTTON: {
        const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)n);
        return std::string("Pad ") + (s ? s : "?");
    }
    case BIND_PAD_AXIS: {
        const char *s = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)(n >> 1));
        return std::string("Pad ") + ((n & 1) ? "-" : "+") + (s ? s : "?");
    }
    default: return "";
    }
}

std::string binding_label(int code) {
    const int n = code & 0xFFFF;
    if ((code & BIND_KIND) == BIND_PAD_BUTTON) {
        switch (n) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "Pad A (bottom)";
        case SDL_GAMEPAD_BUTTON_EAST: return "Pad B (right)";
        case SDL_GAMEPAD_BUTTON_WEST: return "Pad X (left)";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Pad Y (top)";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Pad left shoulder";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Pad right shoulder";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Pad D-pad up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Pad D-pad down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Pad D-pad left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Pad D-pad right";
        case SDL_GAMEPAD_BUTTON_START: return "Pad Start";
        default: break;
        }
    }
    if ((code & BIND_KIND) == BIND_PAD_AXIS) {
        const bool neg = n & 1;
        switch (n >> 1) {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "Pad left trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Pad right trigger";
        case SDL_GAMEPAD_AXIS_LEFTX: return neg ? "Pad left stick left" : "Pad left stick right";
        case SDL_GAMEPAD_AXIS_LEFTY: return neg ? "Pad left stick up" : "Pad left stick down";
        case SDL_GAMEPAD_AXIS_RIGHTX: return neg ? "Pad right stick left" : "Pad right stick right";
        case SDL_GAMEPAD_AXIS_RIGHTY: return neg ? "Pad right stick up" : "Pad right stick down";
        default: break;
        }
    }
    return binding_name(code);
}

int binding_from_name(const char *name) {
    if (!name || !*name) return 0;
    if (!std::strncmp(name, "Mouse ", 6)) {
        const char *names[] = { "?", "Left", "Middle", "Right", "X1", "X2" };
        for (int i = 1; i < 6; i++)
            if (!std::strcmp(name + 6, names[i])) return BIND_MOUSE | i;
        return 0;
    }
    if (!std::strncmp(name, "Pad ", 4)) {
        const char *p = name + 4;
        if (*p == '+' || *p == '-') {
            const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(p + 1);
            return a == SDL_GAMEPAD_AXIS_INVALID ? 0 : BIND_PAD_AXIS | a << 1 | (*p == '-' ? 1 : 0);
        }
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(p);
        return b == SDL_GAMEPAD_BUTTON_INVALID ? 0 : BIND_PAD_BUTTON | b;
    }
    const SDL_Scancode s = SDL_GetScancodeFromName(name);
    return s == SDL_SCANCODE_UNKNOWN ? 0 : BIND_KEY | s;
}

void input_capture(int action, int slot) {
    capture_action = action >= 0 && action < ACT_COUNT ? action : -1;
    capture_slot = slot < 0 || slot >= BIND_SLOTS ? 0 : slot;
}

bool input_capturing(int *action, int *slot) {
    if (action) *action = capture_action;
    if (slot) *slot = capture_slot;
    return capture_action >= 0;
}

bool input_event(const SDL_Event &e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
    if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    // Alt+Enter is the fullscreen switch (menu.cpp), never Start: from its press, Enter is not a key of the game's
    // until it is let go and a moment has passed. (Let go of Alt first and the key's repeats come without Alt;
    // the switch can move the focus, and SDL then sends the held Enter again as a new press.)
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
        alt_enter_held = true;
        alt_enter_until = SDL_GetTicks() + 400;
    }
    const bool alt_enter = (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) &&
                           e.key.scancode == SDL_SCANCODE_RETURN && (alt_enter_held || SDL_GetTicks() < alt_enter_until);
    if (alt_enter && e.type == SDL_EVENT_KEY_UP) alt_enter_held = false;
    // what is held, whatever else happens to the event
    if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.scancode < SDL_SCANCODE_COUNT) {
        if (alt_enter) keys[e.key.scancode] = false;
        else keys[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
    }
    if (test_user_event && e.type == test_user_event) {
        const int v = int(reinterpret_cast<intptr_t>(e.user.data1));
        set_test_system(e.user.code == 0 ? v : -2, e.user.code == 1 ? v : -2);
        return true;
    }
    const bool was_looking = looking();
    if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.button < 8)
        mouse_buttons[e.button.button] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    pointer_event(e, was_looking);
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        clear_held();
        look_toggled = false;
    }
    if (take_capture(e)) {
        update_capture();
        return true;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !alt_enter) pressed(BIND_KEY | e.key.scancode);
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) pressed(BIND_MOUSE | e.button.button);
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pressed(BIND_PAD_BUTTON | e.gbutton.button);
    if (e.type == SDL_EVENT_MOUSE_MOTION && looking()) {
        const float k = settings.controls.mouse_sensitivity / 100.0f;
        // a drag that just started: its motion so far too
        const float mx = was_looking ? e.motion.xrel : drag_dx, my = was_looking ? e.motion.yrel : drag_dy;
        acc_dx += mx * k;
        acc_dy += my * k * (settings.controls.invert_y ? -1.0f : 1.0f);
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL && focused() && !menu::is_open() && !automap::full_open())
        acc_wheel += (float)e.wheel.integer_y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
    update_capture();
    return false;
}

void input_test_pump() {
    load_test_script();
    while (test_next < test_events.size() && test_events[test_next].tick <= pad_reads) {
        SDL_Event e = test_events[test_next++].e;
        SDL_PushEvent(&e);
    }
}

// The keyboard and the mouse's buttons by settings.controls; the controller's left stick and D-pad, its
// buttons by settings.controls (by default: A (south) A, X (west) B, left trigger Z, Start, shoulders L/R,
// right stick the C buttons).
void input_screen(int open) {
    if (open && !screen_open) {
        if (looking()) {
            show_cursor(true); // (the pointer is back to point and click)
            cursor_moved_at = SDL_GetTicks();
        }
        look_toggled = false;
        look_block = true;
        drag_started = false;
    }
    screen_open = open != 0;
}

void input_pad(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    pad_reads++;
    update_look_block();
    {
        static int was = -1;
        static const bool log = std::getenv("ELTALE_MOUSE_LOG") != nullptr;
        const int now = looking() ? 1 : 0;
        if (log && now != was) std::fprintf(stderr, "P1_HOST_LOOK %s read=%ld screen=%d\n", now ? "on" : "off", pad_reads, screen_open ? 1 : 0);
        was = now;
    }
    update_capture();
    // the cursor: shown with the F1 menu; hidden in mouse look (relative mode hides it) and after a while still
    if (menu::is_open()) show_cursor(true);
    else if (cursor_visible && SDL_GetTicks() - cursor_moved_at > CURSOR_IDLE_MS && !relative_on) show_cursor(false);
    // (the F1 menu's ImGui may have shown it while it was open)
    if (!cursor_visible && !hidden_window && window() && !relative_on && !menu::is_open() && SDL_CursorVisible())
        SDL_HideCursor();
    if (menu::is_open()) {
        *buttons = 0;
        *stick_x = *stick_y = 0;
        return;
    }
    uint16_t b = 0;
    int x = 0, y = 0;
    const int full = held(ACT_WALK) ? 32 : 80;
    if (held(ACT_STICK_LEFT)) x -= full;
    if (held(ACT_STICK_RIGHT)) x += full;
    if (held(ACT_STICK_UP)) y += full;
    if (held(ACT_STICK_DOWN)) y -= full;
    for (int a = 0; a < ACT_COUNT; a++)
        if (actions[a].button && held(a)) b |= actions[a].button;
    if (pad) {
        int8_t lx = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), false);
        int8_t ly = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), true);
        if (lx || ly) { x = lx; y = ly; }
    }
    *buttons = b;
    *stick_x = (int8_t)x;
    *stick_y = (int8_t)y;
}

void input_camera(float *dx, float *dy, int *wheel, int *flags) {
    *dx = *dy = 0.0f;
    *wheel = *flags = 0;
    if (menu::is_open()) {
        acc_dx = acc_dy = acc_wheel = 0.0f;
        reset_clicks = zoom_steps = 0;
        return;
    }
    float x = acc_dx, y = acc_dy;
    acc_dx = acc_dy = 0.0f;
    const float inv = settings.controls.invert_y ? -1.0f : 1.0f;
    if (held(ACT_CAM_LEFT)) x -= KEY_TURN;
    if (held(ACT_CAM_RIGHT)) x += KEY_TURN;
    if (held(ACT_CAM_UP)) y -= KEY_PITCH * inv;
    if (held(ACT_CAM_DOWN)) y += KEY_PITCH * inv;
    const int w = (int)acc_wheel; // whole steps; a fraction waits
    acc_wheel -= (float)w;
    *dx = x;
    *dy = y;
    *wheel = w + zoom_steps;
    *flags = reset_clicks ? 1 : 0;
    reset_clicks = zoom_steps = 0;
}

void input_pointer(float *x, float *y, int *width, int *flags) {
    *x = *y = 0.0f;
    *width = 0;
    *flags = 0;
    const int left = left_clicks, right = right_clicks, up = left_ups;
    left_clicks = right_clicks = left_ups = 0;
    // (over the window: as SDL's flags say now, so a leave with no enter after it, as a fullscreen switch can send,
    // does not leave the pointer outside)
    const int inside_now = system_mouse_inside();
    const bool inside = inside_now >= 0 ? inside_now != 0 : pointer_inside;
    if (!settings.controls.mouse_menus || !pointer_known || !inside || !focused() || menu::is_open() ||
        automap::full_open() || looking() || !window() || !renderer()) // (the open map has the mouse)
        return;
    uint32_t pw = 0, ph = 0;
    bool cpu = false;
    if (!renderer()->shown_size(&pw, &ph, &cpu)) return;
    // the picture's place in the window, as the presenter fits it (present_vk.cpp fit: its own shape, as large as
    // fits, centred), in pixels; SDL's mouse is in window coordinates
    int ww = 0, wh = 0, pxw = 0, pxh = 0;
    SDL_GetWindowSize(window(), &ww, &wh);
    SDL_GetWindowSizeInPixels(window(), &pxw, &pxh);
    if (ww <= 0 || wh <= 0 || pxw <= 0 || pxh <= 0) return;
    int fw = pxw, fh = int(int64_t(pxw) * ph / pw);
    if (fh > pxh) {
        fh = pxh;
        fw = int(int64_t(pxh) * pw / ph);
    }
    const int fx = (pxw - fw) / 2, fy = (pxh - fh) / 2;
    const float mx = pointer_x * float(pxw) / float(ww), my = pointer_y * float(pxh) / float(wh);
    // the 1x picture: 240 high, as wide as its shape (a CPU-drawn screen: 320)
    const int w1 = cpu ? 320 : int(std::lround(240.0 * double(pw) / double(ph)));
    const float u = (mx - float(fx)) * float(w1) / float(fw), v = (my - float(fy)) * 240.0f / float(fh);
    if (u < 0.0f || v < 0.0f || u >= float(w1) || v >= 240.0f) return;
    *x = u;
    *y = v;
    *width = w1;
    *flags = 1 | (cursor_visible ? 2 : 0) | (left ? 4 : 0) | (right ? 8 : 0) | (mouse_buttons[SDL_BUTTON_LEFT] ? 16 : 0) |
             (up ? 32 : 0);
}

void input_spell_keys(unsigned *keys, int *on) {
    *keys = menu::is_open() ? 0 : spell_keys;
    spell_keys = 0;
    *on = settings.controls.spell_ui ? 1 : 0;
}

void input_shutdown() {
    if (relative_on && window()) SDL_SetWindowRelativeMouseMode(window(), false);
    relative_on = false;
    if (pad) SDL_CloseGamepad(pad);
    pad = nullptr;
}

} // namespace host
