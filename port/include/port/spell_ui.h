/* The spell UI (port/p0/spell_ui.c, docs/SPELL_UI.md): a mouse-friendly face on the game's own spell input.
 *
 * While a spell is being put together (the game's spell input, core_flags & 4) a panel shows the spell so far on the
 * game's chevron plate (click it: cast), the four element crystals spaced out (click: that element) and under each
 * the plate of the spell that element would make (click: the same). A quick bar at the bottom holds 3 pages of 6
 * spells the player put there (click or 5-0: cast; drag the plate onto it to keep a spell; per save).
 *
 * Everything it does to the game is controller input: a click or a key becomes the C button, A or B the controller
 * would press, on the game's thread, at a controller read (port/os/controller.c), through the same path as the
 * mouse in menus (port/mouse_menu.h). A quick-bar spell is its C buttons one read at a time, then A. With no mouse
 * and none of its keys used it changes nothing, and with it off (the F1 menu's switch) the game draws and
 * behaves as the original. Its pictures are the native renderer's (the HUD section), drawn from the game's own
 * textures, palettes and fonts as they are in RDRAM; nothing of it goes into the frame's display list. */
#ifndef ELTALE_PORT_SPELL_UI_H
#define ELTALE_PORT_SPELL_UI_H

#include "common.h"

/* The keys the host sends (ElHost_GetSpellKeys; an input script's `spellkey VI MASK` rows): presses since the last
 * read */
enum {
    PORT_SPELLKEY_CRYSTAL = 1u << 0,  /* << element 0..3: keys 1-4 (fire, earth, water, wind) */
    PORT_SPELLKEY_SLOT = 1u << 4,     /* << slot 0..5 of the page shown: keys 5-0 */
    PORT_SPELLKEY_PAGE_PREV = 1u << 10,
    PORT_SPELLKEY_PAGE_NEXT = 1u << 11,
    PORT_SPELLKEY_CLEAR = 1u << 12,   /* Esc: the elements put in so far taken back */
};

/* On: the host's F1 switch (settings.json controls.spell_ui, default on); headless ELTALE_SPELL_UI=1 (default
 * off there, so the headless checks and picture baselines are the original game's). */
int Port_SpellUiEnabled(void);

/* port/os/controller.c, each read: the keys pressed since the last read and the host's switches (bit 0 this UI, bit 1
 * the pause menu: port/pause_menu.h, which gets the keys too; -1 an input script's read) */
void Port_SpellUiKeys(u32 keys, int on);
/* port/os/controller.c, each read, before the camera takes the wheel: a wheel over the quick bar pages it (the
 * steps it used are taken: what is left goes to the camera) */
int Port_SpellUiWheel(int wheel);
/* port/os/controller.c, each read, after the pad and before Port_MouseMenuRead: the pointer's clicks on the panel
 * and the bar, the keys, and the quick bar's casts, as C buttons, A and B pressed for this read */
void Port_SpellUiRead(u16 *buttons, s8 *stick_x, s8 *stick_y);

/* Hud_Draw (patches/p0.patch): the panel and the bar, last in the HUD section (native items only) */
void Port_SpellUiDraw(void);
/* Hud_Draw around Hud_DrawSpellInput: the game's spell panel runs as always but is not shown (nor pointed at) */
void Port_SpellUiReplaceBegin(void);
void Port_SpellUiReplaceEnd(void);

/* SaveMenu_CaptureGameState (kind 0) and SaveMenu_RestoreGameState (kind 1): the quick bar is kept with the save
 * (a file per save record in the data folder, spellbar/) */
void Port_SpellUiSaveEvent(int kind, s32 slot);

/* ---- The hook for aiming (docs/SPELL_UI.md "Aiming") -------------------------------------------------------------
 * A spell is ready on the plate: elements put in and the game would cast it now (Magic_GetSpellRestrictionFlags 0).
 * Returns its id (Magic_LookupSpellSequence's: element << 8 | index) or -1. */
s32 Port_SpellUiReady(void);
/* Cast the spell on the plate: A pressed at the next read (the one way the UI casts: the plate, the quick bar's
 * last step, and a click on the battlefield once aiming is done). 0 when nothing is ready. */
int Port_SpellUiCast(void);

#endif
