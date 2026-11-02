/* The spell UI (port/include/port/spell_ui.h, docs/SPELL_UI.md).
 *
 * Two halves, both on the game's thread:
 * - Port_SpellUiRead, at each controller read: the pointer's clicks and drags on the panel and the quick bar, the
 *   keys, and the quick bar's casts, turned into the C buttons, A and B the controller would press (one press a
 *   read, a read without it between, so the game sees each as a new press).
 * - Port_SpellUiDraw, last in Hud_Draw: the panel and the bar as native 2D items, drawn with the game's own HUD
 *   code and textures (the plates, gems, crystals, digits and the spell-name font in RDRAM) into a display list of
 *   our own that is thrown away: the frame's list is untouched, the native core records the rectangles.
 * What the game is doing is read from its own state (player_state, core_flags) and its own rules
 * (Magic_LookupSpellSequence, Magic_GetSpellRestrictionFlags, Player_CanAct); nothing here writes game state. */
#include "common.h"
#include "game/battle/battle.h"
#include "game/core/main.h"
#include "game/magic/magic.h"
#include "game/player/player.h"
#include "game/save/save_menu.h"
#include "game/ui/hud.h"
#include "game/native_draw/native_draw.h"
#include "port/data.h"
#include "port/gfx.h"
#include "port/mouse_menu.h"
#include "port/os.h"
#include "port/spell_aim.h"
#include "port/spell_ui.h"
#include "port/text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "game/native_draw/nd_2d_gbi.h" /* last: the gbi macros tell the native 2D core */

/* the game's HUD pieces (the common block in RDRAM; hud.c) */
extern HudTextureDesc D_803A86F8; /* the main atlas: parchment, crystals, digits */
extern HudTextureDesc D_803A8718; /* the battle atlas: plates, gems */
extern HudTextureDesc D_803A8748; /* the spell-name font (CI4, 8x9 glyphs; English: the port's Latin letters) */
extern u8 D_8039ADB0[];           /* the main atlas's palette */
extern u8 D_8039EC90[];           /* the battle atlas's palette */
extern Gfx D_803A8788[];          /* the HUD's set-up list */
extern u8 **D_800C03D8[];         /* the spell names, per element */
extern u16 D_800C186E[];          /* MP by the number of elements (Magic_GetSpellRestrictionFlags) */
extern u8 *D_803A8778[];          /* the spell-name font's palettes (16 colours each) */
s32 Hud_MeasureGlyphString(u8 *str);
void Hud_DrawGlyphString(s32 x, s32 y, s32 pal, u8 *str);
void Hud_DrawNumber(u16 flags, s32 x, s32 y, s32 value);
void Hud_LoadTextureTile(HudTextureDesc *tex, s32 x, s32 y, s32 w, s32 h);
void Hud_DrawClippedTextureRect(s32 x, s32 y, s32 x2, s32 y2, s32 s, s32 t, s32 dsdx, s32 dtdy);
void Hud_DrawTextureTile(void *tex, s32 x, s32 y, s32 w, s32 h, s32 s, s32 t, s32 dsdx, s32 dtdy);

/* ---- layout (game pixels: 240 high; the panel CENTRE-anchored in the 320 middle, the bar LEFT) ------------------ */
enum {
    HINT_Y = 92,                                     /* "Click plate to cast · R-click to undo" */
    PLATE_W = 184, PLATE_H = 14, PLATE_X = 160 - PLATE_W / 2, PLATE_Y = 103,
    CRYSTAL_PITCH = 74, CRYSTAL_X0 = 49,             /* crystal i's middle: 49 + 74 i (49, 123, 197, 271) */
    CRYSTAL_Y = 121, CRYSTAL_SIZE = 24,              /* the HUD's 12x12 crystals at 2x */
    NEXT_W = 70, NEXT_H = 22, NEXT_Y = 149,          /* the next-spell plates (bottom 171: above the minimap's 176) */
    BAR_X = 124, BAR_Y = 194, BAR_H = 21,            /* the quick bar (spec 1.3) */
    SLOT_W = 17, SLOT_H = 15, SLOT_GAP = 3, LABEL_W = 38, DOTS_W = 14,
    SLOTS = 6, PAGES = 3,
    TIP_BOTTOM = 190,
    PANEL_X0 = 8, PANEL_X1 = 312, PANEL_Y0 = 86, PANEL_Y1 = 240, /* the open panel's area and below it (the bar, the HUD) */
};

/* where the game's pieces are in its atlases */
enum {
    PLAIN_S = 100, PLAIN_T = 0, CHEVRON_S = 100, CHEVRON_T = 14, GAME_PLATE_W = 110, /* battle atlas, 110x14 */
    GEM_S = 64, GEM_T = 36, GEM = 11,                                                 /* battle atlas, + 11 e */
    CRYSTAL_S = 32, CRYSTAL_T = 16, CRYSTAL = 12,                                     /* main atlas, + 12 e */
    PARCH_S = 0, PARCH_T = 0,                                                         /* main atlas, 16x16 */
};

/* the element order on screen (fire, earth, water, wind = the game's element 0..3) and its C buttons */
static const u16 c_button[4] = { 0x0008, 0x0002, 0x0004, 0x0001 };

typedef struct Rect {
    s16 x, y, w, h;
} Rect;

