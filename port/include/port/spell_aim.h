/* Click to aim spells (port/p0/spell_aim.c, docs/SPELL_AIM.md).
 *
 * In a battle, with a spell ready (put together and castable now), the pointer over the battlefield shows where the
 * spell would go and whom it would hit: its area on the ground (circles, lines, a fan, a landing circle), and a ring
 * under every monster it would hit. A left click turns the player to face the monster or the ground point under the
 * pointer and casts (the game's A). The spell keeps its own reach and rules: the only thing the aim sets is the
 * player's facing (Actor.rot) in Player_StartCastSpell, which the player could have set by turning before building the
 * spell. Who gets hit comes from a copy of the game's own movement and hit test (no game function with side effects is
 * called, no game state is written but the facing at the cast).
 *
 * While aiming, the spell UI's panel folds up to the spell's plate at the top (Port_SpellAimCollapsed) and the camera
 * rises (Port_SpellAimLift; the eye's height only, drawing only: port/mouse_camera.h). Everything here is controller
 * input and drawing: with no pointer (or the switch off) nothing changes and every game hash is as before. */
#ifndef ELTALE_PORT_SPELL_AIM_H
#define ELTALE_PORT_SPELL_AIM_H

#include "common.h"

struct Player;
struct Actor;

/* The switch: the host's F1 "Click to aim spells" (settings.json controls.spell_aim, default on) and "Raise the camera
 * while aiming" (controls.spell_aim_camera); headless ELTALE_SPELL_AIM=1 (default off there) and
 * ELTALE_SPELL_AIM_CAMERA=0|1 (default 1). */
int Port_SpellAimEnabled(void);
/* port/os/controller.c (through Port_SpellUiKeys), each read: the host's switches (4 aim, 8 camera), -1 none */
void Port_SpellAimSwitch(int on);

/* port/os/controller.c, each read, after Port_MouseMenuRead: hover (the preview) and a click (the aim, and A) */
void Port_SpellAimRead(u16 *buttons, s8 *stick_x, s8 *stick_y);
/* Player_StartCastSpell (patches/p0.patch): an aim taken at this read turns the caster toward its point */
void Port_SpellAimApply(struct Player *p, struct Actor *a);

/* Hud_Draw (patches/p0.patch), before the spell UI: the preview, as world-space overlays (native renderer only) */
void Port_SpellAimDraw(void);
/* The end of the frame build (port/p0/mouse_camera.c Port_MouseCameraRestore, before the game's eye is put back):
 * the view this tick's picture was drawn with, for the next read's pointer */
void Port_SpellAimViewDrawn(void);

/* The spell UI (port/p0/spell_ui.c): aiming, so its panel shows only the spell's plate */
int Port_SpellAimCollapsed(void);
/* The plate's place while folded (CENTRE units) */
void Port_SpellAimPlateRect(s32 *x, s32 *y, s32 *w, s32 *h);
/* port/p0/mouse_camera.c, each tick: 1 while the camera should be raised (aiming), else 0 */
int Port_SpellAimLiftWanted(void);

#endif
