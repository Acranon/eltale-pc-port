/* The mouse in the game's menus (port/include/port/mouse_menu.h; docs/CONTROLS.md, "The mouse in menus").
 *
 * The game's drawing names its targets each frame (Port_MouseTarget at the draw sites, patches/p0.patch); the
 * next controller read (Port_MouseMenuRead) hit-tests the pointer against them, newest first (what is drawn
 * last is on top):
 * - the pointer moved onto a target: the target's cursor variable takes its value (or its function is told),
 *   so the game's own cursor and highlight go there and the game draws them as it always does;
 * - a left click on a target: the same, then the target's input (A, a C button, a stick push) is pressed for
 *   this one read, exactly as if the controller had pressed it;
 * - a right click (the host's: let go without a drag, so right-drag mouse look still works): B, on screens that
 *   take it (Port_MouseBack).
 * The Controller Pak manager draws with the CPU only when something changes, so its targets come from its
 * state here, and pointing steers its selection with stick pushes (its own redraw then follows).
 *
 * Nothing here runs without a pointer: no pointer, nothing changed (every game hash as before). */
#include "common.h"
#include "game/audio/sound.h"
#include "game/core/main.h"
#include "game/item/item.h"
#include "game/player/player.h"
#include "game/save/save_menu.h"
#include "game/text/text_window.h"
#include "game/ui/menu.h"
#include "game/native_draw/native_draw.h"
#include "port/mouse_menu.h"
#include "port/os.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TARGETS = 64 };

typedef struct Target {
    s16 tag, anchor;
    s16 x, y, w, h;
    void *var;
    s32 size, value;
    PortMouseFn fn;
    u32 click;
} Target;

/* what this frame's drawing names (building), and the last whole frame's (hit-tested) */
static Target building[MAX_TARGETS], shown[MAX_TARGETS];
static int building_count, shown_count;
static int building_back, shown_back;
static int frame_seen;

/* the pointer at this read and the last */
static float ptr_x, ptr_y;
static int ptr_width, ptr_flags;
static float last_x = -1.0f, last_y = -1.0f;
static int last_in;
static int hover_tag = -1;
static u32 injected;
static int log_on = -1;
static int log_after; /* a click was pressed at the last read: the state it led to, at this one */

/* the Controller Pak manager */
static int pak_running;
static int pak_goal_row = -1, pak_goal_choice = -1, pak_click, pak_wait, pak_steer_ttl;

void Port_MouseFrameBegin(void) {
    building_count = 0;
    building_back = 0;
    frame_seen = 1;
}

static Target *add(s32 tag, s32 anchor, s32 x, s32 y, s32 w, s32 h, u32 click) {
    Target *t;
    if (building_count == MAX_TARGETS || w <= 0 || h <= 0) return NULL;
    t = &building[building_count++];
    memset(t, 0, sizeof *t);
    t->tag = (s16)tag;
    t->anchor = (s16)anchor;
    t->x = (s16)x;
    t->y = (s16)y;
    t->w = (s16)w;
    t->h = (s16)h;
    t->click = click;
    return t;
}

void Port_MouseTarget(s32 tag, s32 anchor, s32 x, s32 y, s32 w, s32 h, void *var, s32 size, s32 value, u32 click) {
    Target *t = add(tag, anchor, x, y, w, h, click);
    if (!t) return;
    t->var = var;
    t->size = size;
    t->value = value;
}

void Port_MouseTargetFn(s32 tag, s32 anchor, s32 x, s32 y, s32 w, s32 h, PortMouseFn fn, s32 arg, u32 click) {
    Target *t = add(tag, anchor, x, y, w, h, click);
    if (!t) return;
    t->fn = fn;
    t->value = arg;
}

void Port_MouseBack(void) { building_back = 1; }

s32 Port_MouseHoverTag(void) { return hover_tag; }

void Port_MouseInject(u32 input) { injected |= input; }

void Port_MousePakManager(int running) {
    pak_running = running;
    pak_goal_row = pak_goal_choice = -1;
    pak_click = pak_wait = pak_steer_ttl = 0;
}

void Port_MousePointerSet(float x, float y, int width, int flags) {
    ptr_x = x;
    ptr_y = y;
    ptr_width = width;
    ptr_flags = flags;
}

/* the 1x picture's width: the host's, else from the shape the game draws at (an input script's pointer) */
static int picture_width(void) {
    if (pak_running) return 320; /* the Pak manager's 320x240 picture, pillarboxed */
    if (ptr_width > 0) return ptr_width;
    return 2 * (int)lroundf(ND_ASPECT() * 120.0f);
}

