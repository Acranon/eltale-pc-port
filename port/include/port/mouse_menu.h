/* The mouse in the game's menus (port/p0/mouse_menu.c, docs/CONTROLS.md "The mouse in menus").
 *
 * Each frame the game's drawing names what can be pointed at: a target is a rectangle in the 320x240 screen's
 * units (where the game draws it) with the anchor the native renderer places it by (nd_2d.h: CENTRE for menus
 * and text, LEFT / RIGHT for the HUD's corners), what pointing at it does (a cursor variable set to a value, or
 * a function) and what a left click on it presses (A, a C button, a stick push...). The next controller read
 * hit-tests the pointer against the last frame's targets: a pointer that moved onto a target moves the game's
 * own cursor there, and a click becomes that input for one read. A right click is B on screens that take B
 * (Port_MouseBack).
 *
 * The pointer comes in with the pad, once per controller read (port/os/controller.c): from the host
 * (ElHost_GetPointer: picture units, so every widescreen mode, scale, border and window shape map the same way)
 * or from an input script's `mouse` rows. Everything runs on the game's thread, before the game reads its
 * input, so a run with the same pointer rows is the same run; with no pointer nothing here changes anything. */
#ifndef ELTALE_PORT_MOUSE_MENU_H
#define ELTALE_PORT_MOUSE_MENU_H

#include "common.h"

/* where a target's rectangle is placed on a wider picture (the native renderer's EL_RD2D_ANCHOR_*) */
enum { PORT_MOUSE_CENTRE, PORT_MOUSE_LEFT, PORT_MOUSE_RIGHT };

/* what a click presses (one read): N64 button bits, and a full stick push */
#define PORT_MOUSE_STICK_LEFT 0x10000u
#define PORT_MOUSE_STICK_RIGHT 0x20000u
#define PORT_MOUSE_STICK_UP 0x40000u
#define PORT_MOUSE_STICK_DOWN 0x80000u
#define PORT_MOUSE_A 0x8000u
#define PORT_MOUSE_B 0x4000u
#define PORT_MOUSE_START 0x1000u

/* the pointer's flags (ElHost_GetPointer, an input script's mouse row) */
enum {
    PORT_POINTER_IN = 1,     /* over the picture, the mouse in menus on, mouse look off */
    PORT_POINTER_SHOWN = 2,  /* the OS cursor is shown (hover highlights) */
    PORT_POINTER_LEFT = 4,   /* a left click since the last read */
    PORT_POINTER_RIGHT = 8,  /* a right click (pressed and let go without a drag) since the last read */
    PORT_POINTER_HELD = 16,  /* the left button is down now (the spell UI's drags) */
    PORT_POINTER_UP = 32,    /* the left button was let go since the last read */
};

/* Tags the HUD asks about (Port_MouseHoverTag) to draw a hover highlight; other targets use 0. */
enum {
    PORT_TAG_CRYSTAL = 0x100,     /* + element 0..3: the HUD's element crystals */
    PORT_TAG_SPELL = 0x110,       /* + direction 0..3: the spell list's names around the cross */
    PORT_TAG_SPELL_CAST = 0x120,  /* the spell being put together (A casts it) */
    PORT_TAG_ALLOCATE = 0x130,    /* + element 0..3: the element allocation cross */
};

/* A function target: event 1 the pointer moved over it, 2 a left click on it (before its click is pressed);
 * x, y the pointer in the target's anchor's 320x240 units. */
typedef void (*PortMouseFn)(s32 arg, s32 x, s32 y, s32 event);

/* the start of each frame's drawing (nd_frame_begin): the targets named from here on replace the last frame's */
void Port_MouseFrameBegin(void);
/* a cursor target: the pointer over it sets the `size`-byte variable `var` (if any) to `value` */
void Port_MouseTarget(s32 tag, s32 anchor, s32 x, s32 y, s32 w, s32 h, void *var, s32 size, s32 value, u32 click);
void Port_MouseTargetFn(s32 tag, s32 anchor, s32 x, s32 y, s32 w, s32 h, PortMouseFn fn, s32 arg, u32 click);
/* this screen takes B: a right click presses it */
void Port_MouseBack(void);
/* the tag of the target under the shown pointer at the last read, or -1 */
s32 Port_MouseHoverTag(void);
/* pressed for this read too (a function target's doing) */
void Port_MouseInject(u32 input);

/* port/os/controller.c, each read: the pointer (picture units: x across a picture `width` wide at 1x, y down
 * 240; width 0: work it out from the picture's shape) and its flags */
void Port_MousePointerSet(float x, float y, int width, int flags);
/* port/os/controller.c, each read, after the pad: hover, clicks and what they press */
void Port_MouseMenuRead(u16 *buttons, s8 *stick_x, s8 *stick_y);
/* the Controller Pak manager runs (port/p0/stubs.c): its own screens (no frame drawing) */
void Port_MousePakManager(int running);

/* The spell UI (port/p0/spell_ui.c) reads the pointer before the menus do, takes the clicks it uses (so no target
 * under its own panel or bar presses anything too) and mutes the targets of the game's spell panel it replaces. */
void Port_MousePointerGet(float *x, float *y, int *flags);
void Port_MousePointerTake(int flags);
/* the pointer in an anchor's 320x240 units (PORT_MOUSE_*) */
void Port_MouseToAnchor(int anchor, float *x, float *y);
/* The game shows a screen the pointer is for: the pause and save menus, the title, the Controller Pak manager, and
 * in play the item menu, a text window, the spell input (the spell panel) and the element allocation. The host turns
 * mouse look off while one is shown (ElHost_SetScreen). */
int Port_MouseScreenOpen(void);
/* while on, Port_MouseTarget / Port_MouseTargetFn / Port_MouseBack name nothing */
void Port_MouseMute(int on);

#endif
