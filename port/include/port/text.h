#ifndef PORT_TEXT_H
#define PORT_TEXT_H

/* port/text/en_text.c: the English script in the game's own font.
 *
 * The translation (text/en.tsv next to the exe, built from translations/en.v2.tsv) is keyed by the
 * ROM offset of the Japanese string each row replaces; the game's text is looked up by its bytes,
 * wherever it was copied from. English strings use font 1 (byte 0xC1): the font's own Latin
 * glyphs spaced by their width, plus the port's page 30 (byte 0x9E) of extra glyphs. */

#include <stdint.h>

/* Read the translation and the font's glyph widths (once; the ROM must be loaded). */
void Port_TextInit(void);

/* Nonzero while the text language is English (the menu's setting, ELTALE_LANG=en|jp to force). */
int Port_TextEnglish(void);

/* After Text_LoadFontAssets: put the extra glyph page in RDRAM. */
void Port_TextFontLoaded(void);
/* The RDRAM address of font page `page` >= 30 (the extra page). */
uint8_t *Port_TextExtraPage(int page);
/* Font 1: the left edge and width of glyph `glyph` of `page`. */
int Port_TextGlyphWidth(int page, int glyph, int *left);
/* The pixels a space advances in font 1. */
int Port_TextSpaceWidth(void);

/* The English for a string drawn as it is (menus, names, choices), or `jp` if there is none. */
const uint8_t *Port_TextTranslate(const uint8_t *jp);

/* The same, word-wrapped to `width` pixels (the item menu's descriptions). */
const uint8_t *Port_TextTranslateWrapped(const uint8_t *jp, int width);

/* Lay a message out for a text window `width` pixels wide with `rows` lines a page: writes the
 * English into `out` (at most `cap` bytes) and returns 1, or returns 0 to keep the Japanese.
 * The message is `jp`'s English, or the one Port_TextSetPending gave. */
int Port_TextLayoutMessage(uint8_t *out, int cap, const uint8_t *jp, int width, int rows);

/* Build the next message from pieces (the item-get message: prefix + item name + suffix). */
void Port_TextSetPending(const uint8_t *const *pieces, int n);

/* The width in pixels of one line of game text (font 0: 10 a glyph; font 1: proportional). */
int Port_TextLineWidth(const uint8_t *s);

#endif