/* the pointer in an anchor's 320x240 units */
static void to_anchor(int anchor, float *x, float *y) {
    const float side = (float)(picture_width() - 320) * 0.5f;
    *x = ptr_x - (anchor == PORT_MOUSE_LEFT ? 0.0f : anchor == PORT_MOUSE_RIGHT ? 2.0f * side : side);
    *y = ptr_y;
}

static void set_var(const Target *t) {
    switch (t->size) {
    case 1: *(s8 *)t->var = (s8)t->value; break;
    case 2: *(s16 *)t->var = (s16)t->value; break;
    case 4: *(s32 *)t->var = t->value; break;
    default: break;
    }
}

static void press(u32 click, u16 *buttons, s8 *stick_x, s8 *stick_y) {
    *buttons |= (u16)click;
    if (click & PORT_MOUSE_STICK_LEFT) *stick_x = -80;
    if (click & PORT_MOUSE_STICK_RIGHT) *stick_x = 80;
    if (click & PORT_MOUSE_STICK_UP) *stick_y = 80;
    if (click & PORT_MOUSE_STICK_DOWN) *stick_y = -80;
}

/* ---- the Controller Pak manager (overlays/pakmenu: drawn by the CPU, redrawn when its state changes) ---- */
extern s16 pak_menu_state;
extern s16 pak_menu_selected_row;
extern s16 pak_menu_dialog_selection;
extern s16 pak_menu_cursor_x;
extern s16 pak_menu_cursor_y;

/* the two answers' cursor places (the cursor stands left of each word), or 0 for a list or a lone OK */
static int pak_choices(int *y, int *left, int *right) {
    switch (pak_menu_state) {
    case 1: *y = 0x36; *left = 0x4C; *right = 0xBA; return 1;
    case 3: case 5: case 8: case 9: case 10: *y = 0xA2; *left = 0x60; *right = 0xA6; return 1;
    case 4: *y = 0xC0; *left = 0x59; *right = 0x9C; return 1;
    default: return 0;
    }
}

static void pak_read(int moved, int left, int right, u16 *buttons, s8 *stick_x, s8 *stick_y) {
    float x = ptr_x, y = ptr_y;
    int cy, cl, cr, over = -1, step = 0;
    const int in = (ptr_flags & PORT_POINTER_IN) != 0;

    if (in && pak_choices(&cy, &cl, &cr)) {
        if (y >= cy - 4 && y < cy + 18) {
            if (x >= cl - 4 && x < cl + 64) over = 0;
            else if (x >= cr - 4 && x < cr + 64) over = 1;
        }
        if (over >= 0 && (moved || left)) {
            pak_goal_choice = over;
            pak_goal_row = -1;
            pak_steer_ttl = 30;
        }
    } else if (in && pak_menu_state == 2) {
        /* the notes list: six rows of 13 from y 0x58, the cursor at x 0x26 */
        if (x >= 0x20 && x < 0x128 && y >= 0x58 - 2 && y < 0x58 - 2 + 6 * 13) over = (int)(y - (0x58 - 2)) / 13;
        if (over >= 0 && (moved || left)) {
            pak_goal_row = over;
            pak_goal_choice = -1;
            pak_steer_ttl = 30;
        }
    } else if (in && pak_menu_state == 7) {
        over = 0; /* a lone OK: a click anywhere */
    }
    if (left && over >= 0) pak_click = 1;
    if (right) *buttons |= PORT_MOUSE_B;
    /* steer one push at a time (a push is seen when pressed: a read without it between) */
    if (pak_steer_ttl > 0) pak_steer_ttl--;
    else pak_goal_row = pak_goal_choice = -1, pak_click = 0;
    if (pak_wait) {
        pak_wait = 0;
        return;
    }
    if (pak_goal_choice >= 0 && pak_choices(&cy, &cl, &cr)) {
        if (pak_menu_dialog_selection != pak_goal_choice) step = pak_goal_choice ? 1 : -1, *stick_x = (s8)(step * 80);
    } else if (pak_goal_row >= 0 && pak_menu_state == 2) {
        if (pak_menu_selected_row < pak_goal_row) step = 1, *stick_y = -80;
        else if (pak_menu_selected_row > pak_goal_row) step = 1, *stick_y = 80;
    }
    if (step) {
        pak_wait = 1;
        return;
    }
    if (pak_click) {
        *buttons |= PORT_MOUSE_A;
        pak_click = 0;
        pak_goal_row = pak_goal_choice = -1;
        pak_wait = 1;
    }
}

