/* The English script in the game's own font, or one of the optional fonts (port/include/port/text.h).
 *
 * text/en.tsv (tools/text/runtime_text.py): one row per string, "<ROM offset hex>\t<English>",
 * the English in ASCII with \n a line break, \f a page break and \x02 a Yes/No choice. The
 * Japanese string at that ROM offset (up to its 0xFF) is the key: the game's text is found by its
 * bytes wherever the game copied it to. tools/text/enfont.py is the same layout in Python (for
 * previews of the original font), so the two must agree.
 *
 * Fonts: set 0 is the original (the game's own Latin glyphs, font pages 0-2, plus the port's extra
 * page 30); sets 1.. are en_fonts[] (port/text/en_fonts.h). English text is encoded with font byte
 * 0xC1 + set, so text laid out in one font still draws right after the player picks another. */
#include "port/text.h"
#include "port/p0/host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "en_glyphs.h"
#include "en_fonts.h"

int Port_WindowLanguage(void);
int Port_WindowTextFont(void);

enum { FONT_ROM = 0xD3A930, PAGE_BYTES = 0x7B8, ROW_BYTES = 152, EXTRA_PAGE = 30, MAX_TEXT = 0x4B0 };
enum { SETS = 1 + sizeof en_fonts / sizeof en_fonts[0], PAGES = 32 };
/* logical characters beyond ASCII: a dot inside "...", and the curly quotes */
enum { SDOT = 1, OPEN = 3, CLOSE = 4 };
/* the game's font in RDRAM (Text_LoadFontAssets), and the port's pages past the 4 MiB it uses */
#define GAME_FONT_ADDR 0x803245A0u
#define EXTRA_ADDR 0x807F0000u
#define SET_PAGES_ADDR 0x807F0800u

typedef struct {
    uint64_t hash;
    char *text;       /* the English */
    uint8_t *drawn;   /* encoded without wrapping, for Port_TextTranslate (made on first use) */
    int drawn_set;
    uint8_t *wrapped; /* encoded wrapped to wrapped_width, for Port_TextTranslateWrapped */
    int wrapped_width, wrapped_set;
} Entry;

typedef struct { uint16_t s; uint8_t w, xoff, adv; } Metric; /* s: up to 303 on the game's 304-wide pages */

typedef struct {
    int height, line, space;
    uint32_t page_addr[PAGES];
    int page_width;
    uint8_t code_page[128], code_index[128]; /* 0xFF: no glyph */
    Metric metric[PAGES][128];
} FontSet;

static Entry *entries;
static int entry_count;
static FontSet sets[SETS];
static char pending[MAX_TEXT];
static int has_pending;
static int forced_lang = -1, forced_font = -1; /* ELTALE_LANG, ELTALE_FONT */
static char *spell_text[16][16], *spell_locked; /* rows 0x2000000 + element * 16 + index, 0x20000FF */
/* monster names in the battle HUD's kana codes (rows 0x3000000 + the name's ROM offset): a short list */
enum { MAX_MONSTERS = 128 };
static struct { uint64_t hash; char *text; } monsters[MAX_MONSTERS];
static int monster_count;

static uint64_t hash_bytes(const uint8_t *s) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < MAX_TEXT && s[i] != 0xFF; i++) h = (h ^ s[i]) * 1099511628211ull;
    return h;
}

static int by_hash(const void *a, const void *b) {
    uint64_t x = ((const Entry *)a)->hash, y = ((const Entry *)b)->hash;
    return x < y ? -1 : x > y;
}

/* With ELTALE_TEXT_LOG set: print each Japanese string shown that has no English (once). */
static void log_miss(const uint8_t *jp) {
    static int on = -1;
    static uint64_t seen[512];
    static int n;
    if (on < 0) on = getenv("ELTALE_TEXT_LOG") != NULL;
    if (!on || jp[0] == 0xFF) return;
    uint64_t h = hash_bytes(jp);
    for (int i = 0; i < n; i++)
        if (seen[i] == h) return;
    if (n < 512) seen[n++] = h;
    fprintf(stderr, "P1_TEXT_MISS");
    for (int i = 0; i < 48 && jp[i] != 0xFF; i++) fprintf(stderr, " %02X", jp[i]);
    fprintf(stderr, "\n");
}