static int in_rect(const Rect *r, float x, float y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

static int picture_wide(void) { return 2 * (int)lroundf(ND_ASPECT() * 120.0f) >= 400; }

static Rect crystal_rect(int e) { /* the crystal with its digits: the click's */
    Rect r = { (s16)(CRYSTAL_X0 + CRYSTAL_PITCH * e - 16), CRYSTAL_Y - 3, 36, 28 };
    return r;
}
static Rect next_rect(int e) {
    Rect r = { (s16)(CRYSTAL_X0 + CRYSTAL_PITCH * e - NEXT_W / 2), NEXT_Y, NEXT_W, NEXT_H };
    return r;
}
static Rect plate_rect(void) {
    Rect r = { PLATE_X, PLATE_Y, PLATE_W, PLATE_H };
    if (Port_SpellAimCollapsed()) { /* aiming: the plate alone, at the top (port/spell_aim.h) */
        s32 x, y, w, h;
        Port_SpellAimPlateRect(&x, &y, &w, &h);
        r.x = (s16)x, r.y = (s16)y, r.w = (s16)w, r.h = (s16)h;
    }
    return r;
}
static Rect bar_rect(void) {
    Rect r = { BAR_X, BAR_Y, (s16)(3 + (picture_wide() ? LABEL_W : 0) + SLOTS * SLOT_W + (SLOTS - 1) * SLOT_GAP +
                                   (picture_wide() ? DOTS_W : 0) + 3), BAR_H };
    return r;
}
static Rect slot_rect(int i) {
    Rect r = { (s16)(BAR_X + 3 + (picture_wide() ? LABEL_W : 0) + i * (SLOT_W + SLOT_GAP)), BAR_Y + 3, SLOT_W, SLOT_H };
    return r;
}
static Rect dot_rect(int p) { /* the page dots (16:9), with room round them for the click */
    Rect s = slot_rect(SLOTS - 1);
    Rect r = { (s16)(s.x + SLOT_W + 3), (s16)(BAR_Y + 2 + p * 6), 10, 6 };
    return r;
}

/* ---- state ------------------------------------------------------------------------------------------------------ */
typedef struct Spell {
    u8 len;     /* 0: empty */
    u8 dirs[3];
} Spell;

enum { T_NONE, T_PLATE, T_CRYSTAL, T_NEXT, T_SLOT, T_DOT, T_BAR };
enum { I_NONE, I_CRYSTAL, I_BACK, I_CAST };

static struct {
    int host_on;         /* the host's switch: -1 none (headless: ELTALE_SPELL_UI) */
    u32 keys;            /* pressed since the last read */
    Spell slots[PAGES * SLOTS];
    int page;
    /* the per-save file: its key (0 none: a new game until saved) */
    u64 save_key;
    int forgot;          /* the title or the intro was seen: the bar forgot its save */
    /* the pointer */
    float px, py;        /* in the panel's (CENTRE) and the bar's (LEFT) units */
    float lx, ly;
    int pflags;
    int hover, hover_arg; /* what it is over (T_*) */
    int hover_ticks;
    int armed, armed_arg; /* a press on the plate or a slot, waiting for its release */
    float press_x, press_y;
    int dragging;         /* T_PLATE or T_SLOT once the press moved */
    int held_target, held_arg; /* pressed and still held (drawn 1 pixel down) */
    /* what the reads press */
    int wait;            /* a press went out at the last read: this one is the release */
    int intent, intent_arg;
    int macro;           /* a quick-bar spell (or Esc's clearing) going in */
    Spell macro_spell;
    int macro_cast, macro_a_sent, macro_tries, macro_last_count;
    int log_after;
    /* what was shown (for the log) */
    char view[64];
    /* the drawing */
    int panel_t;         /* the panel's opening (0..OPEN_TICKS) */
    int panel_closing;   /* ticks of the closing fade left */
    Spell panel_last;
    int bar_alpha;       /* 0..255, fades on the enemy's turn */
    int tip_alpha;
} S = { .host_on = -1 };

static int log_on = -1;

static int log_level(void) {
    if (log_on < 0) log_on = getenv("ELTALE_SPELLUI_LOG") ? atoi(getenv("ELTALE_SPELLUI_LOG")) : 0;
    return log_on;
}

int Port_SpellUiEnabled(void) {
    static int env = -2;
    if (S.host_on >= 0) return S.host_on;
    if (env == -2) env = getenv("ELTALE_SPELL_UI") ? atoi(getenv("ELTALE_SPELL_UI")) != 0 : 0;
    return env;
}

void Port_SpellUiKeys(u32 keys, int on) {
    S.keys |= keys;
    if (on >= 0) S.host_on = on & 1;
    Port_SpellAimSwitch(on); /* (the host's bits 2 and 4: click to aim spells, port/spell_aim.h) */
}

/* ---- the game's rules ------------------------------------------------------------------------------------------ */

static int count_now(void) { return player_state.spell_input_count; }

static int building(void) { return (core_flags & 4) && player_state.spell_input_count; }

/* The game shows the HUD and nothing over it (no text window, menu, cutscene, element allocation) */
static int ui_shown(void) {
    if (!Port_SpellUiEnabled() || core_game_mode != 1) return 0;
    if (core_flags & (0x1 | 0x2 | 0x8 | 0x80 | 0x400 | 0x800 | 0x4000)) return 0;
    if (D_8007A172 == 4 || !player_state.stats || player_state.stats->hp == 0) return 0;
    return 1;
}

/* The game takes a first element now (Player_Update: no spell input, menu or text, and the player can act) */
static int can_open(void) {
    return !(core_flags & 0xF) && Player_CanAct(&player_state, &player_state.actor) &&
           !(player_state.actor.battle->status_bits & 0x40);
}

static int ui_live(void) { return ui_shown() && (building() || can_open()); }

/* Magic_GetSpellRestrictionFlags of a sequence: 4 not learned (spirit level), 2 not here (battle / field), 1 MP */
static int seq_flags(const u8 *dirs, int len, u32 *id_out) {
    u8 buf[3];
    memcpy(buf, dirs, (size_t)len);
    u32 id = (u32)Magic_LookupSpellSequence((u16)len, buf);
    if (id_out) *id_out = id;
    return Magic_GetSpellRestrictionFlags(id, player_state.stats->mp);
}

/* The game would take element e next (Player_HandleSpellInput: refused when not learned or no MP) */
static int element_ok(int e) {
    int n = count_now();
    u8 d[3];
    if (n >= 3) return 0;
    memcpy(d, player_state.spell_input_dirs, 3);
    d[n] = (u8)e;
    return !(seq_flags(d, n + 1, NULL) & 5);
}

static u16 mp_cost(int len) { return len >= 1 && len <= 3 ? D_800C186E[len] : 0; }

/* A kept spell: 'c' castable now, 'm' not enough MP (or not learned), 'x' not castable here, '-' empty */
static int spell_state(const Spell *s) {
    if (!s->len) return '-';
    for (int k = 1; k <= s->len; k++) {
        int f = seq_flags(s->dirs, k, NULL);
        if (f & 4) return 'm';
        if (f & 1) return 'm';
        if (k == s->len && f) return 'x';
    }
    return 'c';
}

/* A spell's name in the HUD's font (English: the port's Latin letters; Japanese: the game's own) */
static const u8 *spell_name(u32 id) {
    int g = (int)(id >> 8) & 0xF, i = (int)id & 0xFF;
    const u8 *en = Port_HudSpellName(g, i);
    if (en) return en;
    return (const u8 *)Port_N64Ptr(Port_N64Ptr(D_800C03D8, g), i);
}

/* ---- the quick bar's file (per save, as the area map's fog of war: the save's record hashed) -------------------- */

static void data_path(char *out, size_t size, const char *leaf, int make) {
    const char *dir = Port_WindowDataDir();
    const char *env = getenv("ELTALE_DATA_DIR");
    if (!strcmp(dir, ".") && env && *env) dir = env;
    snprintf(out, size, "%s/spellbar", dir);
    if (make) make_dir(out);
    snprintf(out, size, "%s/spellbar/%s", dir, leaf);
}

static void bar_write(void) {
    char path[1024], tmp[1100], leaf[64];
    if (!S.save_key) return;
    snprintf(leaf, sizeof leaf, "save_%016llx.bar", (unsigned long long)S.save_key);
    data_path(path, sizeof path, leaf, 1);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "ELTSPELLBAR 1\npage %d\n", S.page);
    for (int i = 0; i < PAGES * SLOTS; i++) {
        const Spell *s = &S.slots[i];
        if (!s->len) continue;
        fprintf(f, "slot %d", i);
        for (int k = 0; k < s->len; k++) fprintf(f, " %d", s->dirs[k]);
        fprintf(f, "\n");
    }
    int ok = fclose(f) == 0;
    if (!ok) { remove(tmp); return; }
    remove(path);
    rename(tmp, path);
}

static void bar_clear(void) {
    memset(S.slots, 0, sizeof S.slots);
    S.page = 0;
}

static int bar_read(void) {
    char path[1024], leaf[64], line[128];
    bar_clear();
    snprintf(leaf, sizeof leaf, "save_%016llx.bar", (unsigned long long)S.save_key);
    data_path(path, sizeof path, leaf, 0);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fgets(line, sizeof line, f) && !strncmp(line, "ELTSPELLBAR 1", 13);
    while (ok && fgets(line, sizeof line, f)) {
        int i, d[3], page, n;
        if (sscanf(line, "page %d", &page) == 1 && page >= 0 && page < PAGES) S.page = page;
        n = sscanf(line, "slot %d %d %d %d", &i, &d[0], &d[1], &d[2]);
        if (n >= 2 && i >= 0 && i < PAGES * SLOTS) {
            Spell s = { (u8)(n - 1), { 0, 0, 0 } };
            for (int k = 0; k < n - 1; k++) {
                if (d[k] < 0 || d[k] > 3) s.len = 0;
                s.dirs[k] = (u8)(d[k] & 3);
            }
            S.slots[i] = s;
        }
    }
    fclose(f);
    return ok ? 1 : -1;
}

void Port_SpellUiSaveEvent(int kind, s32 slot) {
    if (slot < 0 || slot >= 16 || !Port_SpellUiEnabled()) return; /* (off: no files, as the original) */
    const SaveHeader *e = &save_menu_save_headers[slot];
    u64 h = 0xCBF29CE484222325ull;
    const u32 head[4] = { e->play_time, e->day_count, (u32)e->map_id, (u32)e->subarea };
    const u8 *parts[2] = { (const u8 *)head, (const u8 *)&save_menu_game_state_block };
    const size_t sizes[2] = { sizeof head, sizeof save_menu_game_state_block };
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; i < sizes[k]; i++) h = (h ^ parts[k][i]) * 0x100000001B3ull;
    S.save_key = h;
    S.forgot = 0;
    int result = 1;
    if (kind == 0) bar_write();
    else result = bar_read();
    if (log_level())
        printf("P1_SPELLUI_SAVE vi=%llu %s slot=%d key=%016llx result=%d\n", (unsigned long long)Port_OsViCount(),
               kind ? "read" : "write", (int)slot, (unsigned long long)h, result);
}

static void slots_changed(void) {
    bar_write();
    if (log_level()) {
        printf("P1_SPELLUI_SLOTS vi=%llu page=%d", (unsigned long long)Port_OsViCount(), S.page);
        for (int i = 0; i < PAGES * SLOTS; i++) {
            const Spell *s = &S.slots[i];
            printf(" ");
            if (!s->len) printf("-");
            for (int k = 0; k < s->len; k++) printf("%d", s->dirs[k]);
        }
        printf("\n");
    }
}

/* ---- the reads ------------------------------------------------------------------------------------------------- */

static void press(u16 *buttons, u16 b, const char *why) {
    *buttons |= b;
    S.wait = 1;
    S.log_after = 1;
    if (log_level())
        printf("P1_SPELLUI_PRESS vi=%llu buttons=%04x why=%s count=%d\n", (unsigned long long)Port_OsViCount(), b, why,
               count_now());
}

/* The one place the UI casts: A, which the game takes as the controller's (Player_HandleSpellInput: the spell on the
 * plate, with the game's own aim and target rules). The plate's click, the quick bar's last step and (later) a click
 * on the battlefield to aim all come here (docs/SPELL_UI.md "Aiming"). */
static void cast_press(u16 *buttons, const char *why) { press(buttons, 0x8000, why); }

static void macro_start(const Spell *s, int cast) {
    S.macro = 1;
    S.macro_spell = *s;
    S.macro_cast = cast;
    S.macro_a_sent = 0;
    S.macro_tries = 0;
    S.macro_last_count = -1;
    S.intent = I_NONE;
}