/* ---- each read ----------------------------------------------------------------------------------------- */
void Port_MouseMenuRead(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    const int in = (ptr_flags & PORT_POINTER_IN) != 0;
    const int left = in && (ptr_flags & PORT_POINTER_LEFT);
    const int right = in && (ptr_flags & PORT_POINTER_RIGHT);
    const int moved = in && (!last_in || ptr_x != last_x || ptr_y != last_y);
    const u16 before = *buttons;
    const Target *over = NULL;
    int i;

    if (log_on < 0) log_on = getenv("ELTALE_MOUSE_LOG") ? atoi(getenv("ELTALE_MOUSE_LOG")) : 0;
    /* ELTALE_MOUSE_LOG: what a click picked (tools/mouse_menu_test.py checks it) */
    if (log_after) {
        log_after = 0;
        printf("P1_MOUSE_STATE vi=%llu mode=%d core_flags=%x menu_flags=%x item=%d+%d spell=%d:%d%d%d id=%x save=%x slot=%d choice=%d "
               "dialog=%d text_choice=%d text_delay=%d bgm=%d map=%.0f,%.0f title=%x pak=%d,%d,%d\n",
               (unsigned long long)Port_OsViCount(), core_game_mode, core_flags, menu_flags, item_menu_scroll, item_menu_cursor,
               player_state.spell_input_count, player_state.spell_input_dirs[0], player_state.spell_input_dirs[1],
               player_state.spell_input_dirs[2], player_state.spell_id, save_menu_flags, save_menu_selected_slot,
               save_menu_choice_cursor, save_menu_dialog_index, text_last_choice_index, menu_text_delay_option,
               menu_bgm_volume_option, menu_map_cursor_x, menu_map_cursor_y, title_menu_choice, pak_running ? pak_menu_state : -1,
               pak_menu_selected_row, pak_menu_dialog_selection);
    }
    if (frame_seen) {
        /* ELTALE_MOUSE_LOG=2: each new set of targets (where the menus are, for writing tests) */
        if (log_on > 1 && (building_count != shown_count || building_back != shown_back ||
                           memcmp(shown, building, sizeof(Target) * (size_t)building_count))) {
            printf("P1_MOUSE_TARGETS vi=%llu n=%d back=%d", (unsigned long long)Port_OsViCount(), building_count, building_back);
            for (i = 0; i < building_count; i++)
                printf(" %d:%x@%d,%d,%dx%d", building[i].anchor, building[i].tag, building[i].x, building[i].y, building[i].w,
                       building[i].h);
            printf("\n");
        }
        memcpy(shown, building, sizeof(Target) * (size_t)building_count);
        shown_count = building_count;
        shown_back = building_back;
        frame_seen = 0;
    }
    last_in = in;
    if (in) last_x = ptr_x, last_y = ptr_y;
    hover_tag = -1;
    injected = 0;
    if (!in && !ptr_flags) return;

    if (pak_running) {
        pak_read(moved, left, right, buttons, stick_x, stick_y);
    } else if (in) {
        for (i = shown_count - 1; i >= 0; i--) {
            const Target *t = &shown[i];
            float x, y;
            to_anchor(t->anchor, &x, &y);
            if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) {
                over = t;
                break;
            }
        }
        if (over) {
            float x, y;
            to_anchor(over->anchor, &x, &y);
            if (ptr_flags & PORT_POINTER_SHOWN) hover_tag = over->tag;
            if (moved || left) {
                if (over->var) set_var(over);
                if (log_on && moved)
                    printf("P1_MOUSE_HOVER vi=%llu pointer=%.1f,%.1f target=%d tag=%x value=%d\n", (unsigned long long)Port_OsViCount(),
                           ptr_x, ptr_y, (int)(over - shown), over->tag, over->var || over->fn ? over->value : -1);
                if (over->fn) over->fn(over->value, (s32)floorf(x), (s32)floorf(y), 1);
            }
            if (left) {
                if (over->fn) over->fn(over->value, (s32)floorf(x), (s32)floorf(y), 2);
                press(over->click, buttons, stick_x, stick_y);
            }
        }
        if (right && shown_back) *buttons |= PORT_MOUSE_B;
        if (injected) press(injected, buttons, stick_x, stick_y);
    }
    if (log_on && (left || right || *buttons != before)) log_after = 1;
    if (log_on && (left || right || *buttons != before))
        printf("P1_MOUSE_MENU vi=%llu pointer=%.1f,%.1f width=%d click=%s%s target=%d tag=%d buttons=%04x stick=%d,%d\n",
                (unsigned long long)Port_OsViCount(), ptr_x, ptr_y, picture_width(), left ? "L" : "", right ? "R" : "",
                over ? (int)(over - shown) : -1, over ? over->tag : -1, *buttons, *stick_x, *stick_y);
}
