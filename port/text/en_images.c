/* The English pictures: swapped in place of the Japanese ones while the text language is English
 * (port/include/port/text.h).
 *
 * en_images.inc (tools/images/build_en_images.py, made at build time from the ROM and art/en) holds
 * each picture at its original size, format and palette. A picture the game keeps in the native
 * image (its data compiled from the ROM) is swapped there; one it reads from the ROM (the map
 * panels) is swapped in the loaded ROM, so the next read gets it. The title menu's English strip
 * is wider than the Japanese one: it lives in RDRAM past the 4 MiB the game uses and the title
 * screen draws it itself (Port_TitleStrip). */
#include "port/text.h"
#include "port/p0/host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    unsigned char *native; /* the game's own copy, or 0 for one read from the ROM */
    unsigned rom;
    unsigned ram;  /* where the game has already loaded it from the ROM (the common block), or 0 */
    unsigned size;
    const unsigned char *en;
    unsigned char *original; /* saved on first swap */
} EnImage;

#include "en_images.inc"

#define TITLE_STRIP_ADDR 0x807F8000u

static int applied = -1;

void Port_ImagesUpdate(void) {
    int en = Port_TextEnglish();
    if (en == applied) return;
    unsigned char *rom = Port_HostRomWritable();
    for (size_t i = 0; i < sizeof en_images / sizeof en_images[0]; i++) {
        EnImage *img = &en_images[i];
        unsigned char *target = img->native ? img->native : rom + img->rom;
        if (!img->original) {
            img->original = malloc(img->size);
            memcpy(img->original, target, img->size);
        }
        memcpy(target, en ? img->en : img->original, img->size);
        if (img->ram) memcpy((void *)(uintptr_t)img->ram, en ? img->en : img->original, img->size);
    }
    memcpy((void *)(uintptr_t)TITLE_STRIP_ADDR, en_title_strip, sizeof en_title_strip);
    applied = en;
}

/* The battle HUD's spell names in its own font: 0x83 (English: glyphs 6 pixels apart), the glyph
 * codes (en_hud_codes), 0x82 for a space. */
static const uint8_t *hud_encode(const char *text, uint8_t *buf, int cap) {
    int n = 0;
    buf[n++] = 0x83;
    for (; *text && n < cap - 1; text++) {
        unsigned char c = (unsigned char)*text;
        if (c == ' ') buf[n++] = 0x82;
        else if (c < 128 && en_hud_codes[c] != 255) buf[n++] = en_hud_codes[c];
    }
    buf[n] = 0xFF;
    return buf;
}

const uint8_t *Port_HudSpellName(int element, int index) {
    static uint8_t cache[16][16][32];
    const char *text = Port_TextSpell(element, index);
    if (!text) return NULL;
    if (getenv("ELTALE_TEXT_LOG")) printf("P1_SPELL %d %d %s\n", element, index, text);
    Port_ImagesUpdate();
    return hud_encode(text, cache[element & 15][index & 15], sizeof cache[0][0]);
}

const uint8_t *Port_HudSpellLocked(void) {
    static uint8_t buf[32];
    const char *text = Port_TextSpell(-1, -1);
    if (!text) return NULL;
    Port_ImagesUpdate();
    return hud_encode(text, buf, sizeof buf);
}

int Port_TitleStrip(uint8_t **texels, int *width, int *height, int pieces[3][2]) {
    if (!Port_TextEnglish()) return 0;
    Port_ImagesUpdate();
    *texels = (uint8_t *)(uintptr_t)TITLE_STRIP_ADDR;
    *width = EN_TITLE_W;
    *height = EN_TITLE_H;
    for (int i = 0; i < 3; i++) {
        pieces[i][0] = en_title_pieces[i][0];
        pieces[i][1] = en_title_pieces[i][1];
    }
    return 1;
}