static void macro_end(const char *why) {
    if (S.macro && log_level())
        printf("P1_SPELLUI_MACRO vi=%llu end=%s count=%d\n", (unsigned long long)Port_OsViCount(), why, count_now());
    S.macro = 0;
}

/* one step of a quick-bar spell: B while what is in is not the start of it, then its elements, then A */
static void macro_step(u16 *buttons) {
    const Spell *t = &S.macro_spell;
    const int n = count_now();
    int prefix = n <= t->len;
    for (int k = 0; k < n && prefix; k++) prefix = player_state.spell_input_dirs[k] == t->dirs[k];
    if (S.macro_a_sent) {
        /* A went in: cast (the input closed), or pressed again once if a read missed it */
        if (!building()) macro_end("cast");
        else if (++S.macro_tries > 2) macro_end("refused");
        else cast_press(buttons, "slot-cast-again");
        return;
    }
    if (!ui_live()) {
        macro_end("hidden");
        return;
    }
    if (n == S.macro_last_count && ++S.macro_tries > 3) {
        macro_end("stuck");
        return;
    }
    if (n != S.macro_last_count) S.macro_tries = 0;
    S.macro_last_count = n;
    if (n && !prefix) {
        press(buttons, 0x4000, "slot-back");
    } else if (n < t->len) {
        if (!element_ok(t->dirs[n])) {
            macro_end("refused");
            return;
        }
        press(buttons, c_button[t->dirs[n]], "slot-element");
    } else if (S.macro_cast && t->len) {
        if (Port_SpellUiReady() < 0) {
            macro_end("not-castable");
            return;
        }
        S.macro_a_sent = 1;
        S.macro_tries = 0;
        cast_press(buttons, "slot-cast");
    } else {
        macro_end(t->len ? "built" : "cleared");
    }
}

/* The pointer is on the panel or the bar: a piece of them, or anywhere in the panel's area while it is open in full (the
 * middle and everything under it: from a HUD crystal to the panel the pointer never crosses the battlefield), so aiming
 * starts once the pointer leaves it (port/spell_aim.h) */
int Port_SpellUiPointerOver(void) {
    if (!Port_SpellUiEnabled() || !(S.pflags & PORT_POINTER_IN)) return 0;
    if (S.hover != T_NONE) return 1;
    if (building() && ui_shown() && !Port_SpellAimCollapsed())
        return S.px >= PANEL_X0 && S.px < PANEL_X1 && S.py >= PANEL_Y0 && S.py < PANEL_Y1;
    return 0;
}

s32 Port_SpellUiReady(void) {
    u32 id;
    if (!building() || !ui_shown()) return -1;
    return seq_flags(player_state.spell_input_dirs, count_now(), &id) == 0 ? (s32)id : -1;
}

int Port_SpellUiCast(void) {
    if (Port_SpellUiReady() < 0) return 0;
    S.intent = I_CAST;
    macro_end("cast-asked");
    return 1;
}

/* the slot under the pointer (page-relative) or -1, in LEFT units */
static int slot_at(float x, float y) {
    for (int i = 0; i < SLOTS; i++) {
        Rect r = slot_rect(i);
        if (in_rect(&r, x, y)) return i;
    }
    return -1;
}

/* what the pointer is over: T_* and its argument */
static int target_at(int *arg) {
    *arg = 0;
    if (!ui_shown()) return T_NONE;
    if (building()) {
        Rect r = plate_rect();
        if (in_rect(&r, S.px, S.py)) return T_PLATE;
        for (int e = 0; e < 4 && !Port_SpellAimCollapsed(); e++) {
            r = crystal_rect(e);
            if (in_rect(&r, S.px, S.py)) return *arg = e, T_CRYSTAL;
            r = next_rect(e);
            if (in_rect(&r, S.px, S.py)) return *arg = e, T_NEXT;
        }
    }
    int s = slot_at(S.lx, S.ly);
    if (s >= 0) return *arg = s, T_SLOT;
    if (picture_wide())
        for (int p = 0; p < PAGES; p++) {
            Rect r = dot_rect(p);
            if (in_rect(&r, S.lx, S.ly)) return *arg = p, T_DOT;
        }
    Rect b = bar_rect();
    if (in_rect(&b, S.lx, S.ly)) return T_BAR;
    return T_NONE;
}

static void set_page(int p) {
    p = (p % PAGES + PAGES) % PAGES;
    if (p == S.page) return;
    S.page = p;
    slots_changed();
}

/* a slot clicked or its key: cast what it holds; empty while a spell is in: keep that spell there */
static void slot_use(int i) {
    Spell *s = &S.slots[S.page * SLOTS + i];
    if (s->len) {
        if (!ui_live() || spell_state(s) != 'c') {
            if (log_level())
                printf("P1_SPELLUI_SLOT_REFUSED vi=%llu slot=%d state=%c\n", (unsigned long long)Port_OsViCount(), i,
                       spell_state(s));
            return;
        }
        macro_start(s, 1);
    } else if (building()) {
        s->len = (u8)count_now();
        memcpy(s->dirs, player_state.spell_input_dirs, 3);
        slots_changed();
    }
}

int Port_SpellUiWheel(int wheel) {
    float x, y;
    int f;
    if (!wheel || !ui_shown()) return wheel;
    Port_MousePointerGet(&x, &y, &f);
    if (!(f & PORT_POINTER_IN)) return wheel;
    Port_MouseToAnchor(PORT_MOUSE_LEFT, &x, &y);
    Rect b = bar_rect();
    if (!in_rect(&b, x, y)) return wheel;
    set_page(S.page + (wheel > 0 ? -1 : 1));
    return 0;
}

static void log_view(void) {
    char v[64];
    int n = 0;
    if (!log_level()) return;
    v[n++] = ui_shown() ? 'S' : 's';
    v[n++] = ui_live() ? 'L' : 'l';
    v[n++] = building() ? 'B' : 'b';
    v[n++] = ' ';
    for (int e = 0; e < 4; e++) v[n++] = building() ? (element_ok(e) ? '1' : '0') : '.';
    v[n++] = ' ';
    for (int i = 0; i < SLOTS; i++) {
        const Spell *s = &S.slots[S.page * SLOTS + i];
        v[n++] = (char)(s->len ? spell_state(s) : building() ? '+' : '-');
    }
    v[n] = 0;
    if (!strcmp(v, S.view)) return;
    strcpy(S.view, v);
    printf("P1_SPELLUI_VIEW vi=%llu view=%s page=%d mp=%d count=%d\n", (unsigned long long)Port_OsViCount(), v, S.page,
           player_state.stats ? player_state.stats->mp : -1, count_now());
}

static void dump_table(void) {
    static int done;
    if (done || log_level() < 2 || !player_state.stats) return;
    done = 1;
    printf("P1_SPELLUI_MP_BY_LENGTH %d %d %d %d\n", D_800C186E[0], D_800C186E[1], D_800C186E[2], D_800C186E[3]);
    printf("P1_SPELLUI_LABEL_PALETTES");
    for (int i = 0; i < 8; i++) printf(" %d", hud_spell_label_palettes[i]);
    printf("\n");
    for (int p = 0; p < 4; p++) { /* (the font has 4) */
        const u8 *pal = (const u8 *)Port_N64Ptr(D_803A8778, p);
        printf("P1_SPELLUI_PALETTE %d", p);
        for (int i = 0; i < 16; i++) printf(" %04x", pal[2 * i] << 8 | pal[2 * i + 1]);
        printf("\n");
    }
    for (int g = 0; g < 4; g++)
        for (int i = 0; i < 15; i++) {
            const SpellDef *r = &MAGIC_SPELLS(g)[i];
            const char *name = Port_TextSpell(g, i);
            printf("P1_SPELLUI_SPELL %d %d len=%d seq=", g, i, r->input_length);
            for (int k = 0; k < r->input_length && k < 4; k++) printf("%d", r->input_sequence[k]);
            printf(" level=%d flags=%x mp=%d name=%s\n", r->required_level, r->flags, mp_cost(r->input_length),
                   name ? name : "?");
        }
    /* every sequence: what the game looks up (a prefix of every spell is a spell) */
    for (int a = 0; a < 4; a++)
        for (int b = -1; b < 4; b++)
            for (int c = -1; c < 4; c++) {
                u8 d[3] = { (u8)a, (u8)(b < 0 ? 0 : b), (u8)(c < 0 ? 0 : c) };
                int len = b < 0 ? 1 : c < 0 ? 2 : 3;
                if ((b < 0 && c >= 0)) continue;
                u32 id = (u32)Magic_LookupSpellSequence((u16)len, d);
                const SpellDef *r = &MAGIC_SPELLS((id >> 8) & 0xF)[id & 0xFF];
                int exact = r->input_length == len;
                printf("P1_SPELLUI_SEQ %d%s%s -> %d.%d %s\n", a, b < 0 ? "" : (char[]){ (char)('0' + b), 0 },
                       c < 0 ? "" : (char[]){ (char)('0' + c), 0 }, (int)(id >> 8) & 0xF, (int)id & 0xFF,
                       exact ? "match" : "fallback");
            }
}