static Entry *find(const uint8_t *jp) {
    if (!entry_count || !jp || jp[0] == 0xFF) return NULL;
    uint64_t h = hash_bytes(jp);
    int lo = 0, hi = entry_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (entries[mid].hash == h) return &entries[mid];
        if (entries[mid].hash < h) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* ---- the fonts ---- */

static int pixel(const uint8_t *page, int x, int y) {
    uint8_t b = page[y * ROW_BYTES + x / 2];
    return x % 2 == 0 ? b >> 4 : b & 15;
}

/* set 0, one glyph of a 304x13 page: its ink's columns */
static void measure(FontSet *f, int page, int g, const uint8_t *pixels) {
    int left = -1, right = -1;
    for (int x = 0; x < 10; x++)
        for (int y = 0; y < 13; y++)
            if (pixel(pixels, g * 10 + x, y)) {
                if (left < 0) left = x;
                right = x;
            }
    int w = left < 0 ? 3 : right - left + 1;
    f->metric[page][g] = (Metric){ (uint16_t)(g * 10 + (left < 0 ? 0 : left)), (uint8_t)w, 0, (uint8_t)w };
}

static void map_code(FontSet *f, int code, int page, int index) {
    f->code_page[code] = (uint8_t)page;
    f->code_index[code] = (uint8_t)index;
}

static void init_original(const uint8_t *rom) {
    FontSet *f = &sets[0];
    f->height = 13;
    f->line = 14;
    f->space = 4;
    f->page_width = 304;
    for (int p = 0; p < PAGES; p++) f->page_addr[p] = GAME_FONT_ADDR + p * PAGE_BYTES;
    f->page_addr[EXTRA_PAGE] = EXTRA_ADDR;
    for (int i = 0; i < 10; i++) map_code(f, '0' + i, 0, i);
    map_code(f, '!', 0, 10); map_code(f, '?', 0, 11); map_code(f, '.', 0, 16); map_code(f, ',', 0, 17);
    map_code(f, ':', 0, 20); map_code(f, '~', 0, 24);
    for (int i = 0; i < 26; i++) {
        map_code(f, 'A' + i, 1, i);
        map_code(f, 'a' + i, 2, i);
    }
    map_code(f, '&', 1, 26); map_code(f, '\'', 2, 26);
    map_code(f, OPEN, 2, 28); map_code(f, CLOSE, 2, 27);
    for (int i = 0; en_extra_order[i]; i++) map_code(f, (unsigned char)en_extra_order[i], EXTRA_PAGE, i);
    for (int p = 0; p < 3; p++)
        for (int g = 0; g < 30; g++) measure(f, p, g, rom + FONT_ROM + p * PAGE_BYTES);
    for (int i = 0; en_extra_order[i]; i++) {
        measure(f, EXTRA_PAGE, i, en_extra_page);
        if (en_extra_width[i]) f->metric[EXTRA_PAGE][i].w = f->metric[EXTRA_PAGE][i].adv = en_extra_width[i];
    }
}

static void init_ttf_sets(void) {
    uint32_t addr = SET_PAGES_ADDR;
    for (int k = 1; k < SETS; k++) {
        const EnFont *src = &en_fonts[k - 1];
        FontSet *f = &sets[k];
        int count[PAGES] = { 0 };
        f->height = src->height;
        f->line = src->line;
        f->space = src->space;
        f->page_width = 256;
        for (int p = 0; p < src->pages; p++, addr += 0x800) f->page_addr[p] = addr;
        for (int c = 0; c < 128; c++) {
            const EnGlyph *g = &src->glyph[c];
            if (!g->width) continue;
            int code = c == 1 ? OPEN : c == 2 ? CLOSE : c;
            int i = count[g->page]++;
            f->metric[g->page][i] = (Metric){ g->s, g->width, g->xoff, g->advance };
            map_code(f, code, g->page, i);
        }
        /* what a font lacks: straight quotes for curly ones, a plain dot for the spaced one */
        if (f->code_page[OPEN] == 0xFF) map_code(f, OPEN, f->code_page['"'], f->code_index['"']);
        if (f->code_page[CLOSE] == 0xFF) map_code(f, CLOSE, f->code_page['"'], f->code_index['"']);
        map_code(f, SDOT, f->code_page['.'], f->code_index['.']);
    }
}

static const char *text_path(char *buf, size_t size) {
    const char *env = getenv("ELTALE_TEXT");
    if (env) return env;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    if (n == 0 || n >= size) return "text/en.tsv";
    char *slash = strrchr(buf, '\\');
    if (!slash) return "text/en.tsv";
    snprintf(slash + 1, size - (size_t)(slash + 1 - buf), "text\\en.tsv");
    return buf;
#else
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return "text/en.tsv";
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    if (!slash) return "text/en.tsv";
    snprintf(slash + 1, size - (size_t)(slash + 1 - buf), "text/en.tsv");
    return buf;
#endif
}

void Port_TextInit(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *env = getenv("ELTALE_LANG");
    if (env) forced_lang = strcmp(env, "en") == 0;
    env = getenv("ELTALE_FONT");
    if (env) forced_font = atoi(env);

    memset(sets, 0, sizeof sets);
    for (int k = 0; k < SETS; k++) memset(sets[k].code_page, 0xFF, sizeof sets[k].code_page);
    const uint8_t *rom = Port_HostRom();
    init_original(rom);
    init_ttf_sets();
    Port_TextFontLoaded(); /* now too: the Controller Pak manager draws text before the game loads its font */

    char path_buf[1024];
    const char *path = text_path(path_buf, sizeof path_buf);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "text: no English script at %s\n", path);
        return;
    }
    static char line[8192];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        uint32_t at = (uint32_t)strtoul(line, NULL, 16);
        char *text = tab + 1;
        size_t len = strlen(text);
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = 0;
        /* unescape \n, \f, \2 */
        char *w = text;
        for (char *r = text; *r; r++) {
            if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
            else if (r[0] == '\\' && r[1] == 'f') { *w++ = '\f'; r++; }
            else if (r[0] == '\\' && r[1] == '2') { *w++ = '\x02'; r++; }
            else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
            else *w++ = *r;
        }
        *w = 0;
        if (at == 0x20000FF) {
            spell_locked = strdup(text);
            continue;
        }
        if (at >= 0x2000000 && at < 0x2000100) {
            spell_text[(at >> 4) & 15][at & 15] = strdup(text);
            continue;
        }
        if (at >= 0x3000000 && at < 0x4000000) {
            if (monster_count < MAX_MONSTERS) {
                monsters[monster_count].hash = hash_bytes(rom + (at - 0x3000000));
                monsters[monster_count++].text = strdup(text);
            }
            continue;
        }
        if (at >= 0x1000000) continue;
        if (entry_count == cap) {
            cap = cap ? cap * 2 : 1024;
            entries = realloc(entries, (size_t)cap * sizeof *entries);
        }
        entries[entry_count] = (Entry){ hash_bytes(rom + at), strdup(text), NULL, -1, NULL, 0, -1 };
        entry_count++;
    }
    fclose(f);
    qsort(entries, (size_t)entry_count, sizeof *entries, by_hash);
}

