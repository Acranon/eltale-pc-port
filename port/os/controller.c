#include "common.h"
#include "port/controller.h"
#include "port/mouse_camera.h"
#include "port/mouse_menu.h"
#include "port/os.h"
#include "port/spell_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_INPUT_ROWS = 4096 };
typedef struct InputRow {
    uint64_t vi;
    u16 buttons;
    s8 stick_x;
    s8 stick_y;
} InputRow;
static InputRow rows[MAX_INPUT_ROWS];
static unsigned row_count, next_row;
/* `mouse VI DX DY WHEEL FLAGS [X Y]`: the mouse camera's input (port/mouse_camera.h) on the read at that VI, once
 * (turn counts, wheel steps, flags 1 = reset); not repeated. The menus' pointer (port/mouse_menu.h): flags 2 a
 * left click, 4 a right click, 8 the pointer gone (hidden, out of the window); X Y move it, in the 1x picture's
 * units (x across the picture as wide as its shape makes it: 320 at 4:3, 426 at 16:9; y down 240). Once given,
 * the pointer stays where it was put and is shown. For the spell UI's drags (port/spell_ui.h): flags 16 the left
 * button held from then on (18: pressed and held), 32 let go; a left click (2) without 16 is pressed and let go at
 * once. */
enum { MAX_MOUSE_ROWS = 4096 };
typedef struct MouseRow {
    uint64_t vi;
    float dx, dy;
    int wheel, flags;
    int has_xy;
    float x, y;
} MouseRow;
static MouseRow mouse_rows[MAX_MOUSE_ROWS];
static unsigned mouse_count, next_mouse;
/* an input script's pointer: where its rows put it (shown once placed) */
static float script_x, script_y;
static int script_pointer, script_held;
/* `spellkey VI MASK`: the spell UI's keys (port/spell_ui.h PORT_SPELLKEY_*) pressed at the read at that VI */
enum { MAX_KEY_ROWS = 1024 };
typedef struct KeyRow {
    uint64_t vi;
    unsigned mask;
} KeyRow;
static KeyRow key_rows[MAX_KEY_ROWS];
static unsigned key_count, next_key;

/* `repeat FROM PERIOD`: after the last row, the rows from VI FROM on play again every PERIOD VIs */
static unsigned repeat_first;
static uint64_t repeat_period, repeat_offset;
static OSContPad current_pad;
static u8 channels = 4;

int Port_ControllerLoadScript(const char *path) {
    row_count = next_row = 0;
    mouse_count = next_mouse = 0;
    key_count = next_key = 0;
    script_pointer = script_held = 0;
    repeat_period = repeat_offset = 0;
    memset(&current_pad, 0, sizeof current_pad);
    if (!path) return 0;
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1;
    }
    char line[256];
    unsigned lineno = 0;
    while (fgets(line, sizeof line, file)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned long long from, period;
        if (sscanf(line, "repeat %llu %llu", &from, &period) == 2) {
            repeat_first = 0;
            while (repeat_first < row_count && rows[repeat_first].vi < from) repeat_first++;
            if (repeat_first == row_count || !period || from + period <= rows[row_count - 1].vi) {
                fprintf(stderr, "input script %s:%u: repeat must follow its rows and span them\n", path, lineno);
                fclose(file);
                return -1;
            }
            repeat_period = period;
            continue;
        }
        unsigned long long vi;
        int kmask;
        if (sscanf(line, "spellkey %llu %i", &vi, &kmask) == 2) {
            if (key_count == MAX_KEY_ROWS || (key_count && vi <= key_rows[key_count - 1].vi)) {
                fprintf(stderr, "input script %s:%u: too many or unordered spellkey rows\n", path, lineno);
                fclose(file);
                return -1;
            }
            key_rows[key_count++] = (KeyRow){ (uint64_t)vi, (unsigned)kmask };
            continue;
        }
        int buttons;
        int x, y;
        float mdx, mdy;
        int wheel, mflags;
        float px, py;
        int n = sscanf(line, "mouse %llu %f %f %d %d %f %f", &vi, &mdx, &mdy, &wheel, &mflags, &px, &py);
        if (n == 5 || n == 7) {
            if (mouse_count == MAX_MOUSE_ROWS || (mouse_count && vi <= mouse_rows[mouse_count - 1].vi)) {
                fprintf(stderr, "input script %s:%u: too many or unordered mouse rows\n", path, lineno);
                fclose(file);
                return -1;
            }
            mouse_rows[mouse_count++] = (MouseRow){ (uint64_t)vi, mdx, mdy, wheel, mflags, n == 7, n == 7 ? px : 0.0f,
                                                    n == 7 ? py : 0.0f };
            continue;
        }
        if (sscanf(line, "%llu %i %d %d", &vi, &buttons, &x, &y) != 4 ||
            row_count == MAX_INPUT_ROWS || buttons < 0 || buttons > 0xFFFF ||
            x < -128 || x > 127 || y < -128 || y > 127 ||
            (row_count && vi <= rows[row_count - 1].vi)) {
            fprintf(stderr, "input script %s:%u: bad or unordered row\n", path, lineno);
            fclose(file);
            return -1;
        }
        rows[row_count++] = (InputRow){ (uint64_t)vi, (u16)buttons, (s8)x, (s8)y };
    }
    fclose(file);
    return 0;
}