void Port_SpellUiRead(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    (void)stick_x;
    (void)stick_y;
    const u32 keys = S.keys;
    S.keys = 0;
    if (!Port_SpellUiEnabled()) {
        S.macro = S.armed = S.dragging = S.wait = 0;
        S.intent = I_NONE;
        S.hover = T_NONE;
        return;
    }
    /* the title or the intro: the next game starts with an empty bar */
    if (core_game_mode == 3 || core_game_mode == 4) {
        if (!S.forgot) {
            bar_clear();
            S.save_key = 0;
            S.forgot = 1;
        }
        return;
    }
    if (!player_state.stats) return;
    dump_table();
    if (S.log_after && log_level()) {
        S.log_after = 0;
        printf("P1_SPELLUI_STATE vi=%llu count=%d dirs=%d%d%d core_flags=%x mp=%d spell_id=%x\n",
               (unsigned long long)Port_OsViCount(), count_now(), player_state.spell_input_dirs[0],
               player_state.spell_input_dirs[1], player_state.spell_input_dirs[2], core_flags,
               player_state.stats ? player_state.stats->mp : -1, player_state.spell_id);
    }

    /* the pointer */
    float x, y;
    Port_MousePointerGet(&x, &y, &S.pflags);
    const int in = (S.pflags & PORT_POINTER_IN) != 0;
    S.px = S.lx = x;
    S.py = S.ly = y;
    Port_MouseToAnchor(PORT_MOUSE_CENTRE, &S.px, &S.py);
    Port_MouseToAnchor(PORT_MOUSE_LEFT, &S.lx, &S.ly);
    int arg = 0, over = in ? target_at(&arg) : T_NONE;
    if (over == T_SLOT && (S.hover != T_SLOT || S.hover_arg != arg)) S.hover_ticks = 0;
    else if (over == T_SLOT) S.hover_ticks++;
    S.hover = over;
    S.hover_arg = arg;
    int take = 0;
    const int left = in && (S.pflags & PORT_POINTER_LEFT), up = (S.pflags & PORT_POINTER_UP) != 0;
    const int right = in && (S.pflags & PORT_POINTER_RIGHT);
    const int live = ui_live();
    if ((left || up || right) && log_level())
        printf("P1_SPELLUI_CLICK vi=%llu pointer=%.1f,%.1f centre=%.1f left=%.1f flags=%x over=%d arg=%d armed=%d live=%d ready=%d battle=%x core=%x\n",
               (unsigned long long)Port_OsViCount(), x, y, S.px, S.lx, S.pflags, over, arg, S.armed, live, (int)Port_SpellUiReady(),
               battle_flags, core_flags);

    if (left && over != T_NONE) {
        take |= PORT_POINTER_LEFT;
        S.held_target = over;
        S.held_arg = arg;
        switch (over) {
        case T_CRYSTAL:
        case T_NEXT:
            if (live && element_ok(arg)) S.intent = I_CRYSTAL, S.intent_arg = arg, macro_end("click");
            break;
        case T_PLATE:
        case T_SLOT:
            S.armed = over;
            S.armed_arg = arg;
            S.press_x = over == T_PLATE ? S.px : S.lx;
            S.press_y = over == T_PLATE ? S.py : S.ly;
            break;
        case T_DOT:
            set_page(arg);
            break;
        default:
            break;
        }
    }
    if (!(S.pflags & PORT_POINTER_HELD) && !left) S.held_target = T_NONE;
    /* a drag: the plate (a spell in) or a kept spell, moved more than 3 units while held */
    if (S.armed && !S.dragging && (S.pflags & PORT_POINTER_HELD)) {
        const float ax = S.armed == T_PLATE ? S.px : S.lx, ay = S.armed == T_PLATE ? S.py : S.ly;
        if (fabsf(ax - S.press_x) + fabsf(ay - S.press_y) > 3.0f &&
            (S.armed == T_SLOT ? S.slots[S.page * SLOTS + S.armed_arg].len != 0 : building()))
            S.dragging = S.armed;
    }
    if (up && S.armed) {
        take |= PORT_POINTER_UP;
        if (S.dragging) {
            const int to = in ? slot_at(S.lx, S.ly) : -1;
            Spell *from = S.dragging == T_SLOT ? &S.slots[S.page * SLOTS + S.armed_arg] : NULL;
            Rect b = bar_rect();
            if (to >= 0) {
                Spell *dst = &S.slots[S.page * SLOTS + to];
                if (from) {
                    Spell t = *dst;
                    *dst = *from;
                    *from = t;
                } else if (building()) {
                    dst->len = (u8)count_now();
                    memcpy(dst->dirs, player_state.spell_input_dirs, 3);
                }
                slots_changed();
            } else if (from && !(in && in_rect(&b, S.lx, S.ly))) {
                memset(from, 0, sizeof *from); /* dropped off the bar: cleared */
                slots_changed();
            }
        } else if (over == S.armed && (over != T_SLOT || arg == S.armed_arg)) {
            if (over == T_PLATE) Port_SpellUiCast();
            else slot_use(arg);
        }
        S.armed = S.dragging = 0;
    }
    if (!(S.pflags & PORT_POINTER_HELD) && !up && !left) S.armed = S.dragging = 0;
    /* right click: the last element back (as B); with nothing in, nothing */
    if (right && building() && live) {
        take |= PORT_POINTER_RIGHT;
        S.intent = I_BACK;
        macro_end("right");
    }
    /* (a click on the battlefield, none of the UI's, while a spell is ready: click to aim, port/p0/spell_aim.c, which
     * reads the pointer after the menus) */
    if (over != T_NONE) take |= PORT_POINTER_LEFT | PORT_POINTER_UP;
    Port_MousePointerTake(take);

    /* the keys */
    if (keys && log_level()) printf("P1_SPELLUI_KEYS vi=%llu keys=%#x\n", (unsigned long long)Port_OsViCount(), keys);
    for (int e = 0; e < 4; e++)
        if ((keys & (PORT_SPELLKEY_CRYSTAL << e)) && live) S.intent = I_CRYSTAL, S.intent_arg = e, macro_end("key");
    for (int i = 0; i < SLOTS; i++)
        if ((keys & (PORT_SPELLKEY_SLOT << i)) && ui_shown()) slot_use(i);
    if (keys & PORT_SPELLKEY_PAGE_PREV) set_page(S.page - 1);
    if (keys & PORT_SPELLKEY_PAGE_NEXT) set_page(S.page + 1);
    if ((keys & PORT_SPELLKEY_CLEAR) && building() && live) {
        Spell none = { 0, { 0, 0, 0 } };
        macro_start(&none, 0);
    }

    /* what goes in at this read: one press, then a read without it */
    if (S.wait) {
        S.wait = 0;
    } else if (S.intent != I_NONE) {
        const int what = S.intent, a = S.intent_arg;
        S.intent = I_NONE;
        if (what == I_CRYSTAL && ui_live() && count_now() < 3) press(buttons, c_button[a & 3], "element");
        else if (what == I_BACK && building()) press(buttons, 0x4000, "back");
        else if (what == I_CAST && Port_SpellUiReady() >= 0) cast_press(buttons, "cast");
    } else if (S.macro) {
        macro_step(buttons);
    }
    log_view();
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

/* our own small pieces (RGBA5551, host order): a white block (flat colours), the hotkey digits, the empty gem, the
 * small pips, "+" */
enum { ART_W = 64, ART_H = 32 };
enum {
    ART_WHITE_S = 0, ART_WHITE_T = 0,     /* 4x4 */
    ART_DIGIT_S = 0, ART_DIGIT_T = 8,     /* 5x7 cells: yellow, outlined (the crystals' keys) */
    ART_SDIGIT_S = 0, ART_SDIGIT_T = 16,  /* 4x5 cells: ink (the slots' keys) */
    ART_EMPTY_S = 0, ART_EMPTY_T = 21,    /* 11x11 */
    ART_PIP_S = 12, ART_PIP_T = 21,       /* 5x5 cells, element 0..3 then grey */
    ART_PLUS_S = 40, ART_PLUS_T = 21,     /* 5x5 */
};
static u16 art[ART_W * ART_H];
static int art_made;

static u16 rgba5551(int r, int g, int b, int a) {
    return (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >= 128));
}

static void art_put(int x, int y, u16 c) {
    if (x >= 0 && y >= 0 && x < ART_W && y < ART_H) art[y * ART_W + x] = c;
}

static const char *const tiny_digits[10] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
    "111100111001111", "111100111101111", "111001001010010", "111101111101111", "111101111001111",
};