int Port_TextEnglish(void) {
    if (!entry_count) return 0;
    if (forced_lang >= 0) return forced_lang;
    return Port_WindowLanguage() > 0;
}

/* the font set English is laid out in now */
static int active_set(void) {
    int k = forced_font >= 0 ? forced_font : Port_WindowTextFont();
    return k > 0 && k < SETS ? k : 0;
}

void Port_TextFontLoaded(void) {
    memcpy((void *)(uintptr_t)EXTRA_ADDR, en_extra_page, sizeof en_extra_page);
    for (int k = 1; k < SETS; k++) {
        const EnFont *src = &en_fonts[k - 1];
        size_t bytes = (size_t)(sets[k].page_width / 2) * (size_t)src->height;
        for (int p = 0; p < src->pages; p++)
            memcpy((void *)(uintptr_t)sets[k].page_addr[p], src->texels + p * bytes, bytes);
    }
}

static FontSet *set_of(int font) { return font >= 1 && font <= SETS ? &sets[font - 1] : &sets[0]; }

void Port_TextPage(int font, int page, uint8_t **texels, int *width, int *height) {
    FontSet *f = set_of(font);
    if (page < 0 || page >= PAGES) page = 0;
    *texels = (uint8_t *)(uintptr_t)f->page_addr[page];
    *width = f->page_width;
    *height = f->height;
}