/* Set by the host when a window gives live input (port/gfx/host_bridge.c). Used when no script
 * was loaded. */
void (*Port_LivePad)(u16 *buttons, s8 *stick_x, s8 *stick_y);
/* Set by the host too: the mouse camera's input since the last read (port/mouse_camera.h) */
void (*Port_LiveCamera)(float *dx, float *dy, int *wheel, int *flags);
/* and the menus' pointer (port/mouse_menu.h): picture units, the 1x picture's width, PORT_POINTER_* */
void (*Port_LivePointer)(float *x, float *y, int *width, int *flags);
/* and the spell UI's keys since the last read (port/spell_ui.h PORT_SPELLKEY_*) and whether it is on */
void (*Port_LiveSpellKeys)(unsigned *keys, int *on);
/* and, first, whether a screen that takes the pointer is shown (no mouse look over it: port/mouse_menu.h) */
void (*Port_LiveScreen)(int open);

static void update_camera(void) {
    float dx = 0.0f, dy = 0.0f;
    int wheel = 0, flags = 0;
    if (Port_LiveCamera && row_count == 0 && mouse_count == 0) {
        if (Port_LiveScreen) Port_LiveScreen(Port_MouseScreenOpen());
        Port_LiveCamera(&dx, &dy, &wheel, &flags);
        if (Port_LivePointer) {
            float x = 0.0f, y = 0.0f;
            int width = 0, pflags = 0;
            Port_LivePointer(&x, &y, &width, &pflags);
            Port_MousePointerSet(x, y, width, pflags);
        }
        if (Port_LiveSpellKeys) {
            unsigned keys = 0;
            int on = 0;
            Port_LiveSpellKeys(&keys, &on);
            Port_SpellUiKeys(keys, on);
        }
    } else {
        uint64_t now = Port_OsViCount();
        int clicks = 0;
        while (next_mouse < mouse_count && mouse_rows[next_mouse].vi <= now) {
            const MouseRow *m = &mouse_rows[next_mouse++];
            if (m->has_xy)
                fprintf(stderr, "P1_INPUT vi=%llu mouse=%g,%g wheel=%d flags=%d pointer=%g,%g\n", (unsigned long long)now,
                        m->dx, m->dy, m->wheel, m->flags, m->x, m->y);
            else
                fprintf(stderr, "P1_INPUT vi=%llu mouse=%g,%g wheel=%d flags=%d\n", (unsigned long long)now, m->dx, m->dy,
                        m->wheel, m->flags);
            dx += m->dx;
            dy += m->dy;
            wheel += m->wheel;
            flags |= m->flags & PORT_CAMERA_RESET;
            if (m->has_xy) {
                script_x = m->x;
                script_y = m->y;
                script_pointer = 1;
            }
            if (m->flags & 8) script_pointer = 0;
            clicks |= m->flags & 6;
            if (m->flags & 16) script_held = 1;
            else if ((m->flags & 2) && !script_held) clicks |= 32; /* a click: pressed and let go */
            if ((m->flags & 32) && script_held) script_held = 0, clicks |= 32;
        }
        /* flags 2 / 4: a left / right click; 16 held, 32 let go */
        Port_MousePointerSet(script_x, script_y, 0,
                             script_pointer ? PORT_POINTER_IN | PORT_POINTER_SHOWN | ((clicks & 2) ? PORT_POINTER_LEFT : 0) |
                                                  ((clicks & 4) ? PORT_POINTER_RIGHT : 0) | (script_held ? PORT_POINTER_HELD : 0) |
                                                  ((clicks & 32) ? PORT_POINTER_UP : 0)
                                            : 0);
        unsigned keys = 0;
        while (next_key < key_count && key_rows[next_key].vi <= now) {
            fprintf(stderr, "P1_INPUT vi=%llu spellkey=%#x\n", (unsigned long long)now, key_rows[next_key].mask);
            keys |= key_rows[next_key++].mask;
        }
        Port_SpellUiKeys(keys, -1);
    }
    wheel = Port_SpellUiWheel(wheel);
    Port_CameraInputSet(dx, dy, wheel, flags);
}