static void make_art(void) {
    if (art_made) return;
    art_made = 1;
    memset(art, 0, sizeof art);
    const u16 white = rgba5551(255, 255, 255, 255), black = rgba5551(0, 0, 0, 255);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) art_put(ART_WHITE_S + x, ART_WHITE_T + y, white);
    for (int d = 0; d < 10; d++) {
        const u16 yellow = rgba5551(0xF2, 0xE2, 0x7A, 255), ink = rgba5551(0x4A, 0x3C, 0x22, 255);
        /* outlined: the outline first, the digit over it */
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 3; x++)
                if (tiny_digits[d][y * 3 + x] == '1')
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) art_put(ART_DIGIT_S + d * 5 + 1 + x + dx, ART_DIGIT_T + 1 + y + dy, black);
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 3; x++)
                if (tiny_digits[d][y * 3 + x] == '1') {
                    art_put(ART_DIGIT_S + d * 5 + 1 + x, ART_DIGIT_T + 1 + y, yellow);
                    art_put(ART_SDIGIT_S + d * 4 + x, ART_SDIGIT_T + y, ink);
                }
    }
    /* the empty gem: an outlined diamond (the game's gems are 11x11) */
    for (int y = 0; y < 11; y++)
        for (int x = 0; x < 11; x++) {
            int d = abs(x - 5) + abs(y - 5);
            if (d == 5) art_put(ART_EMPTY_S + x, ART_EMPTY_T + y, rgba5551(0x3A, 0x2E, 0x1C, 255));
            else if (d == 4) art_put(ART_EMPTY_S + x, ART_EMPTY_T + y, rgba5551(0x8E, 0x7A, 0x58, 255));
        }
    /* the small pips: the game's element colours, a darker rim */
    for (int k = 0; k < 5; k++) {
        int r = 0x8A, g = 0x8A, b = 0x8A;
        if (k < 4) r = hud_element_colors[k][0], g = hud_element_colors[k][1], b = hud_element_colors[k][2];
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) {
                int d = abs(x - 2) + abs(y - 2);
                if (d == 2) art_put(ART_PIP_S + k * 6 + x, ART_PIP_T + y, rgba5551(r * 9 / 20, g * 9 / 20, b * 9 / 20, 255));
                else if (d < 2) art_put(ART_PIP_S + k * 6 + x, ART_PIP_T + y, rgba5551(r, g, b, 255));
            }
        art_put(ART_PIP_S + k * 6 + 2, ART_PIP_T + 1, rgba5551(255, 255, 255, 255)); /* a glint */
    }
    for (int i = 0; i < 5; i++) {
        art_put(ART_PLUS_S + 2, ART_PLUS_T + i, rgba5551(0x6B, 0x2A, 0x1A, 255));
        art_put(ART_PLUS_S + i, ART_PLUS_T + 2, rgba5551(0x6B, 0x2A, 0x1A, 255));
    }
}

/* the drawing's own display list (thrown away: the native core has what it needs as the commands are made) */
static Gfx dl_scratch[6144];
static Gfx *dl_saved;
static s32 saved_ox, saved_oy, saved_cl, saved_ct, saved_cr, saved_cb;
static int alpha_mul = 255; /* the fades */

/* color = TEXEL0 x ENV, alpha = TEXEL0 x ENV (both cycles) */
#define CC_TEX_ENV_W0 (1u << 20 | 5u << 15 | 1u << 12 | 5u << 9 | 1u << 5 | 5u)
#define CC_TEX_ENV_W1 (15u << 28 | 15u << 24 | 1u << 21 | 5u << 18 | 7u << 15 | 7u << 12 | 7u << 9 | 7u << 6 | 7u << 3 | 7u)
/* color = ENV, alpha = TEXEL0 x ENV: a silhouette of the texture in a flat colour */
#define CC_ENV_W0 (15u << 20 | 31u << 15 | 1u << 12 | 5u << 9 | 15u << 5 | 31u)
#define CC_ENV_W1 (15u << 28 | 15u << 24 | 1u << 21 | 5u << 18 | 5u << 15 | 7u << 12 | 7u << 9 | 5u << 6 | 7u << 3 | 7u)

static void env(int r, int g, int b, int a) {
    gDPPipeSync(gfx_dl_head++);
    gDPSetEnvColor(gfx_dl_head++, r, g, b, a * alpha_mul / 255);
}

static void combine_tex(void) {
    gDPPipeSync(gfx_dl_head++);
    gDPSetCombine(gfx_dl_head++, CC_TEX_ENV_W0, CC_TEX_ENV_W1);
}

static void combine_flat(void) {
    gDPPipeSync(gfx_dl_head++);
    gDPSetCombine(gfx_dl_head++, CC_ENV_W0, CC_ENV_W1);
}

static void draw_begin(void) {
    dl_saved = gfx_dl_head;
    gfx_dl_head = dl_scratch;
    saved_ox = hud_draw_origin_x, saved_oy = hud_draw_origin_y;
    saved_cl = hud_clip_left, saved_ct = hud_clip_top, saved_cr = hud_clip_right, saved_cb = hud_clip_bottom;
    hud_draw_origin_x = hud_draw_origin_y = 0;
    hud_clip_left = hud_clip_top = 0;
    hud_clip_right = 320;
    hud_clip_bottom = 240;
    nd_2d_push();
    gSPDisplayList(gfx_dl_head++, D_803A8788); /* the HUD's own set-up */
    gDPPipeSync(gfx_dl_head++);
    gDPSetTextureFilter(gfx_dl_head++, G_TF_POINT);
    gDPSetRenderMode(gfx_dl_head++, 0x504240, 0); /* blended, as the game fades its spell panel */
    combine_tex();
    env(255, 255, 255, 255);
}

static void draw_end(void) {
    nd_2d_pop();
    gfx_dl_head = dl_saved;
    hud_draw_origin_x = saved_ox, hud_draw_origin_y = saved_oy;
    hud_clip_left = saved_cl, hud_clip_top = saved_ct, hud_clip_right = saved_cr, hud_clip_bottom = saved_cb;
}

static void tlut_battle(void) {
    gDPPipeSync(gfx_dl_head++);
    gDPLoadTLUT_pal256(gfx_dl_head++, D_8039EC90);
}

static void tlut_main(void) {
    gDPPipeSync(gfx_dl_head++);
    gDPLoadTLUT_pal256(gfx_dl_head++, D_8039ADB0);
}