int Port_TextGlyph(int font, int page, int glyph, int *s, int *width, int *xoff) {
    FontSet *f = set_of(font);
    if (page < 0 || page >= PAGES || glyph < 0 || glyph >= 128) {
        *s = *width = *xoff = 0;
        return 0;
    }
    Metric m = f->metric[page][glyph];
    *s = m.s;
    *width = m.w;
    *xoff = m.xoff;
    return m.adv;
}

int Port_TextSpaceWidth(int font) { return set_of(font)->space; }
int Port_TextLineHeight(int font) { return set_of(font)->line; }
int Port_TextGlyphHeight(int font) { return set_of(font)->height; }

/* ---- layout ---- */

static int is_open_quote(const char *text, const char *p) {
    if (p == text) return 1;
    char c = p[-1];
    return c == ' ' || c == '\n' || c == '\f' || c == '(' || c == '*' || c == '-';
}

/* the logical character at p: quotes by position, spaced dots inside "..." */
static int code_at(const char *text, const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == '.' && p[1] == '.') return SDOT;
    if (c == '"') return is_open_quote(text, p) ? OPEN : CLOSE;
    return c < 128 ? c : -1;
}

static int char_width(const FontSet *f, const char *text, const char *p) {
    if (*p == ' ') return f->space;
    int c = code_at(text, p);
    if (c < 0 || f->code_page[c] == 0xFF) return 0;
    return f->metric[f->code_page[c]][f->code_index[c]].adv;
}

typedef struct {
    uint8_t *out;
    int n, cap, page;
} Writer;

static void put(Writer *w, uint8_t b) {
    if (w->n < w->cap - 1) w->out[w->n++] = b;
}

static void put_char(Writer *w, const FontSet *f, const char *text, const char *p) {
    if (*p == ' ') { put(w, 0x7F); return; }
    if (*p == '\x02') { put(w, 0xF2); return; }
    int c = code_at(text, p);
    if (c < 0 || f->code_page[c] == 0xFF) return;
    if (f->code_page[c] != w->page) {
        w->page = f->code_page[c];
        put(w, (uint8_t)(0x80 | w->page));
    }
    put(w, f->code_index[c]);
}

/* Word-wrap `text` in font set k to `width` pixels and `rows` lines a page. */
static int layout(int k, uint8_t *out, int cap, const char *text, int width, int rows) {
    const FontSet *f = &sets[k];
    Writer w = { out, 0, cap, -1 };
    put(&w, 0xA0);
    put(&w, (uint8_t)(0xC1 + k));
    int row = 0, x = 0, line_has_text = 0;
    const char *p = text;
    while (*p) {
        if (*p == '\f') {
            put(&w, 0xF0);
            row = 0; x = 0; line_has_text = 0;
            p++;
            continue;
        }
        if (*p == '\n') {
            if (++row >= rows) { put(&w, 0xF0); row = 0; }
            else put(&w, 0xE0);
            x = 0; line_has_text = 0;
            p++;
            continue;
        }
        if (*p == ' ') {
            /* a space, then the next word: wrap before the word if it does not fit */
            int ww = 0;
            for (const char *q = p + 1; *q && *q != ' ' && *q != '\n' && *q != '\f'; q++) ww += char_width(f, text, q);
            if (line_has_text && x + f->space + ww > width) {
                if (++row >= rows) { put(&w, 0xF0); row = 0; }
                else put(&w, 0xE0);
                x = 0; line_has_text = 0;
            } else {
                put(&w, 0x7F);
                x += f->space;
            }
            p++;
            continue;
        }
        x += char_width(f, text, p);
        put_char(&w, f, text, p);
        line_has_text = 1;
        p++;
    }
    out[w.n++] = 0xFF;
    return w.n;
}