static void update_pad(void) {
    update_camera();
    if (Port_LivePad && row_count == 0) {
        u16 before = current_pad.button;
        Port_LivePad(&current_pad.button, &current_pad.stick_x, &current_pad.stick_y);
        if (current_pad.button != before)
            fprintf(stderr, "P1_INPUT live vi=%llu buttons=%04x\n", (unsigned long long)Port_OsViCount(),
                    current_pad.button);
        return;
    }
    uint64_t now = Port_OsViCount();
    for (;;) {
        if (next_row == row_count && repeat_period) {
            next_row = repeat_first;
            repeat_offset += repeat_period;
        }
        if (next_row >= row_count || rows[next_row].vi + repeat_offset > now) break;
        fprintf(stderr, "P1_INPUT vi=%llu buttons=%04x stick=%d,%d\n",
                (unsigned long long)now, rows[next_row].buttons,
                rows[next_row].stick_x, rows[next_row].stick_y);
        current_pad.button = rows[next_row].buttons;
        current_pad.stick_x = rows[next_row].stick_x;
        current_pad.stick_y = rows[next_row].stick_y;
        next_row++;
    }
}

s32 osContInit(OSMesgQueue *queue, u8 *pattern, OSContStatus *status) {
    (void)queue;
    *pattern = 1;
    memset(status, 0, 4 * sizeof *status);
    status[0].status = 1; /* controller with a virtual Controller Pak */
    return 0;
}

s32 osContSetCh(u8 n) { channels = n; return 0; }

s32 osContStartReadData(OSMesgQueue *queue) {
    (void)channels;
    osSendMesg(queue, NULL, OS_MESG_NOBLOCK);
    return 0;
}

/* Start held on the first read: the boot check that opens the Controller Pak manager. */
static int hold_start_at_boot;
void Port_ControllerHoldStartAtBoot(void) { hold_start_at_boot = 1; }

void osContGetReadData(OSContPad *pads) {
    update_pad();
    memset(pads, 0, 4 * sizeof *pads);
    pads[0] = current_pad;
    /* the spell UI's panel, bar and keys (port/spell_ui.h), then the menus' pointer: hover, and a click pressed for
     * this read (port/mouse_menu.h) */
    Port_SpellUiRead(&pads[0].button, &pads[0].stick_x, &pads[0].stick_y);
    Port_MouseMenuRead(&pads[0].button, &pads[0].stick_x, &pads[0].stick_y);
    if (hold_start_at_boot) {
        pads[0].button |= START_BUTTON;
        hold_start_at_boot = 0;
    }
}

s32 osContStartQuery(OSMesgQueue *queue) {
    osSendMesg(queue, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus *status) {
    memset(status, 0, 4 * sizeof *status);
    status[0].status = 1;
}
