/* The English script in the game's own font (port/include/port/text.h).
 *
 * text/en.tsv (tools/text/runtime_text.py): one row per string, "<ROM offset hex>\t<English>",
 * the English in ASCII with \n a line break, \f a page break and \x02 a Yes/No choice. The
 * Japanese string at that ROM offset (up to its 0xFF) is the key: the game's text is found by its
 * bytes wherever the game copied it to. tools/text/enfont.py is the same layout in Python (for
 * previews), so the two must agree. */
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

int Port_Rt64Language(void);

enum { FONT_ROM = 0xD3A930, PAGE_BYTES = 0x7B8, ROW_BYTES = 152, EXTRA_PAGE = 30, SPACE = 4, MAX_TEXT = 0x4B0 };
/* the extra page lives past the 4 MiB the game uses */
#define EXTRA_ADDR ((uint8_t *)(uintptr_t)0x807F0000u)

typedef struct {
    uint64_t hash;
    char *text;      /* the English */
    uint8_t *drawn;  /* encoded without wrapping, for Port_TextTranslate (made on first use) */
    uint8_t *wrapped; /* encoded wrapped to wrapped_width, for Port_TextTranslateWrapped */
    int wrapped_width;
} Entry;

static Entry *entries;
static int entry_count;
static int8_t glyph_page[128], glyph_index[128];
static uint8_t glyph_left[32][30], glyph_width[32][30];
static char pending[MAX_TEXT];
static int has_pending;
static int forced = -1; /* ELTALE_LANG */

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

static void set_glyph(int ch, int page, int index) {
    glyph_page[ch] = (int8_t)page;
    glyph_index[ch] = (int8_t)index;
}

static int pixel(const uint8_t *page, int x, int y) {
    uint8_t b = page[y * ROW_BYTES + x / 2];
    return x % 2 == 0 ? b >> 4 : b & 15;
}

static void measure(int page, const uint8_t *pixels) {
    for (int g = 0; g < 30; g++) {
        int left = -1, right = -1;
        for (int x = 0; x < 10; x++)
            for (int y = 0; y < 13; y++)
                if (pixel(pixels, g * 10 + x, y)) {
                    if (left < 0) left = x;
                    right = x;
                }
        glyph_left[page][g] = (uint8_t)(left < 0 ? 0 : left);
        glyph_width[page][g] = (uint8_t)(left < 0 ? 3 : right - left + 1);
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
    const char *lang = getenv("ELTALE_LANG");
    if (lang) forced = strcmp(lang, "en") == 0;

    memset(glyph_page, -1, sizeof glyph_page);
    for (int i = 0; i < 10; i++) set_glyph('0' + i, 0, i);
    set_glyph('!', 0, 10); set_glyph('?', 0, 11); set_glyph('.', 0, 16); set_glyph(',', 0, 17);
    set_glyph(':', 0, 20); set_glyph('~', 0, 24);
    for (int i = 0; i < 26; i++) {
        set_glyph('A' + i, 1, i);
        set_glyph('a' + i, 2, i);
    }
    set_glyph('&', 1, 26); set_glyph('\'', 2, 26);
    for (int i = 0; en_extra_order[i]; i++) set_glyph(en_extra_order[i], EXTRA_PAGE, i);
    const uint8_t *rom = Port_HostRom();
    for (int p = 0; p < 3; p++) measure(p, rom + FONT_ROM + p * PAGE_BYTES);
    measure(EXTRA_PAGE, en_extra_page);

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
        /* unescape \n, \f, \x02 */
        char *w = text;
        for (char *r = text; *r; r++) {
            if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
            else if (r[0] == '\\' && r[1] == 'f') { *w++ = '\f'; r++; }
            else if (r[0] == '\\' && r[1] == '2') { *w++ = '\x02'; r++; }
            else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
            else *w++ = *r;
        }
        *w = 0;
        if (at >= 0x1000000) continue;
        if (entry_count == cap) {
            cap = cap ? cap * 2 : 1024;
            entries = realloc(entries, (size_t)cap * sizeof *entries);
        }
        entries[entry_count].hash = hash_bytes(rom + at);
        entries[entry_count].text = strdup(text);
        entries[entry_count].drawn = NULL;
        entries[entry_count].wrapped = NULL;
        entry_count++;
    }
    fclose(f);
    qsort(entries, (size_t)entry_count, sizeof *entries, by_hash);
}

int Port_TextEnglish(void) {
    if (!entry_count) return 0;
    if (forced >= 0) return forced;
    int lang = Port_Rt64Language();
    return lang > 0;
}

void Port_TextFontLoaded(void) { memcpy(EXTRA_ADDR, en_extra_page, sizeof en_extra_page); }

uint8_t *Port_TextExtraPage(int page) { return EXTRA_ADDR + (page - EXTRA_PAGE) * PAGE_BYTES; }