/* a piece of our own art at 1x */
static void own(s32 x, s32 y, s32 w, s32 h, s32 s, s32 t) {
    env(255, 255, 255, 255);
    gDPLoadTextureTile(gfx_dl_head++, art, G_IM_FMT_RGBA, G_IM_SIZ_16b, ART_W, ART_H, s, t, s + w - 1, t + h - 1, 0,
                       G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    Hud_DrawClippedTextureRect(x, y, w, h, s, t, 0x400, 0x400);
}

/* a flat rectangle (the white block, coloured by ENV) */
static void flat(s32 x, s32 y, s32 w, s32 h, int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    combine_flat();
    env(r, g, b, a);
    gDPLoadTextureTile(gfx_dl_head++, art, G_IM_FMT_RGBA, G_IM_SIZ_16b, ART_W, ART_H, 0, 0, 3, 3, 0,
                       G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 2, 2, G_TX_NOLOD, G_TX_NOLOD);
    Hud_DrawClippedTextureRect(x, y, w, h, 0, 0, 0x400, 0x400);
    combine_tex();
    env(255, 255, 255, 255);
}

static void frame_rect(s32 x, s32 y, s32 w, s32 h, int r, int g, int b, int a) {
    flat(x, y, w, 1, r, g, b, a);
    flat(x, y + h - 1, w, 1, r, g, b, a);
    flat(x, y + 1, 1, h - 2, r, g, b, a);
    flat(x + w - 1, y + 1, 1, h - 2, r, g, b, a);
}

/* the game's parchment (the main atlas's 16x16 swatch) tiled over a rectangle */
static void parchment(s32 x, s32 y, s32 w, s32 h) {
    const HudTextureDesc *tex = &D_803A86F8;
    void *img;
    u32 width;
    if (Port_InRdram(tex)) {
        img = (void *)(uptr)Port_N64U32(tex, 0);
        width = Port_N64U32(tex, 8);
    } else {
        img = tex->img;
        width = tex->width;
    }
    tlut_main();
    env(255, 255, 255, 255);
    gDPLoadTextureTile(gfx_dl_head++, img, G_IM_FMT_CI, G_IM_SIZ_8b, width, 0, PARCH_S, PARCH_T, PARCH_S + 15, PARCH_T + 15, 0,
                       G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
    Hud_DrawClippedTextureRect(x, y, w, h, PARCH_S + (x & 15), PARCH_T + (y & 15), 0x400, 0x400);
}

/* A plate from the game's 110x14 plate (plain or with the chevrons) at any size: its ends and its top and bottom
 * edges kept, the parchment between repeated (a 9-slice) */
static void plate(int chevrons, s32 x, s32 y, s32 w, s32 h) {
    const s32 ss = chevrons ? CHEVRON_S : PLAIN_S, st = chevrons ? CHEVRON_T : PLAIN_T;
    const s32 ex = chevrons ? 12 : 4, ey = 4; /* the ends kept as they are */
    const s32 mid_w = GAME_PLATE_W - 2 * ex, mid_h = PLATE_H - 2 * ey;
    tlut_battle();
    env(255, 255, 255, 255);
    Hud_LoadTextureTile(&D_803A8718, ss, st, GAME_PLATE_W, PLATE_H);
    for (int row = 0; row < 3; row++) {
        s32 dy, dh, ty;
        if (row == 0) dy = y, dh = ey, ty = st;
        else if (row == 2) dy = y + h - ey, dh = ey, ty = st + PLATE_H - ey;
        else dy = y + ey, dh = h - 2 * ey, ty = st + ey;
        for (s32 yy = 0; yy < dh; yy += (row == 1 ? mid_h : dh)) {
            s32 hh = row == 1 ? (dh - yy < mid_h ? dh - yy : mid_h) : dh;
            /* the left end, the middle repeated, the right end */
            Hud_DrawClippedTextureRect(x, dy + yy, ex, hh, ss, ty, 0x400, 0x400);
            for (s32 xx = 0; xx < w - 2 * ex; xx += mid_w) {
                s32 ww = w - 2 * ex - xx < mid_w ? w - 2 * ex - xx : mid_w;
                Hud_DrawClippedTextureRect(x + ex + xx, dy + yy, ww, hh, ss + ex, ty, 0x400, 0x400);
            }
            Hud_DrawClippedTextureRect(x + w - ex, dy + yy, ex, hh, ss + GAME_PLATE_W - ex, ty, 0x400, 0x400);
        }
    }
}

static void gem(int e, s32 x, s32 y) {
    tlut_battle();
    env(255, 255, 255, 255);
    Hud_DrawTextureTile(&D_803A8718, x, y, GEM, GEM, GEM_S, GEM_T + e * GEM, 0x400, 0x400);
}

/* the HUD's crystal of element e at 2x (or a dull grey one: not available) */
static void crystal(int e, s32 x, s32 y, int dull) {
    tlut_main();
    env(255, 255, 255, 255);
    Hud_LoadTextureTile(&D_803A86F8, CRYSTAL_S + e * CRYSTAL, CRYSTAL_T, CRYSTAL, CRYSTAL);
    Hud_DrawClippedTextureRect(x, y, CRYSTAL_SIZE, CRYSTAL_SIZE, CRYSTAL_S + e * CRYSTAL, CRYSTAL_T, 0x200, 0x200);
    if (dull) {
        combine_flat();
        env(70, 72, 68, 200);
        Hud_DrawClippedTextureRect(x, y, CRYSTAL_SIZE, CRYSTAL_SIZE, CRYSTAL_S + e * CRYSTAL, CRYSTAL_T, 0x200, 0x200);
        combine_tex();
        env(255, 255, 255, 255);
    }
}

/* the HUD's digits (the HP/MP numbers' italic outlined ones) */
static void digits(s32 x, s32 y, s32 value) {
    tlut_main();
    env(255, 255, 255, 255);
    Hud_LoadTextureTile(&D_803A86F8, 0x20, 0x1D, 0x80, 0xA);
    Hud_DrawNumber(1, x, y, value);
}

static s32 digits_width(s32 value) {
    s32 n = 1;
    while (value >= 10) value /= 10, n++;
    return n * 7 + 1;
}

static void tiny_digit(int d, s32 x, s32 y) { own(x, y, 5, 7, ART_DIGIT_S + d * 5, ART_DIGIT_T); }

/* our own palette for the spell-name font: white letters, the game's dark rim (its index 1-3 fill, 4- outline) */
static const u16 white_text[16] = { 0x0000, 0xFFFF, 0xE739, 0xA529, 0x0001, 0x0001, 0x0001, 0x0001,
                                    0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001 };

/* Hud_DrawGlyphString with a palette of our own (the game's takes one of its 4 by number) */
static void glyphs(s32 x, s32 y, const u16 *tlut, const u8 *str) {
    s32 cur = 8, adv = 8;
    if (!str) return;
    env(255, 255, 255, 255);
    gDPPipeSync(gfx_dl_head++);
    gDPLoadTLUT_pal16(gfx_dl_head++, 0, tlut);
    for (; *str != 0xFF; str++) {
        if (*str == 0x82) x += 4;
        else if (*str == 0x83) adv = 6;
        else if (*str & 0x80) x += 1;
        else {
            const s32 row = (*str & 0x70) >> 4;
            if (row != cur) Hud_LoadTextureTile(&D_803A8748, 0, row * 9, 0xA0, 9), cur = row;
            Hud_DrawClippedTextureRect(x, y, 8, 9, (*str & 0xF) * 8, row * 9, 0x400, 0x400);
            x += adv;
        }
    }
}

/* text in the HUD's spell-name font (pal: hud_spell_label_palettes' palettes) */
static void text(s32 x, s32 y, int pal, const u8 *s) {
    if (!s) return;
    env(255, 255, 255, 255);
    Hud_DrawGlyphString(x, y, pal, (u8 *)s);
}

static s32 text_width(const u8 *s) { return s ? Hud_MeasureGlyphString((u8 *)s) : 0; }

static const u8 *ascii(const char *t, u8 *buf, int cap) { return Port_HudEncode(t, buf, cap); }

/* a name split over two lines where it is wider than `w`: at the space that makes the longer line shortest */
static int split_name(const u8 *s, s32 w, u8 *l1, u8 *l2) {
    int n = 0;
    while (s[n] != 0xFF && n < 60) n++;
    if (text_width(s) <= w) return 1;
    const int lead = s[0] == 0x83; /* English: the 6-pixel advance */
    int best = -1;
    s32 best_w = 0x7FFF;
    for (int i = lead; i < n; i++) {
        int cut = s[i] == 0x82 || (!lead && i > 0 && s[i] != 0x80 && s[i] != 0x81 && i == n / 2);
        if (!cut) continue;
        int a = 0, b = 0;
        if (lead) l1[a++] = 0x83, l2[b++] = 0x83;
        for (int k = lead; k < i; k++) l1[a++] = s[k];
        for (int k = s[i] == 0x82 ? i + 1 : i; k < n; k++) l2[b++] = s[k];
        l1[a] = l2[b] = 0xFF;
        s32 m = text_width(l1) > text_width(l2) ? text_width(l1) : text_width(l2);
        if (m < best_w) best_w = m, best = i;
    }
    if (best < 0) return 1;
    int a = 0, b = 0;
    if (lead) l1[a++] = 0x83, l2[b++] = 0x83;
    for (int k = lead; k < best; k++) l1[a++] = s[k];
    for (int k = s[best] == 0x82 ? best + 1 : best; k < n; k++) l2[b++] = s[k];
    l1[a] = l2[b] = 0xFF;
    return 2;
}

/* a small FNV hash of a string: the element id of its glyphs, so a changed name never slides into the old one in
 * the pictures between ticks */
static int text_id(const u8 *s) {
    u32 h = 2166136261u;
    while (s && *s != 0xFF) h = (h ^ *s++) * 16777619u;
    return (int)((h ^ (h >> 8) ^ (h >> 16)) & 0xFF) << 8;
}

enum { OPEN_TICKS = 4, CLOSE_TICKS = 3, BAR_FADE = 5 };

static void draw_hud_keys(void) {
    /* the HUD crystals' hotkeys (1-4), at each crystal's top left */
    s32 ox = 0x1C;
    if (hud_panel_transition_state != 0) ox -= hud_panel_transition_frame * hud_panel_transition_frame * 200 / 144;
    nd_2d_element(0x3B, EL_RD2D_ANCHOR_LEFT);
    for (int e = 0; e < 4; e++) tiny_digit(e + 1, ox + e * 22 - 3, 0xC6 - 6);
}

static void draw_panel(const Spell *st, int live_now, int t_open, int fading) {
    u8 buf[48], l1[64], l2[64];
    const int n = st->len;
    const s32 lift = fading ? 0 : (OPEN_TICKS - t_open) * 2 / OPEN_TICKS; /* slides up 2 as it opens */
    u32 id = 0;
    int flags = n ? seq_flags(st->dirs, n, &id) : 7;
    /* the hint */
    nd_2d_element(0x20, EL_RD2D_ANCHOR_CENTRE);
    {
        const u8 *a = ascii("Click plate to cast", buf, 24);
        static u8 b2[24];
        const u8 *b = ascii("R-click to undo", b2, 24);
        if (a && b) {
            s32 wa = text_width(a), wb = text_width(b), total = wa + 9 + wb, x0 = 160 - total / 2;
            glyphs(x0, HINT_Y + lift, white_text, a);
            flat(x0 + wa + 3, HINT_Y + lift + 3, 3, 3, 0, 0, 0, 255);
            flat(x0 + wa + 4, HINT_Y + lift + 4, 1, 1, 255, 255, 255, 255);
            glyphs(x0 + wa + 9, HINT_Y + lift, white_text, b);
        }
    }
    /* the plate: the gems so far, the name, the MP */
    nd_2d_element(0x21, EL_RD2D_ANCHOR_CENTRE);
    {
        const s32 x = PLATE_X, y = PLATE_Y + lift;
        plate(1, x, y, PLATE_W, PLATE_H);
        if (S.hover == T_PLATE && !fading) frame_rect(x - 1, y - 1, PLATE_W + 2, PLATE_H + 2, 0xF2, 0xC4, 0x2A, 255);
        for (int k = 0; k < 3; k++) {
            if (k < n) gem(st->dirs[k], x + 13 + k * 12, y + 1);
            else own(x + 13 + k * 12, y + 1, 11, 11, ART_EMPTY_S, ART_EMPTY_T);
        }
        const u8 *name = n ? spell_name(id) : NULL;
        const u16 cost = mp_cost(n);
        const s32 cost_w = digits_width(cost) + 12, left = x + 13 + 36 + 4, right = x + PLATE_W - 13 - cost_w - 3;
        nd_2d_element(0x22 | text_id(name), EL_RD2D_ANCHOR_CENTRE);
        if (name) text(left + (right - left - text_width(name)) / 2, y + 3, hud_spell_label_palettes[flags & 7], name);
        nd_2d_element(0x2F, EL_RD2D_ANCHOR_CENTRE);
        if (n) {
            const s32 cx = x + PLATE_W - 13 - cost_w;
            digits(cx, y + 2, cost);
            if (flags & 1) flat(cx, y + 12, digits_width(cost), 1, 0xB0, 0x2A, 0x1A, 255); /* short of MP: underlined */
            const u8 *mp = ascii("MP", buf, 8);
            text(cx + digits_width(cost) + 1, y + 3, (flags & 1) ? hud_spell_label_palettes[1] : 0, mp);
        }
    }
    /* the crystals, their keys and spirit levels, and the next spell under each */
    for (int e = 0; e < 4; e++) {
        const s32 cx = CRYSTAL_X0 + CRYSTAL_PITCH * e;
        const int ok = live_now && !fading && n < 3 && element_ok(e);
        const int down = S.held_target == T_CRYSTAL && S.held_arg == e && (S.pflags & PORT_POINTER_HELD);
        nd_2d_element(0x23, EL_RD2D_ANCHOR_CENTRE);
        crystal(e, cx - 12, CRYSTAL_Y + lift + down, !ok);
        tiny_digit(e + 1, cx - 16, CRYSTAL_Y + lift - 2);
        digits(cx + 1, CRYSTAL_Y + lift + 14, player_state.stats->element_levels[e]);
        /* the plate: the spell this element makes */
        const Rect r = next_rect(e);
        nd_2d_element(0x24 + e, EL_RD2D_ANCHOR_CENTRE);
        if (!ok) {
            const int saved = alpha_mul;
            alpha_mul = alpha_mul * 90 / 255; /* greyed: the plate faint, no name */
            plate(0, r.x, r.y + lift, r.w, r.h);
            alpha_mul = saved;
            continue;
        }
        plate(0, r.x, r.y + lift, r.w, r.h);
        if (S.hover == T_NEXT && S.hover_arg == e) frame_rect(r.x - 1, r.y + lift - 1, r.w + 2, r.h + 2, 0xF2, 0xC4, 0x2A, 255);
        u8 d[3];
        u32 nid;
        memcpy(d, st->dirs, 3);
        d[n] = (u8)e;
        const int nf = seq_flags(d, n + 1, &nid);
        const u8 *name = spell_name(nid);
        nd_2d_element(0x28 + e + text_id(name), EL_RD2D_ANCHOR_CENTRE);
        const int pal = hud_spell_label_palettes[nf & 7];
        if (split_name(name, NEXT_W - 6, l1, l2) == 1) {
            text(r.x + (r.w - text_width(name)) / 2, r.y + lift + 7, pal, name);
        } else {
            text(r.x + (r.w - text_width(l1)) / 2, r.y + lift + 2, pal, l1);
            text(r.x + (r.w - text_width(l2)) / 2, r.y + lift + 11, pal, l2);
        }
    }
}

/* The plate alone while aiming (click to aim spells, port/spell_aim.h): the spell's plate where Port_SpellAimPlateRect
 * puts it (gems, name, MP: clicking it casts as always) and a hint under it */
static void draw_folded(const Spell *st) {
    u8 buf[48];
    static u8 b2[32];
    const Rect r = plate_rect();
    const int n = st->len;
    u32 id = 0;
    const int flags = n ? seq_flags(st->dirs, n, &id) : 7;
    nd_2d_element(0x3D, EL_RD2D_ANCHOR_CENTRE);
    flat(r.x + 2, r.y + 2, r.w, r.h, 0, 0, 0, 110); /* (a shadow: over the 3D) */
    plate(1, r.x, r.y, r.w, r.h);
    if (S.hover == T_PLATE) frame_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 0xF2, 0xC4, 0x2A, 255);
    for (int k = 0; k < 3; k++) {
        if (k < n) gem(st->dirs[k], r.x + 13 + k * 12, r.y + 1);
        else own(r.x + 13 + k * 12, r.y + 1, 11, 11, ART_EMPTY_S, ART_EMPTY_T);
    }
    const u8 *name = n ? spell_name(id) : NULL;
    const u16 cost = mp_cost(n);
    const s32 cost_w = digits_width(cost) + 12, left = r.x + 13 + 36 + 4, right = r.x + r.w - 13 - cost_w - 3;
    nd_2d_element(0x3E | text_id(name), EL_RD2D_ANCHOR_CENTRE);
    if (name) text(left + (right - left - text_width(name)) / 2, r.y + 3, hud_spell_label_palettes[flags & 7], name);
    nd_2d_element(0x3F, EL_RD2D_ANCHOR_CENTRE);
    if (n) {
        const s32 cx = r.x + r.w - 13 - cost_w;
        digits(cx, r.y + 2, cost);
        text(cx + digits_width(cost) + 1, r.y + 3, 0, ascii("MP", buf, 8));
    }
    /* the hint */
    const u8 *a = ascii("Click to cast", buf, sizeof buf);
    const u8 *b = ascii("R-click to undo", b2, sizeof b2);
    if (a && b) {
        const s32 wa = text_width(a), wb = text_width(b), total = wa + 9 + wb, x0 = r.x + (r.w - total) / 2, y = r.y + r.h + 2;
        glyphs(x0, y, white_text, a);
        flat(x0 + wa + 3, y + 3, 3, 3, 0, 0, 0, 255);
        flat(x0 + wa + 4, y + 4, 1, 1, 255, 255, 255, 255);
        glyphs(x0 + wa + 9, y, white_text, b);
    }
}

static void draw_pips(const Spell *s, s32 x, s32 y, int dim) {
    /* the small element pips, 5 apart, centred on x */
    s32 x0 = x - (s->len * 5) / 2;
    for (int k = 0; k < s->len; k++) {
        const int e = dim ? 4 : s->dirs[k];
        own(x0 + k * 5, y, 5, 5, ART_PIP_S + e * 6, ART_PIP_T);
    }
}

static void draw_bar(int live_now) {
    u8 buf[32];
    const Rect b = bar_rect();
    const int wide = picture_wide();
    nd_2d_element(0x30, EL_RD2D_ANCHOR_LEFT);
    flat(b.x + 3, b.y + 3, b.w, b.h, 0, 0, 0, 160); /* the panel's shadow */
    parchment(b.x + 1, b.y + 1, b.w - 2, b.h - 2);
    frame_rect(b.x, b.y, b.w, b.h, 0x1A, 0x14, 0x0A, 255);
    if (wide) {
        const u8 *label = ascii("Spells", buf, sizeof buf);
        glyphs(b.x + 4, b.y + 6, white_text, label);
        for (int p = 0; p < PAGES; p++) {
            const Rect d = dot_rect(p);
            if (p == S.page) flat(d.x + 3, d.y + 1, 3, 3, 0xE8, 0xA0, 0x80, 255);
            else flat(d.x + 3, d.y + 1, 3, 3, 0, 0, 0, 64);
            frame_rect(d.x + 2, d.y, 5, 5, 0x3A, 0x2E, 0x1C, 255);
        }
    }
    for (int i = 0; i < SLOTS; i++) {
        const Rect r = slot_rect(i);
        const Spell *s = &S.slots[S.page * SLOTS + i];
        const int hover = S.hover == T_SLOT && S.hover_arg == i;
        const int down = hover && S.held_target == T_SLOT && (S.pflags & PORT_POINTER_HELD) && !S.dragging;
        const int st = s->len ? spell_state(s) : '-';
        nd_2d_element(0x31 + i, EL_RD2D_ANCHOR_LEFT);
        if (s->len) {
            if (hover && st == 'c') flat(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0xF2, 0xE2, 0x7A, 115);
            else flat(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0xFF, 0xF8, 0xE0, 64);
            if (st != 'c' || !live_now) frame_rect(r.x, r.y, r.w, r.h, 0x7A, 0x3A, 0x2A, 255);
            else if (hover) frame_rect(r.x, r.y, r.w, r.h, 0xF2, 0xC4, 0x2A, 255);
            else frame_rect(r.x, r.y, r.w, r.h, 0x8E, 0x7A, 0x58, 255);
            const int saved = alpha_mul;
            if (st != 'c' || !live_now) alpha_mul = alpha_mul * 102 / 255;
            draw_pips(s, r.x + r.w / 2, r.y + 6 + down, 0);
            alpha_mul = saved;
        } else if (building()) {
            flat(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0xF2, 0xE2, 0x7A, hover ? 115 : 51);
            frame_rect(r.x, r.y, r.w, r.h, 0xF2, 0xC4, 0x2A, 255);
            own(r.x + 6, r.y + 6, 5, 5, ART_PLUS_S, ART_PLUS_T);
        } else {
            flat(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 0, 0, 0, 20);
            frame_rect(r.x, r.y, r.w, r.h, 0x8E, 0x7A, 0x58, 255);
        }
        own(r.x + 1, r.y + 1, 3, 5, ART_SDIGIT_S + ((i + 5) % 10) * 4, ART_SDIGIT_T);
    }
}

/* A spell's description for the name box (spec Q9: the game has none; when they are written, one line each, in the
 * HUD font, comes from here). NULL: the box has no description line. */
static const u8 *spell_description(u32 id) {
    (void)id;
    return NULL;
}

/* the name box over the bar: the kept spell's name, its gems and its MP ("save here": the spell in and its slot) */
static void draw_tip(void) {
    u8 buf[48];
    char line[48];
    const Rect b = bar_rect();
    if (S.hover != T_SLOT) return;
    const Spell *s = &S.slots[S.page * SLOTS + S.hover_arg];
    Spell in = { (u8)count_now(), { 0, 0, 0 } };
    memcpy(in.dirs, player_state.spell_input_dirs, 3);
    const Spell *show = s->len ? s : building() ? &in : NULL;
    if (!show || !show->len) return;
    u32 id;
    const int f = seq_flags(show->dirs, show->len, &id);
    const int st = spell_state(show);
    const u8 *name = spell_name(id);
    const u16 cost = mp_cost(show->len);
    const u8 *sub = NULL;
    if (!s->len) {
        snprintf(line, sizeof line, "Keep it on key %d", (S.hover_arg + 5) % 10);
        sub = ascii(line, buf, sizeof buf);
    } else if (st == 'm' && (f & 4)) {
        sub = ascii("Not learned yet", buf, sizeof buf);
    } else if (st == 'm') {
        sub = ascii("Not enough MP", buf, sizeof buf);
    } else if (st == 'x') {
        sub = ascii(battle_flags & 1 ? "Not in battle" : "Only in battle", buf, sizeof buf);
    }
    const s32 name_w = text_width(name), cost_w = digits_width(cost) + 12;
    s32 w = 6 + name_w + 8 + cost_w + 6, sub_w = sub ? text_width(sub) + 6 + 3 * 12 + 6 : 0;
    if (sub_w > w) w = sub_w;
    if (w < 100) w = 100;
    const u8 *desc = spell_description(id);
    const s32 h = 3 + 9 + 3 + 11 + 3 + (desc ? 11 : 0);
    if (desc && text_width(desc) + 12 > w) w = text_width(desc) + 12;
    const s32 x = b.x, y = TIP_BOTTOM - h;
    const int saved = alpha_mul;
    alpha_mul = alpha_mul * S.tip_alpha / 255;
    nd_2d_element(0x38, EL_RD2D_ANCHOR_LEFT);
    flat(x + 3, y + 3, w, h, 0, 0, 0, 160);
    parchment(x + 1, y + 1, w - 2, h - 2);
    frame_rect(x, y, w, h, 0x1A, 0x14, 0x0A, 255);
    nd_2d_element(0x39 | text_id(name), EL_RD2D_ANCHOR_LEFT);
    text(x + 6, y + 3, hud_spell_label_palettes[f & 7], name);
    nd_2d_element(0x3A, EL_RD2D_ANCHOR_LEFT);
    digits(x + w - 6 - cost_w, y + 2, cost);
    {
        u8 mp[8];
        text(x + w - 6 - 11, y + 3, (f & 1) ? hud_spell_label_palettes[1] : 0, ascii("MP", mp, sizeof mp));
    }
    for (int k = 0; k < show->len; k++) gem(show->dirs[k], x + 6 + k * 12, y + 15);
    if (sub) text(x + 6 + show->len * 12 + 4, y + 16, (st == 'c' || !s->len) ? 0 : hud_spell_label_palettes[1], sub);
    if (desc) text(x + 6, y + 29, 0, desc);
    alpha_mul = saved;
}

static void draw_drag(void) {
    Spell in;
    const Spell *s = NULL;
    if (S.dragging == T_SLOT) s = &S.slots[S.page * SLOTS + S.armed_arg];
    else if (S.dragging == T_PLATE && building()) {
        in.len = (u8)count_now();
        memcpy(in.dirs, player_state.spell_input_dirs, 3);
        s = &in;
    }
    if (!s || !s->len) return;
    nd_2d_element(0x3C, EL_RD2D_ANCHOR_LEFT);
    flat((s32)S.lx - 9, (s32)S.ly - 8, 18, 11, 0xFF, 0xF8, 0xE0, 140);
    frame_rect((s32)S.lx - 10, (s32)S.ly - 9, 20, 13, 0xF2, 0xC4, 0x2A, 255);
    draw_pips(s, (s32)S.lx, (s32)S.ly - 5, 0);
}

void Port_SpellUiReplaceBegin(void) {
    if (!Port_SpellUiEnabled()) return;
    nd_2d_mute(1);
    Port_MouseMute(1);
}

void Port_SpellUiReplaceEnd(void) {
    nd_2d_mute(0);
    Port_MouseMute(0);
}

void Port_SpellUiDraw(void) {
    if (!Port_SpellUiEnabled()) return;
    const int shown = ui_shown(), live_now = ui_live(), build = building() && shown;
    /* the timers (per tick) */
    if (build) {
        if (S.panel_t < OPEN_TICKS) S.panel_t++;
        S.panel_last.len = (u8)count_now();
        memcpy(S.panel_last.dirs, player_state.spell_input_dirs, 3);
        S.panel_closing = 0;
    } else if (S.panel_t) {
        S.panel_t = 0;
        S.panel_closing = shown ? CLOSE_TICKS : 0;
    } else if (S.panel_closing) {
        S.panel_closing--;
    }
    /* the bar: shown in the field; in battle faded out while the player cannot act */
    const int bar_target = shown && (live_now || !(battle_flags & 1)) ? 255 : 0;
    if (S.bar_alpha < bar_target) S.bar_alpha = S.bar_alpha + 255 / BAR_FADE > bar_target ? bar_target : S.bar_alpha + 255 / BAR_FADE;
    if (S.bar_alpha > bar_target) S.bar_alpha = S.bar_alpha - 255 / BAR_FADE < bar_target ? bar_target : S.bar_alpha - 255 / BAR_FADE;
    const int tip_on = S.hover == T_SLOT && S.hover_ticks >= 7 && !S.dragging && live_now;
    S.tip_alpha = tip_on ? (S.tip_alpha + 85 > 255 ? 255 : S.tip_alpha + 85) : 0;
    if (!ND2D_TRACK() || !shown) return;
    make_art();
    draw_begin();
    alpha_mul = 255;
    if (!(core_flags & 0xC00)) draw_hud_keys();
    if (S.bar_alpha) {
        alpha_mul = S.bar_alpha;
        draw_bar(live_now);
        if (S.tip_alpha) draw_tip();
    }
    alpha_mul = 255;
    if (build && Port_SpellAimCollapsed()) {
        /* aiming: the plate alone, at the top (port/spell_aim.h) */
        Spell now = { (u8)count_now(), { 0, 0, 0 } };
        memcpy(now.dirs, player_state.spell_input_dirs, 3);
        draw_folded(&now);
    } else if (build) {
        /* fades in over OPEN_TICKS */
        alpha_mul = 255 * (S.panel_t < OPEN_TICKS ? S.panel_t : OPEN_TICKS) / OPEN_TICKS;
        Spell now = { (u8)count_now(), { 0, 0, 0 } };
        memcpy(now.dirs, player_state.spell_input_dirs, 3);
        draw_panel(&now, live_now, S.panel_t, 0);
    } else if (S.panel_closing) {
        alpha_mul = 255 * S.panel_closing / (CLOSE_TICKS + 1);
        draw_panel(&S.panel_last, 0, OPEN_TICKS, 1);
    }
    alpha_mul = 255;
    draw_drag();
    draw_end();
}
