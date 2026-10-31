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

/* The width in pixels of one line of game text (Japanese: 10 a glyph; English: its font's). */
int Port_TextLineWidth(const uint8_t *s);

#endif
