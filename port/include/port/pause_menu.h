/* The unified pause menu (port/p0/pause_menu.c, docs/PAUSE_MENU.md): one screen in place of the Start menu's four
 * pages (Status, Map, Elements, Options).
 *
 * The game's Start menu (game mode 2) runs as always: Start opens it, Start or B closes it, through the game's own
 * code. While it is open this draws one screen instead of the pages: the world map of a region on the left (pan,
 * zoom, the player's pin, the player's own marker), Status and Elements on the right, Options as a dialog. Everything
 * is drawn from the game's own pictures (the menu's sprites, the map pictures, the exp gauge, the HUD's font) into a
 * display list of our own that the native renderer records (as the spell UI: port/spell_ui.h); the frame's own list
 * only loses the pages. The game state it changes is the game's own options (text speed and music volume, as the
 * Options page sets them); the marker, the region and the view are the port's. With the switch off (the F1 menu)
 * the game's four pages are back, untouched.
 *
 * The player's marker: one at a time, on a region's map, kept per save in the data folder (mapmarker/, keyed by the
 * save's record as the spell UI's quick bar and the area map's fog of war). In that region's field the compass shows
 * a cyan dot toward it, in place of the dot the game draws for its own map-page marker. */
#ifndef ELTALE_PORT_PAUSE_MENU_H
#define ELTALE_PORT_PAUSE_MENU_H

#include "common.h"

/* On: the host's F1 switch (settings.json controls.pause_menu, default on); headless ELTALE_PAUSE_MENU=1 (default off
 * there, so the headless checks and picture baselines are the original game's). */
int Port_PauseMenuEnabled(void);

/* port/os/controller.c, each read (through Port_SpellUiKeys): the spell UI's keys and the host's switches (on: bit 0
 * the spell UI, bit 1 this menu; -1 an input script's read). The menu takes PORT_SPELLKEY_CLEAR (Esc: open and close
 * it) and PORT_PAUSEKEY_OPTIONS (O: its Options). */
enum { PORT_PAUSEKEY_OPTIONS = 1u << 13 };
void Port_PauseMenuKeys(u32 keys, int on);
/* each read, before the camera takes it: the wheel over the open menu zooms its map (all of it taken) */
int Port_PauseMenuWheel(int wheel);
/* each read, after the spell UI and before Port_MouseMenuRead: the pointer's clicks and drags while the menu is open
 * (all taken), Esc (Start, when the game would open or close the menu), the Close button (Start) */
void Port_PauseMenuRead(u16 *buttons, s8 *stick_x, s8 *stick_y);

/* Menu_Update (patches/p0.patch): 1 when this frame is the pause menu's (the switch on and not the save menu): its
 * input and drawing are done here, the game's pages are not run; 0: the game's pages as always */
int Port_PauseMenuFrame(void);

/* Hud_DrawCompass: the game's own dot for the map page's marker is left out (muted) while this is on; this draws the
 * cyan dot for the player's marker (native 2D only), with the HUD's origin and anchor as they are */
int Port_PauseMenuCompassMute(void);
void Port_PauseMenuCompass(void);

/* SaveMenu_CaptureGameState (kind 0) and SaveMenu_RestoreGameState (kind 1): the marker is kept with the save */
void Port_PauseMenuSaveEvent(int kind, s32 slot);

#endif