static uint8_t *encode_copy(int k, const char *text, int width, int rows) {
    uint8_t buf[MAX_TEXT];
    int n = layout(k, buf, sizeof buf, text, width, rows);
    uint8_t *copy = malloc((size_t)n);
    memcpy(copy, buf, (size_t)n);
    return copy;
}

const uint8_t *Port_TextTranslate(const uint8_t *jp) {
    if (!Port_TextEnglish()) return jp;
    Entry *e = find(jp);
    if (!e) {
        if (jp[0] == 0xA0 && (jp[1] < 0xC1 || jp[1] > 0xC0 + SETS)) log_miss(jp);
        return jp;
    }
    int k = active_set();
    if (!e->drawn || e->drawn_set != k) {
        free(e->drawn);
        e->drawn = encode_copy(k, e->text, 1 << 20, 1 << 20);
        e->drawn_set = k;
    }
    return e->drawn;
}

const uint8_t *Port_TextTranslateWrapped(const uint8_t *jp, int width) {
    if (!Port_TextEnglish()) return jp;
    Entry *e = find(jp);
    if (!e) return Port_TextTranslate(jp);
    int k = active_set();
    if (!e->wrapped || e->wrapped_width != width || e->wrapped_set != k) {
        free(e->wrapped);
        e->wrapped = encode_copy(k, e->text, width, 1 << 20);
        e->wrapped_width = width;
        e->wrapped_set = k;
    }
    return e->wrapped;
}

void Port_TextSetPending(const uint8_t *const *pieces, int n) {
    has_pending = 0;
    if (!Port_TextEnglish()) return;
    pending[0] = 0;
    for (int i = 0; i < n; i++) {
        Entry *e = find(pieces[i]);
        if (!e) return; /* keep the Japanese message */
        strncat(pending, e->text, sizeof pending - strlen(pending) - 1);
    }
    has_pending = 1;
}

int Port_TextLayoutMessage(uint8_t *out, int cap, const uint8_t *jp, int width, int height) {
    const char *text = NULL;
    if (has_pending) {
        has_pending = 0;
        if (Port_TextEnglish()) text = pending;
    }
    if (!text && Port_TextEnglish()) {
        Entry *e = find(jp);
        if (e) text = e->text;
        else log_miss(jp);
    }
    if (!text) return 0;
    if (getenv("ELTALE_TEXT_LOG")) fprintf(stderr, "P1_TEXT %.60s\n", text);
    int k = active_set();
    int rows = (height - sets[k].height) / sets[k].line + 1;
    layout(k, out, cap, text, width, rows < 1 ? 1 : rows);
    return 1;
}

const char *Port_TextSpell(int element, int index) {
    if (!Port_TextEnglish()) return NULL;
    if (element < 0) return spell_locked;
    return element < 16 && index >= 0 && index < 16 ? spell_text[element][index] : NULL;
}

const char *Port_TextMonster(const uint8_t *jp) {
    if (!Port_TextEnglish() || !jp || jp[0] == 0xFF) return NULL;
    uint64_t h = hash_bytes(jp);
    for (int i = 0; i < monster_count; i++)
        if (monsters[i].hash == h) return monsters[i].text;
    log_miss(jp);
    return NULL;
}

int Port_TextLineWidth(const uint8_t *s) {
    int x = 0, best = 0, font = 0, page = 0, sw, w, xoff;
    for (; *s != 0xFF; s++) {
        uint8_t b = *s;
        if (b == 0x7F) x += font >= 1 ? Port_TextSpaceWidth(font) : 10;
        else if (b < 0x80) x += font >= 1 ? Port_TextGlyph(font, page, b, &sw, &w, &xoff) : 10;
        else if (b < 0xA0) page = b & 0x1F;
        else if (b >= 0xC0 && b < 0xE0 && (b & 0x1F) < 16) font = b & 0x1F;
        else if (b == 0xE0) { if (x > best) best = x; x = 0; }
    }
    return x > best ? x : best;
}