int Port_TextGlyphWidth(int page, int glyph, int *left) {
    if (page < 0 || page >= 32 || glyph < 0 || glyph >= 30) {
        *left = 0;
        return 10;
    }
    *left = glyph_left[page][glyph];
    return glyph_width[page][glyph];
}

int Port_TextSpaceWidth(void) { return SPACE; }

/* ---- layout ---- */

static int is_open_quote(const char *text, const char *p) {
    if (p == text) return 1;
    char c = p[-1];
    return c == ' ' || c == '\n' || c == '\f' || c == '(' || c == '*' || c == '-';
}

/* the page and glyph for one character (quotes by position), or -1 */
static int glyph_of(const char *text, const char *p, int *page) {
    unsigned char c = (unsigned char)*p;
    if (c == '"') {
        *page = 2;
        return is_open_quote(text, p) ? 28 : 27;
    }
    if (c >= 128 || glyph_page[c] < 0) return -1;
    *page = glyph_page[c];
    return glyph_index[c];
}

static int char_width(const char *text, const char *p) {
    if (*p == ' ') return SPACE;
    int page, g = glyph_of(text, p, &page);
    return g < 0 ? 0 : glyph_width[page][g];
}

typedef struct {
    uint8_t *out;
    int n, cap, page;
} Writer;

static void put(Writer *w, uint8_t b) {
    if (w->n < w->cap - 1) w->out[w->n++] = b;
}

static void put_char(Writer *w, const char *text, const char *p) {
    if (*p == ' ') { put(w, 0x7F); return; }
    if (*p == '\x02') { put(w, 0xF2); return; }
    int page, g = glyph_of(text, p, &page);
    if (g < 0) return;
    if (page != w->page) {
        put(w, (uint8_t)(0x80 | page));
        w->page = page;
    }
    put(w, (uint8_t)g);
}

/* Word-wrap `text` to `width` pixels and `rows` lines a page. */
static int layout(uint8_t *out, int cap, const char *text, int width, int rows) {
    Writer w = { out, 0, cap, -1 };
    put(&w, 0xA0);
    put(&w, 0xC1);
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
            const char *word = p + 1;
            int ww = 0;
            const char *q = word;
            while (*q && *q != ' ' && *q != '\n' && *q != '\f') ww += char_width(text, q++);
            if (line_has_text && x + SPACE + ww > width) {
                if (++row >= rows) { put(&w, 0xF0); row = 0; }
                else put(&w, 0xE0);
                x = 0; line_has_text = 0;
            } else {
                put(&w, 0x7F);
                x += SPACE;
            }
            p++;
            continue;
        }
        x += char_width(text, p);
        put_char(&w, text, p);
        line_has_text = 1;
        p++;
    }
    out[w.n++] = 0xFF;
    return w.n;
}

const uint8_t *Port_TextTranslate(const uint8_t *jp) {
    if (!Port_TextEnglish()) return jp;
    Entry *e = find(jp);
    if (!e) {
        if (jp[0] == 0xA0 && jp[1] != 0xC1) log_miss(jp);
        return jp;
    }
    if (!e->drawn) {
        uint8_t buf[MAX_TEXT];
        int n = layout(buf, sizeof buf, e->text, 1 << 20, 1 << 20);
        e->drawn = malloc((size_t)n);
        memcpy(e->drawn, buf, (size_t)n);
    }
    return e->drawn;
}

const uint8_t *Port_TextTranslateWrapped(const uint8_t *jp, int width) {
    if (!Port_TextEnglish()) return jp;
    Entry *e = find(jp);
    if (!e) return Port_TextTranslate(jp);
    if (!e->wrapped || e->wrapped_width != width) {
        uint8_t buf[MAX_TEXT];
        int n = layout(buf, sizeof buf, e->text, width, 1 << 20);
        free(e->wrapped);
        e->wrapped = malloc((size_t)n);
        memcpy(e->wrapped, buf, (size_t)n);
        e->wrapped_width = width;
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

int Port_TextLayoutMessage(uint8_t *out, int cap, const uint8_t *jp, int width, int rows) {
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
    if (rows < 1) rows = 1;
    layout(out, cap, text, width, rows);
    return 1;
}

int Port_TextLineWidth(const uint8_t *s) {
    int x = 0, best = 0, font = 0, page = 0;
    for (; *s != 0xFF; s++) {
        uint8_t b = *s;
        if (b == 0x7F) x += font == 1 ? SPACE : 10;
        else if (b < 0x80) {
            int left;
            x += font == 1 ? Port_TextGlyphWidth(page, b, &left) : 10;
        } else if (b < 0xA0) page = b & 0x1F;
        else if (b >= 0xC0 && b < 0xE0 && (b & 0x1F) < 16) font = b & 0x1F;
        else if (b == 0xE0) { if (x > best) best = x; x = 0; }
    }
    return x > best ? x : best;
}
