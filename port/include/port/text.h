#ifndef PORT_TEXT_H
#define PORT_TEXT_H

/* port/text/en_text.c: the English script, in the game's own font or an optional one.
 *
 * The translation (text/en.tsv next to the exe, built from translations/en.v2.tsv) is keyed by the
 * ROM offset of the Japanese string each row replaces; the game's text is looked up by its bytes,
 * wherever it was copied from. English strings use font byte 0xC1 + n (font index 1 + n) for font
 * set n: 0 the original (the font's own Latin glyphs spaced by their width, plus the port's page
 * 30 of extra glyphs), 1.. the optional fonts (port/text/en_fonts.h). */

#include <stdint.h>

/* Read the translation and the fonts (once; the ROM must be loaded). */
void Port_TextInit(void);

/* Nonzero while the text language is English (the menu's setting, ELTALE_LANG=en|jp to force;
 * ELTALE_FONT=n forces font set n). */
int Port_TextEnglish(void);

/* After Text_LoadFontAssets: put the port's font pages in RDRAM. */
void Port_TextFontLoaded(void);

/* For the game's font index `font` >= 1 (an English set): page `page`'s texels (4bpp, the game's
 * font palette) and size. */
void Port_TextPage(int font, int page, uint8_t **texels, int *width, int *height);
/* Glyph `glyph` of `page`: where it is in the page (s, width), where to draw it from the pen
 * (x offset); returns how far the pen moves. */
int Port_TextGlyph(int font, int page, int glyph, int *s, int *width, int *xoff);
int Port_TextSpaceWidth(int font);
/* Pixels from one line to the next, and the glyphs' height. */
int Port_TextLineHeight(int font);
int Port_TextGlyphHeight(int font);

/* The English for a string drawn as it is (menus, names, choices), or `jp` if there is none. */
const uint8_t *Port_TextTranslate(const uint8_t *jp);

/* The same, word-wrapped to `width` pixels (the item menu's descriptions). */
const uint8_t *Port_TextTranslateWrapped(const uint8_t *jp, int width);

/* Lay a message out for a text window whose text area is `width` x `height` pixels: writes the
 * English into `out` (at most `cap` bytes) and returns 1, or returns 0 to keep the Japanese.
 * The message is `jp`'s English, or the one Port_TextSetPending gave. */
int Port_TextLayoutMessage(uint8_t *out, int cap, const uint8_t *jp, int width, int height);

/* Build the next message from pieces (the item-get message: prefix + item name + suffix). */
void Port_TextSetPending(const uint8_t *const *pieces, int n);

/* port/text/en_images.c: swap the pictures with text in them to the language's (English ones
 * made by tools/images/build_en_images.py); cheap when nothing changed, called every frame. */
void Port_ImagesUpdate(void);
/* The title menu's English strip and its three items' (s, width), or 0 while the text is Japanese. */
int Port_TitleStrip(uint8_t **texels, int *width, int *height, int pieces[3][2]);

/* The English name of spell `index` of `element` (-1, -1: the "can't use" label), or NULL. */
const char *Port_TextSpell(int element, int index);
/* The same in the battle HUD's font (en_images.c), or NULL while the text is Japanese. */
const uint8_t *Port_HudSpellName(int element, int index);
const uint8_t *Port_HudSpellLocked(void);
/* The English name of the monster whose Japanese name (HUD kana codes, up to 0xFF) is `jp`, or NULL;
 * and the same in the battle HUD's font (the enemy info panel), or NULL while the text is Japanese. */
const char *Port_TextMonster(const uint8_t *jp);
const uint8_t *Port_HudMonsterName(const uint8_t *jp);

/* The width in pixels of one line of game text (Japanese: 10 a glyph; English: its font's). */
int Port_TextLineWidth(const uint8_t *s);

/* ---- English windows that grow to their text (port/text/en_text.c) ----
 * The Japanese windows are sized for 10-pixel glyphs; the English is longer. Where a string is drawn
 * as it is into a fixed box (the save menu, the title's messages, the item menu), the game asks
 * these first and grows the box. Japanese: no change (the hashes of the checks stay). */

/* The string to draw for `jp` and the room it needs: in English, `jp`'s English with each line wider
 * than `max_width` pixels word-wrapped to it, *width its widest line and *height its lines' height
 * ((lines - 1) * line height + glyph height); otherwise `jp` itself and 0 x 0. */
const uint8_t *Port_TextFit(const uint8_t *jp, int max_width, int *width, int *height);

/* Grow the box (*x, *y, *w, *h) about its centre to at least need_w x need_h, kept inside the 4:3
 * screen's 320 x 240 less `margin` on each side. */
void Port_TextGrowBox(int *x, int *y, int *w, int *h, int need_w, int need_h, int margin);

#endif
