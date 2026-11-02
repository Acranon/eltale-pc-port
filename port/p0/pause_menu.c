/* The unified pause menu (port/include/port/pause_menu.h, docs/PAUSE_MENU.md).
 *
 * One screen in place of the Start menu's four pages: the world map of a region on the left, Status and Elements on
 * the right, Options as a dialog over it. The game's menu mode (2) runs as always; Menu_Update asks
 * Port_PauseMenuFrame, which does this screen's input and drawing in place of the pages. Three halves, all on the
 * game's thread:
 * - Port_PauseMenuRead, at each controller read: the pointer's clicks and drags (all of them taken while the menu is
 *   open), the wheel, Esc and O; Esc and the Close button become the Start the game closes (and opens) its menu with.
 * - Port_PauseMenuFrame, in Menu_Update: the controller's buttons as the game read them (input_pressed_buttons ...)
 *   and the pointer's events since the last frame; then the screen, drawn with the game's own pictures (the menu's
 *   sprites, the region maps read from the ROM as the map page reads them, the exp gauge, the HUD's font) into a
 *   display list of our own that the native renderer records (the spell UI's way: nd_2d_push/pop, no new pipelines).
 * - Port_PauseMenuCompass, in Hud_DrawCompass: the cyan dot toward the player's marker.
 * Game state it changes: the two options (menu_text_delay_option, menu_bgm_volume_option: as the Options page does,
 * with Bgm_ApplyVolumeOption at once) and menu_flags 0x20 while its Options dialog is open (the game's "page edit
 * mode": B and Start then do not close the menu). The marker, the regions seen and the view are the port's own. */
#include "common.h"
#include "game/audio/audio.h"
#include "game/audio/sound.h"
#include "game/battle/battle.h"
#include "game/camera/camera.h"
#include "game/core/main.h"
#include "game/event/cutscene.h"
#include "game/input/input.h"
#include "game/math/math.h"
#include "game/player/player.h"
#include "game/save/save_menu.h"
#include "game/text/text_window.h"
#include "game/ui/hud.h"
#include "game/ui/menu.h"
#include "game/native_draw/native_draw.h"
#include "port/data.h"
#include "port/gfx.h"
#include "port/mouse_menu.h"
#include "port/os.h"
#include "port/p0/host.h"
#include "port/pause_menu.h"
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

extern HudTextureDesc D_803A8748; /* the HUD's spell-name font (CI4 8x9 glyphs; English: the port's Latin letters) */
extern Gfx D_803A8788[];          /* the HUD's set-up list */
void Hud_LoadTextureTile(HudTextureDesc *tex, s32 x, s32 y, s32 w, s32 h);

/* ---- the game's rules and data -------------------------------------------------------------------------------- */

enum { MAP_W = 244, MAP_H = 185, REGIONS = 10, MAP_REGIONS = 6 }; /* the map pictures (menu_map_picture_dma) */
static const float zooms[3] = { 1.0f, 1.5f, 2.0f };

/* N64 buttons */
enum {
    BTN_A = 0x8000, BTN_B = 0x4000, BTN_Z = 0x2000, BTN_START = 0x1000, BTN_DU = 0x0800, BTN_DD = 0x0400,
    BTN_DL = 0x0200, BTN_DR = 0x0100, BTN_L = 0x0020, BTN_R = 0x0010, BTN_CU = 0x0008, BTN_CD = 0x0004,
    BTN_CL = 0x0002, BTN_CR = 0x0001,
};

/* the menu's sprites (menu_sprite_tex: the Start menu's own pictures) */
enum {
    SPR_PARCH = 0, SPR_HAND = 1, SPR_QUILL = 4, SPR_ARROW_L = 6, SPR_ARROW_R = 7, SPR_DIGIT = 0x0A, SPR_SLASH = 0x14,
    SPR_PERCENT = 0x15, SPR_PLUS = 0x16, SPR_MINUS = 0x17, SPR_TITLE_STATUS = 0x1A, SPR_LABEL_HP = 0x1B,
    SPR_EXP = 0x20, SPR_TITLE_OPTIONS = 0x21, SPR_LABEL_TEXT = 0x22, SPR_LABEL_SOUND = 0x23, SPR_TRACK = 0x24,
    SPR_TRACK_LINE = 0x25, SPR_LOUD = 0x29, SPR_SOFT = 0x2A, SPR_TITLE_MAP = 0x2B, SPR_PIN = 0x2C,
    SPR_TITLE_ELEMENTS = 0x2D, SPR_GEM = 0x2E, SPR_BAR = 0x32, SPR_BAR_FILL = 0x33, SPR_BLACK = 0x37,
    SPR_BROWN = 0x38, SPR_TAN = 0x39, SPR_BAND = 0x3C, SPR_RULE = 0x3D, SPR_SHADOW = 0x55, SPR_SLOW = 0x56,
    SPR_FAST = 0x57,
};
/* the element gems in the game's element order (fire, earth, water, wind): the Elements page's top, left, bottom,
 * right */
static const u8 gem_sprite[4] = { 0x2E, 0x2F, 0x31, 0x30 };
static const u8 knob_sprite[3] = { 0x28, 0x27, 0x26 }; /* the Options page's knobs (menu_options_marker_sprites) */

/* ---- layout (game pixels: 240 high; 426 wide at 16:9, 320 at 4:3; spec "Layout") ------------------------------ */

typedef struct {
    int W, wide;
    int fx, fy, fw, fh;          /* the frame */
    int map_w;                   /* the map's viewport: (6, 22) in the frame, map_w x 176 */
    int div_x, col_x, col_w;     /* the column divider and the right column (frame x) */
    int opt_x, opt_w, close_x;   /* the Options and Close buttons (frame x, y 5) */
    int stat_bar_x, stat_bar_w, exp_x;
    int elem_bar_x, elem_bar_w;
    int orb_x, orb_w;            /* the Next-spirit gauge's column (column x) */
} Layout;

enum { VP_X = 6, VP_Y = 22, VP_H = 176, MODAL_W = 240, MODAL_H = 108, MODAL_Y = 66, ORB = 32 };

static int picture_width(void) { return 2 * (int)lroundf(ND_ASPECT() * 120.0f); }

static Layout layout(void) {
    Layout L;
    memset(&L, 0, sizeof L);
    L.W = picture_width();
    L.wide = L.W >= 400;
    L.fy = 12;
    L.fh = 204;
    if (L.wide) {
        L.fx = 10 + (L.W - 426) / 2, L.fw = 402, L.map_w = 214, L.div_x = 228, L.col_x = 234, L.col_w = 162;
        L.opt_x = 327, L.opt_w = 53, L.close_x = 383;
        L.stat_bar_x = 38, L.stat_bar_w = 58, L.exp_x = 101, L.elem_bar_x = 51, L.elem_bar_w = 60;
        L.orb_x = 120, L.orb_w = 42;
    } else {
        L.fx = 8 + (L.W - 320) / 2, L.fw = 304, L.map_w = 150, L.div_x = 162, L.col_x = 168, L.col_w = 130;
        L.opt_x = 238, L.opt_w = 44, L.close_x = 285;
        L.stat_bar_x = 38, L.stat_bar_w = 38, L.exp_x = 81, L.elem_bar_x = 51, L.elem_bar_w = 36;
        L.orb_x = 90, L.orb_w = 40;
    }
    return L;
}

typedef struct { int x, y, w, h; } Rect;
static int in_rect(Rect r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static Rect vp_rect(const Layout *L) { Rect r = { L->fx + VP_X, L->fy + VP_Y, L->map_w, VP_H }; return r; }
/* the map's inside (the viewport less its 1-pixel border) */
static Rect vp_inner(const Layout *L) { Rect r = { L->fx + VP_X + 1, L->fy + VP_Y + 1, L->map_w - 2, VP_H - 2 }; return r; }
static Rect zoom_rect(const Layout *L, int in) {
    Rect v = vp_rect(L);
    Rect r = { v.x + v.w - 3 - 11, v.y + v.h - (in ? 16 : 3) - 11, 11, 11 };
    return r;
}
static Rect arrow_rect(const Layout *L, int next) {
    Rect r = { L->fx + (next ? VP_X + L->map_w - 18 : VP_X - 2), L->fy + 5, 20, 15 };
    return r;
}
static Rect opt_rect(const Layout *L) { Rect r = { L->fx + L->opt_x, L->fy + 5, L->opt_w, 13 }; return r; }
static Rect close_rect(const Layout *L) { Rect r = { L->fx + L->close_x, L->fy + 5, 13, 13 }; return r; }
static int modal_x(const Layout *L) { return (L->W - MODAL_W) / 2; }
/* the Options dialog's rows: 0 text speed, 1 sound (sliders), 2 Back */
enum { ROWS = 3, ROW_BACK = 2 };
static const int row_y[2] = { 28, 71 };       /* the hand cursor's y in the dialog (the Options page's 0x3A, 0x65) */
static Rect slider_rect(const Layout *L, int row) {
    Rect r = { modal_x(L) + 95, MODAL_Y + row_y[row] - 2, 96, 16 };
    return r;
}
static Rect back_rect(const Layout *L) { Rect r = { modal_x(L) + MODAL_W - 48, MODAL_Y + MODAL_H - 20, 40, 13 }; return r; }

/* ---- state ------------------------------------------------------------------------------------------------------ */

enum { T_NONE, T_PREV, T_NEXT, T_ZIN, T_ZOUT, T_OPT, T_CLOSE, T_MAP, T_ROW0, T_ROW1, T_BACK };

static struct {
    int host_on;      /* the host's switch (-1: none, headless) */
    u32 keys;         /* keys since the last read */
    /* the player's marker and the regions seen (kept per save) */
    int mark_set, mark_r;
    float mark_x, mark_y; /* map pixels in its region's picture */
    u32 visited;
    u64 save_key;
    int forgot, seed_pending;
    /* the screen */
    int shown;        /* the menu's frame ran since the last read in play */
    int region, zoom;
    float cx, cy;     /* the map pixel at the viewport's middle */
    int open_t, fade_t, fade_from, drop_t;
    int drag;
    float drag_px, drag_py, drag_cx, drag_cy;
    int modal, modal_t, modal_row, modal_release, knob_drag;
    int rep[4];       /* the D-pad's hold, for its repeat */
    int stick_latch;  /* the dialog's stick, one step a push */
    int hover, pressed_t; /* the target under the pointer, and the one held down */
    /* the pointer since the last frame */
    float px, py, last_px, last_py;
    int pflags, ev;
    int wheel, esc, opt_key, pending_start, pending_b;
    /* the compass dot */
    int dot_alpha, dot_logged;
} S = { .host_on = -1, .mark_r = -1, .region = -1 };

static int log_on = -1;
static int log_level(void) {
    if (log_on < 0) log_on = getenv("ELTALE_PAUSE_LOG") ? atoi(getenv("ELTALE_PAUSE_LOG")) : 0;
    return log_on;
}
static unsigned long long vi(void) { return (unsigned long long)Port_OsViCount(); }

int Port_PauseMenuEnabled(void) {
    static int env = -2;
    if (S.host_on >= 0) return S.host_on;
    if (env == -2) env = getenv("ELTALE_PAUSE_MENU") ? atoi(getenv("ELTALE_PAUSE_MENU")) != 0 : 0;
    return env;
}

void Port_PauseMenuKeys(u32 keys, int on) {
    S.keys |= keys;
    if (on >= 0) S.host_on = on;
}

static int region_has_map(int r) { return r >= 0 && r < MAP_REGIONS; }
/* the regions the menu offers: the one the player is in, and the ones with a map the player has been to */
static int region_listed(int r) { return r == menu_map_region || (region_has_map(r) && (S.visited >> r & 1)); }
static int region_step(int from, int dir) {
    for (int k = 1; k <= REGIONS; k++) {
        int r = ((from + dir * k) % REGIONS + REGIONS) % REGIONS;
        if (region_listed(r)) return r;
    }
    return from;
}

/* the game would open its menu now (Core_RunGameplayLoop's test, less the Start press itself) */
static int game_can_open(void) {
    return core_game_mode == 1 && !(core_flags & 0x1087) && !(player_state.actor.animFlags & 1) && !(battle_flags & 1) &&
           D_8007A172 != 4;
}

/* the player's place on a region's map picture (Menu_UpdateMapPage's: the field's position by the region's scale and
 * origin, a spot's fixed point), or 0 when the player is not in that region */
static int player_on_map(int r, float *x, float *y) {
    if (r != menu_map_region || !region_has_map(r)) return 0;
    if (menu_map_spot < 0) {
        *x = (float)((s32)player_state.actor.x / menu_map_world_scale[r] + menu_map_origin[r]);
        *y = (float)((s32)player_state.actor.z / menu_map_world_scale[r + 6] + menu_map_origin[r + 6]);
    } else {
        *x = menu_map_spot_coords[r * 18 + menu_map_spot * 2];
        *y = menu_map_spot_coords[r * 18 + menu_map_spot * 2 + 1];
    }
    return 1;
}

/* a map point's place in the world (the inverse: Menu_UpdateMapPage's menu_map_marker_x / _z) */
static void map_to_world(int r, float mx, float my, float *wx, float *wz) {
    *wx = menu_map_world_scale[r] * (mx - menu_map_origin[r]);
    *wz = menu_map_world_scale[r + 6] * (my - menu_map_origin[r + 6]);
}

/* ---- the marker's file (per save, as the spell UI's quick bar: the save's record hashed) ------------------------ */

static void data_path(char *out, size_t size, const char *leaf, int make) {
    const char *dir = Port_WindowDataDir();
    const char *env = getenv("ELTALE_DATA_DIR");
    if (!strcmp(dir, ".") && env && *env) dir = env;
    snprintf(out, size, "%s/mapmarker", dir);
    if (make) make_dir(out);
    snprintf(out, size, "%s/mapmarker/%s", dir, leaf);
}

static void mark_write(void) {
    char path[1024], tmp[1100], leaf[64];
    if (!S.save_key) return;
    snprintf(leaf, sizeof leaf, "save_%016llx.mark", (unsigned long long)S.save_key);
    data_path(path, sizeof path, leaf, 1);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "ELTMAPMARK 1\nvisited %x\n", (unsigned)S.visited);
    if (S.mark_set) fprintf(f, "marker %d %.2f %.2f\n", S.mark_r, S.mark_x, S.mark_y);
    int ok = fclose(f) == 0;
    if (!ok) { remove(tmp); return; }
    remove(path);
    rename(tmp, path);
}

static void mark_clear(void) {
    S.mark_set = 0;
    S.mark_r = -1;
    S.visited = 0;
}

static int mark_read(void) {
    char path[1024], leaf[64], line[128];
    mark_clear();
    snprintf(leaf, sizeof leaf, "save_%016llx.mark", (unsigned long long)S.save_key);
    data_path(path, sizeof path, leaf, 0);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fgets(line, sizeof line, f) && !strncmp(line, "ELTMAPMARK 1", 12);
    while (ok && fgets(line, sizeof line, f)) {
        unsigned v;
        int r;
        float x, y;
        if (sscanf(line, "visited %x", &v) == 1) S.visited = v & 0x3FF;
        if (sscanf(line, "marker %d %f %f", &r, &x, &y) == 3 && region_has_map(r) && x >= 0 && x <= MAP_W && y >= 0 &&
            y <= MAP_H)
            S.mark_set = 1, S.mark_r = r, S.mark_x = x, S.mark_y = y;
    }
    fclose(f);
    return ok ? 1 : -1;
}

void Port_PauseMenuSaveEvent(int kind, s32 slot) {
    if (slot < 0 || slot >= 16 || !Port_PauseMenuEnabled()) return; /* (off: no files, as the original) */
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
    if (kind == 0) {
        mark_write();
    } else {
        result = mark_read();
        /* a save from before the menu: the regions up to the one it was saved in count as seen (the story goes
         * through them in order) */
        S.seed_pending = result <= 0;
    }
    if (log_level())
        printf("P1_PAUSE_SAVE vi=%llu %s slot=%d key=%016llx result=%d marker=%d,%d,%.1f,%.1f visited=%x\n", vi(),
               kind ? "read" : "write", (int)slot, (unsigned long long)h, result, S.mark_set, S.mark_r, S.mark_x, S.mark_y,
               (unsigned)S.visited);
}

static void log_mark(const char *what) {
    if (!log_level()) return;
    float wx = 0, wz = 0;
    if (S.mark_set) map_to_world(S.mark_r, S.mark_x, S.mark_y, &wx, &wz);
    printf("P1_PAUSE_MARK vi=%llu %s set=%d region=%d map=%.1f,%.1f world=%.0f,%.0f\n", vi(), what, S.mark_set, S.mark_r,
           S.mark_x, S.mark_y, wx, wz);
}

/* ---- each read ---------------------------------------------------------------------------------------------- */

int Port_PauseMenuWheel(int wheel) {
    if (!wheel || !Port_PauseMenuEnabled() || core_game_mode != 2 || !S.shown) return wheel;
    S.wheel += wheel;
    return 0;
}

static void press(u16 *buttons, u16 b, const char *why) {
    *buttons |= b;
    if (log_level()) printf("P1_PAUSE_PRESS vi=%llu buttons=%04x why=%s\n", vi(), b, why);
}

void Port_PauseMenuRead(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    (void)stick_x;
    (void)stick_y;
    const u32 keys = S.keys;
    S.keys = 0;
    if (!Port_PauseMenuEnabled()) {
        S.shown = 0;
        return;
    }
    /* the title or the intro: the next game starts with no marker */
    if (core_game_mode == 3 || core_game_mode == 4) {
        if (!S.forgot) {
            mark_clear();
            S.save_key = 0;
            S.seed_pending = 0;
            S.forgot = 1;
        }
        S.shown = 0;
        return;
    }
    if (core_game_mode == 1) {
        if (S.shown && log_level()) /* closed: the options as the game now has them (the music's volume as set) */
            printf("P1_PAUSE_CLOSED vi=%llu text_delay=%d bgm=%d seq_volume=%d marker=%d,%d,%.1f,%.1f\n", vi(),
                   menu_text_delay_option, menu_bgm_volume_option, (int)audio_seq_volume_limit, S.mark_set, S.mark_r,
                   S.mark_x, S.mark_y);
        S.shown = 0;
        S.wheel = 0;
        /* the regions the player has been in */
        if (menu_map_region >= 0 && menu_map_region < REGIONS && player_state.stats) {
            u32 v = S.visited | 1u << menu_map_region;
            if (S.seed_pending && region_has_map(menu_map_region)) v |= (1u << (menu_map_region + 1)) - 1;
            S.seed_pending = 0;
            if (v != S.visited) {
                S.visited = v;
                mark_write();
            }
        }
        /* Esc opens the menu where the game would (Start: its own way in) */
        if ((keys & PORT_SPELLKEY_CLEAR) && game_can_open()) press(buttons, BTN_START, "esc-open");
        return;
    }
    if (core_game_mode != 2 || !S.shown) return;
    /* the pointer: every click, drag and wheel over the open menu is the menu's */
    float x, y;
    int f;
    Port_MousePointerGet(&x, &y, &f);
    S.pflags = f;
    if (f & PORT_POINTER_IN) {
        S.px = x; /* (picture units from the left: the screen is laid out over the whole picture) */
        S.py = y;
    }
    S.ev |= f & (PORT_POINTER_LEFT | PORT_POINTER_RIGHT | PORT_POINTER_UP);
    Port_MousePointerTake(PORT_POINTER_LEFT | PORT_POINTER_RIGHT | PORT_POINTER_UP);
    if (keys & PORT_PAUSEKEY_OPTIONS) S.opt_key = 1;
    if (keys & PORT_SPELLKEY_CLEAR) {
        if (S.modal) S.esc = 1;
        else S.pending_start = 1;
    }
    if (S.pending_start) {
        S.pending_start = 0;
        if (!S.modal) press(buttons, BTN_START, "close");
    } else if (S.pending_b) {
        S.pending_b = 0;
        if (!S.modal) press(buttons, BTN_B, "back");
    }
}

/* ---- the region maps (read from the ROM as the map page reads them, into the port's own copies) ------------------ */

static u8 map_tex[REGIONS][MAP_W * MAP_H];
static u16 map_tlut[REGIONS][256];
static int map_w[REGIONS], map_h[REGIONS];
static int map_lang[REGIONS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

static int map_load(int r) {
    if (r < 0 || r >= REGIONS) return 0;
    Port_ImagesUpdate(); /* (the English pictures are swapped into the loaded ROM: the language's first) */
    const int en = Port_TextEnglish();
    if (map_lang[r] == en) return 1;
    const MenuPictureDma *d = &menu_map_picture_dma[r];
    const u8 *rom = Port_HostRomWritable();
    const u32 start = (u32)(uptr)d->start, end = (u32)(uptr)d->end;
    if (!rom || end <= start + 0x200) return 0;
    const u8 *p = rom + start;
    for (int i = 0; i < 256; i++) map_tlut[r][i] = (u16)(p[2 * i] << 8 | p[2 * i + 1]);
    if (r < MAP_REGIONS) {
        map_w[r] = MAP_W, map_h[r] = MAP_H;
    } else {
        const int n = r - MAP_REGIONS;
        map_w[r] = menu_map_small_sizes[n][0];
        map_h[r] = menu_map_small_sizes[n][1];
    }
    u32 bytes = (u32)(map_w[r] * map_h[r]);
    if (bytes > end - start - 0x200) bytes = end - start - 0x200;
    memcpy(map_tex[r], p + 0x200, bytes);
    nd_2d_forget((uintptr_t)map_tex[r], sizeof map_tex[r]);
    nd_2d_forget((uintptr_t)map_tlut[r], sizeof map_tlut[r]);
    map_lang[r] = en;
    return 1;
}

/* ---- the view ----------------------------------------------------------------------------------------------------- */

static void clamp_view(const Layout *L) {
    if (!region_has_map(S.region)) {
        S.cx = MAP_W * 0.5f, S.cy = MAP_H * 0.5f;
        return;
    }
    const Rect in = vp_inner(L);
    const float z = zooms[S.zoom], hw = in.w / (2.0f * z), hh = in.h / (2.0f * z);
    if (MAP_W <= 2.0f * hw) S.cx = MAP_W * 0.5f;
    else S.cx = S.cx < hw ? hw : S.cx > MAP_W - hw ? MAP_W - hw : S.cx;
    if (MAP_H <= 2.0f * hh) S.cy = MAP_H * 0.5f;
    else S.cy = S.cy < hh ? hh : S.cy > MAP_H - hh ? MAP_H - hh : S.cy;
}

/* the map picture's top left on the screen (whole pixels) */
static void map_origin(const Layout *L, float *ox, float *oy) {
    const Rect in = vp_inner(L);
    const float z = zooms[S.zoom];
    *ox = floorf(in.x + in.w * 0.5f - S.cx * z + 0.5f);
    *oy = floorf(in.y + in.h * 0.5f - S.cy * z + 0.5f);
}

static void log_view(const char *why) {
    if (log_level())
        printf("P1_PAUSE_VIEW vi=%llu %s region=%d zoom=%d centre=%.1f,%.1f modal=%d row=%d visited=%x\n", vi(), why, S.region,
               S.zoom, S.cx, S.cy, S.modal, S.modal_row, (unsigned)S.visited);
}

static void set_region(const Layout *L, int r) {
    if (r == S.region) return;
    S.fade_from = S.region;
    S.fade_t = 3;
    S.region = r;
    S.cx = MAP_W * 0.5f, S.cy = MAP_H * 0.5f;
    S.drag = 0;
    clamp_view(L);
    Sfx_Play(0, 0, 0xFF);
    log_view("region");
}

static void set_zoom(const Layout *L, int z) {
    z = z < 0 ? 0 : z > 2 ? 2 : z;
    if (z == S.zoom || !region_has_map(S.region)) return;
    S.zoom = z;
    clamp_view(L);
    log_view("zoom");
}

static void pan(const Layout *L, float dx, float dy) { /* screen pixels */
    if (!region_has_map(S.region)) return;
    S.cx += dx / zooms[S.zoom];
    S.cy += dy / zooms[S.zoom];
    clamp_view(L);
}

/* place the marker at a map point, or take it away when the point is on it (the quill's box) */
static void marker_toggle(const Layout *L, float mx, float my, const char *why) {
    (void)L;
    if (!region_has_map(S.region)) return;
    if (mx < 6.0f || mx > 236.0f || my < 6.0f || my > 178.0f) return; /* off the map picture (the game's own bounds) */
    if (S.mark_set && S.mark_r == S.region) {
        const float z = zooms[S.zoom], dx = (mx - S.mark_x) * z, dy = (my - S.mark_y) * z;
        if (dx >= -3.0f && dx <= 10.0f && dy >= -13.0f && dy <= 2.0f) {
            S.mark_set = 0;
            S.mark_r = -1;
            Sfx_Play(0, 0x13, 0xFF);
            mark_write();
            log_mark(why);
            return;
        }
    }
    S.mark_set = 1;
    S.mark_r = S.region;
    S.mark_x = floorf(mx * 2.0f + 0.5f) * 0.5f; /* (half pixels: a 2x zoom's) */
    S.mark_y = floorf(my * 2.0f + 0.5f) * 0.5f;
    S.drop_t = 0;
    S.dot_alpha = 0;
    Sfx_Play(0, 0x17, 0xFF);
    mark_write();
    log_mark(why);
}

/* ---- the Options dialog -------------------------------------------------------------------------------------- */

/* a row's place 0..4 on its slider: the text speed is kept reversed (the Options page: left slow = 4) */
static int option_get(int row) { return row == 0 ? 4 - menu_text_delay_option : menu_bgm_volume_option; }
static void option_set(int row, int p) {
    p = p < 0 ? 0 : p > 4 ? 4 : p;
    if (p == option_get(row)) return;
    if (row == 0) menu_text_delay_option = (s8)(4 - p);
    else {
        menu_bgm_volume_option = (s8)p;
        Bgm_ApplyVolumeOption(); /* (the game applies it when the menu closes; here at once) */
    }
    Sfx_Play(0, 0, 0xFF);
    if (log_level())
        printf("P1_PAUSE_OPTION vi=%llu text_delay=%d bgm=%d\n", vi(), menu_text_delay_option, menu_bgm_volume_option);
}
/* the knob's x for place p on a row (the Options page's places, as dialog x) */
static int knob_x(int row, int p) {
    return (row == 0 ? menu_options_top_row_pos[p][0] : menu_options_bgm_row_pos[p][0]) - 0x28;
}
static int knob_at(const Layout *L, int row, float x) {
    int best = 0;
    float bd = 1e9f;
    for (int p = 0; p < 5; p++) {
        float d = fabsf(x - (modal_x(L) + knob_x(row, p) + 4));
        if (d < bd) bd = d, best = p;
    }
    return best;
}

static void modal_open(void) {
    S.modal = 1;
    S.modal_t = 0;
    S.modal_row = 0;
    S.knob_drag = -1;
    S.stick_latch = 1;
    menu_flags |= 0x20; /* the game's page edit mode: B and Start do not close the menu meanwhile */
    Sfx_Play(0, 1, 0xFF);
    log_view("options");
}

static void modal_close(void) {
    S.modal = 0;
    S.modal_release = 1; /* menu_flags 0x20 goes at the next frame: this frame's B or Start does not close the menu */
    S.knob_drag = -1;
    Sfx_Play(0, 2, 0xFF);
    log_view("options-closed");
}

/* ---- each frame (Menu_Update) -------------------------------------------------------------------------------- */

static int hit(const Layout *L, float x, float y) {
    if (S.modal) {
        if (in_rect(back_rect(L), x, y)) return T_BACK;
        if (in_rect(slider_rect(L, 0), x, y)) return T_ROW0;
        if (in_rect(slider_rect(L, 1), x, y)) return T_ROW1;
        return T_NONE;
    }
    if (in_rect(zoom_rect(L, 1), x, y)) return T_ZIN;
    if (in_rect(zoom_rect(L, 0), x, y)) return T_ZOUT;
    if (in_rect(arrow_rect(L, 0), x, y)) return T_PREV;
    if (in_rect(arrow_rect(L, 1), x, y)) return T_NEXT;
    if (in_rect(opt_rect(L), x, y)) return T_OPT;
    if (in_rect(close_rect(L), x, y)) return T_CLOSE;
    if (in_rect(vp_inner(L), x, y)) return T_MAP;
    return T_NONE;
}

static void on_open(const Layout *L) {
    S.shown = 1;
    S.open_t = 0;
    S.fade_t = 0;
    S.modal = S.modal_release = 0;
    S.drag = 0;
    S.ev = S.wheel = S.esc = S.opt_key = S.pending_start = S.pending_b = 0;
    S.hover = T_NONE;
    S.pressed_t = T_NONE;
    S.knob_drag = -1;
    memset(S.rep, 0, sizeof S.rep);
    S.region = menu_map_region >= 0 && menu_map_region < REGIONS ? menu_map_region : 0;
    S.cx = MAP_W * 0.5f, S.cy = MAP_H * 0.5f;
    float x, y;
    if (player_on_map(S.region, &x, &y)) S.cx = x, S.cy = y; /* opens on the player */
    clamp_view(L);
    if (log_level())
        printf("P1_PAUSE_OPEN vi=%llu width=%d region=%d spot=%d zoom=%d centre=%.1f,%.1f visited=%x marker=%d,%d,%.1f,%.1f\n",
               vi(), L->W, S.region, menu_map_spot, S.zoom, S.cx, S.cy, (unsigned)S.visited, S.mark_set, S.mark_r, S.mark_x,
               S.mark_y);
}

static void frame_input(const Layout *L) {
    const u16 pressed = input_pressed_buttons, held = input_held_buttons;
    const int ev = S.ev, wheel = S.wheel;
    const int in = (S.pflags & PORT_POINTER_IN) != 0;
    S.ev = S.wheel = 0;
    const int over = in ? hit(L, S.px, S.py) : T_NONE;
    const int moved = in && (S.px != S.last_px || S.py != S.last_py);
    S.last_px = in ? S.px : -1.0f;
    S.last_py = in ? S.py : -1.0f;
    S.hover = (S.pflags & PORT_POINTER_SHOWN) ? over : T_NONE;
    if (!(S.pflags & PORT_POINTER_HELD)) S.pressed_t = T_NONE;

    if (S.modal) {
        const int esc = S.esc || S.opt_key;
        S.esc = S.opt_key = 0;
        if (S.modal_t < 4) S.modal_t++;
        /* the pointer */
        if (in && (ev & PORT_POINTER_LEFT)) {
            S.pressed_t = over;
            if (over == T_ROW0 || over == T_ROW1) {
                const int row = over - T_ROW0;
                S.modal_row = row;
                S.knob_drag = row;
                option_set(row, knob_at(L, row, S.px));
            }
        }
        if (S.knob_drag >= 0) {
            if (S.pflags & PORT_POINTER_HELD) option_set(S.knob_drag, knob_at(L, S.knob_drag, S.px));
            else S.knob_drag = -1;
        }
        if (in && (ev & PORT_POINTER_LEFT) && over == T_BACK) {
            modal_close();
            return;
        }
        if (in && (ev & PORT_POINTER_RIGHT)) {
            modal_close();
            return;
        }
        /* pointing at a row (the pointer moved there) moves the hand to it */
        if (moved && (over == T_ROW0 || over == T_ROW1)) S.modal_row = over - T_ROW0;
        else if (moved && over == T_BACK) S.modal_row = ROW_BACK;
        /* the controller */
        if (esc || (pressed & (BTN_B | BTN_START)) || ((pressed & BTN_A) && S.modal_row == ROW_BACK)) {
            modal_close();
            return;
        }
        int dy = 0, dx = 0;
        if (pressed & (BTN_DU | BTN_CU)) dy = -1;
        if (pressed & (BTN_DD | BTN_CD)) dy = 1;
        if (pressed & (BTN_DL | BTN_CL)) dx = -1;
        if (pressed & (BTN_DR | BTN_CR)) dx = 1;
        const int sx = input_stick_x, sy = input_stick_y;
        if (sx > -20 && sx < 20 && sy > -30 && sy < 30) S.stick_latch = 0;
        else if (!S.stick_latch) {
            S.stick_latch = 1;
            if (sy >= 30) dy = -1;
            else if (sy <= -30) dy = 1;
            else if (sx >= 20) dx = 1;
            else if (sx <= -20) dx = -1;
        }
        if (dy) {
            int r = S.modal_row + dy;
            r = r < 0 ? 0 : r >= ROWS ? ROWS - 1 : r;
            if (r != S.modal_row) S.modal_row = r, Sfx_Play(0, 0, 0xFF);
        }
        if (dx && S.modal_row < ROW_BACK) option_set(S.modal_row, option_get(S.modal_row) + dx);
        return;
    }

    /* the main screen: the pointer */
    if (in && (ev & PORT_POINTER_LEFT)) {
        S.pressed_t = over;
        switch (over) {
        case T_PREV: set_region(L, region_step(S.region, -1)); break;
        case T_NEXT: set_region(L, region_step(S.region, 1)); break;
        case T_ZIN: set_zoom(L, S.zoom + 1); break;
        case T_ZOUT: set_zoom(L, S.zoom - 1); break;
        case T_OPT: modal_open(); return;
        case T_CLOSE: S.pending_start = 1; break; /* the game's Start, at the next read */
        case T_MAP:
            S.drag = 1;
            S.drag_px = S.px, S.drag_py = S.py, S.drag_cx = S.cx, S.drag_cy = S.cy;
            break;
        default: break;
        }
    }
    if (S.drag) {
        if (S.pflags & PORT_POINTER_HELD) {
            const float z = zooms[S.zoom];
            S.cx = S.drag_cx - (S.px - S.drag_px) / z;
            S.cy = S.drag_cy - (S.py - S.drag_py) / z;
            clamp_view(L);
        } else {
            S.drag = 0;
            log_view("drag");
        }
    }
    if (in && (ev & PORT_POINTER_RIGHT)) {
        if (over == T_MAP) {
            float ox, oy;
            map_origin(L, &ox, &oy);
            const float z = zooms[S.zoom];
            marker_toggle(L, (S.px - ox) / z, (S.py - oy) / z, "pointer");
        } else if (over == T_NONE) {
            S.pending_b = 1; /* a right click off the map backs out (B), as on the game's own screens */
        }
    }
    if (wheel) set_zoom(L, S.zoom + (wheel > 0 ? 1 : -1));
    if (S.opt_key) {
        S.opt_key = 0;
        modal_open();
        return;
    }
    S.esc = 0;

    /* the controller */
    if (pressed & (BTN_Z | BTN_L | BTN_CL)) set_region(L, region_step(S.region, -1));
    else if (pressed & (BTN_R | BTN_CR)) set_region(L, region_step(S.region, 1));
    if (pressed & BTN_CU) set_zoom(L, S.zoom + 1);
    if (pressed & BTN_CD) set_zoom(L, S.zoom - 1);
    static const u16 dirs[4] = { BTN_DL, BTN_DR, BTN_DU, BTN_DD };
    static const s8 ddx[4] = { -1, 1, 0, 0 }, ddy[4] = { 0, 0, -1, 1 };
    for (int k = 0; k < 4; k++) {
        if (!(held & dirs[k])) {
            S.rep[k] = 0;
            continue;
        }
        /* 8 pixels a press, repeating after 8 ticks held */
        if ((pressed & dirs[k]) || (S.rep[k] >= 8 && !(S.rep[k] & 1))) pan(L, ddx[k] * 8.0f, ddy[k] * 8.0f);
        S.rep[k]++;
    }
    const int sx = input_stick_x, sy = input_stick_y;
    if (sx > 8 || sx < -8 || sy > 8 || sy < -8) {
        const float fx = sx > 8 ? sx - 8 : sx < -8 ? sx + 8 : 0, fy = sy > 8 ? sy - 8 : sy < -8 ? sy + 8 : 0;
        pan(L, fx / 72.0f * 6.0f, -fy / 72.0f * 6.0f);
    }
    if (pressed & BTN_A) marker_toggle(L, S.cx, S.cy, "reticle");
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */

/* our own pieces (RGBA5551, host order): a white block (flat colours), the close cross, a gear, and the compass's
 * diamond in cyan (made from the game's own red one: compass_art) */
enum { ART_W = 40, ART_H = 8 };
enum { ART_WHITE_S = 0, ART_CROSS_S = 10, ART_GEAR_S = 18, ART_DOT_S = 26 };
static u16 art[ART_W * ART_H];
static int art_made;

static u16 rgba5551(int r, int g, int b, int a) {
    return (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >= 128));
}

static void make_art(void) {
    if (art_made) return;
    art_made = 1;
    memset(art, 0, sizeof art);
    const u16 white = rgba5551(255, 255, 255, 255);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) art[y * ART_W + ART_WHITE_S + x] = white;
    /* the close cross, 7x7 */
    for (int i = 0; i < 7; i++) {
        art[i * ART_W + ART_CROSS_S + i] = rgba5551(0x6B, 0x2A, 0x1A, 255);
        art[i * ART_W + ART_CROSS_S + 6 - i] = rgba5551(0x6B, 0x2A, 0x1A, 255);
        if (i < 6) art[i * ART_W + ART_CROSS_S + i + 1] = rgba5551(0x6B, 0x2A, 0x1A, 255);
        if (i < 6) art[i * ART_W + ART_CROSS_S + 5 - i] = rgba5551(0x6B, 0x2A, 0x1A, 255);
    }
    /* a gear, 7x7 */
    static const char *gear[7] = { "..#.#..", ".#####.", "##...##", ".#...#.", "##...##", ".#####.", "..#.#.." };
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++)
            if (gear[y][x] == '#') art[y * ART_W + ART_GEAR_S + x] = rgba5551(0x3E, 0x3A, 0x32, 255);
}

/* The compass's marker dot is the HUD atlas's 8x8 red diamond (s 0, t 0x32: the dot Hud_DrawCompass draws for the
 * map page's marker); the player's is the same diamond in cyan (#3AC8F0, spec 4.3): each texel's brightness against
 * the brightest red one, the black rim and the white glint kept. Made once the common block is in RDRAM. */
static int compass_art_made;
static void compass_art(void) {
    extern HudTextureDesc D_803A86F8;
    extern u8 D_8039ADB0[];
    if (compass_art_made) return;
    const HudTextureDesc *t = &D_803A86F8;
    const u8 *img = Port_InRdram(t) ? (const u8 *)(uptr)Port_N64U32(t, 0) : (const u8 *)t->img;
    const int width = Port_InRdram(t) ? (int)Port_N64U32(t, 8) : (int)t->width;
    if (!img || width <= 8) return;
    compass_art_made = 1;
    int rgb[64][4], top = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const u8 i = img[(0x32 + y) * width + x];
            const u16 c = (u16)(D_8039ADB0[2 * i] << 8 | D_8039ADB0[2 * i + 1]);
            int *o = rgb[y * 8 + x];
            o[0] = (c >> 11 & 31) * 255 / 31, o[1] = (c >> 6 & 31) * 255 / 31, o[2] = (c >> 1 & 31) * 255 / 31, o[3] = c & 1;
            if (o[3] && o[0] > top && !(o[1] > 200 && o[2] > 200)) top = o[0];
        }
    for (int k = 0; k < 64; k++) {
        const int *o = rgb[k];
        u16 v = 0;
        if (o[3]) {
            if (o[0] > 200 && o[1] > 200 && o[2] > 200) v = rgba5551(o[0], o[1], o[2], 255); /* the glint */
            else {
                const int l = o[0] * 256 / top;
                const int r = 0x3A * l / 256, g = 0xC8 * l / 256, b = 0xF0 * l / 256;
                v = rgba5551(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255);
            }
        }
        art[(k / 8) * ART_W + ART_DOT_S + k % 8] = v;
    }
    nd_2d_forget((uintptr_t)art, sizeof art);
}

static Gfx dl_scratch[8192];
static Gfx *dl_saved;
static int alpha_mul = 255; /* the fades */
static float clip_x0, clip_y0, clip_x1 = 1023, clip_y1 = 240;

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
static void filter(int bilerp) {
    gDPPipeSync(gfx_dl_head++);
    gDPSetTextureFilter(gfx_dl_head++, bilerp ? G_TF_BILERP : G_TF_POINT);
}

static void draw_begin(void) {
    dl_saved = gfx_dl_head;
    gfx_dl_head = dl_scratch;
    nd_2d_push();
    gSPDisplayList(gfx_dl_head++, D_803A8788); /* the HUD's own set-up */
    gDPPipeSync(gfx_dl_head++);
    gDPSetTextureLUT(gfx_dl_head++, G_TT_RGBA16);
    filter(0);
    gDPSetRenderMode(gfx_dl_head++, 0x504240, 0); /* blended, as the menu's sprites */
    combine_tex();
    env(255, 255, 255, 255);
    clip_x0 = clip_y0 = 0, clip_x1 = 1023, clip_y1 = 240;
}

static void draw_end(void) {
    nd_2d_pop();
    gfx_dl_head = dl_saved;
}

static void clip(Rect r) { clip_x0 = r.x, clip_y0 = r.y, clip_x1 = r.x + r.w, clip_y1 = r.y + r.h; }
static void unclip(void) { clip_x0 = clip_y0 = 0, clip_x1 = 1023, clip_y1 = 240; }

/* a rectangle (screen pixels, fractions allowed) showing the loaded tile from texel (s, t), sx / sy texels a pixel */
static void rect(float x, float y, float w, float h, float s, float t, float sx, float sy) {
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < clip_x0) s += (clip_x0 - x0) * sx, x0 = clip_x0;
    if (y0 < clip_y0) t += (clip_y0 - y0) * sy, y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    gSPTextureRectangle(gfx_dl_head++, (s32)lroundf(x0 * 4.0f), (s32)lroundf(y0 * 4.0f), (s32)lroundf(x1 * 4.0f),
                        (s32)lroundf(y1 * 4.0f), G_TX_RENDERTILE, (s32)lroundf(s * 32.0f), (s32)lroundf(t * 32.0f),
                        (s32)lroundf(sx * 1024.0f), (s32)lroundf(sy * 1024.0f));
}

static void tlut(const void *p) {
    gDPPipeSync(gfx_dl_head++);
    gDPLoadTLUT_pal256(gfx_dl_head++, p);
}

static void load_ci8(const void *img, int width, int s0, int t0, int s1, int t1, int masks, int maskt, int mirror) {
    gDPLoadTextureTile(gfx_dl_head++, img, G_IM_FMT_CI, G_IM_SIZ_8b, width, 0, s0, t0, s1, t1, 0,
                       G_TX_WRAP | (mirror ? G_TX_MIRROR : G_TX_NOMIRROR), G_TX_NOMIRROR | G_TX_WRAP, masks, maskt,
                       G_TX_NOLOD, G_TX_NOLOD);
}

/* the menu's sprite `id`: loaded with its palette */
static int spr_w(int id) { return menu_sprite_sizes[id][0]; }
static int spr_h(int id) { return menu_sprite_sizes[id][1]; }
static void spr_load(int id, int masks, int maskt) {
    tlut(menu_sprite_tlut);
    load_ci8(menu_sprite_tex[id], spr_w(id), 0, 0, spr_w(id) - 1, spr_h(id) - 1, masks, maskt, 0);
}
/* at its own size */
static void spr(int id, float x, float y) {
    spr_load(id, 0, 0);
    rect(x, y, (float)spr_w(id), (float)spr_h(id), 0, 0, 1, 1);
}
/* stretched to w x h */
static void spr_scaled(int id, float x, float y, float w, float h) {
    spr_load(id, 0, 0);
    rect(x, y, w, h, 0, 0, spr_w(id) / w, spr_h(id) / h);
}
/* one of the menu's colour swatches (0x37 black, 0x38 brown, 0x39 tan, 0x3C the band) over a rectangle */
static void swatch(int id, float x, float y, float w, float h) {
    spr_load(id, 0, 0);
    rect(x, y, w, h, 0, 0, 0, 0);
}
/* the menu's parchment (sprite 0, 16x16, repeated), lined up with the screen as the menu draws it */
static void parchment(float x, float y, float w, float h) {
    spr_load(SPR_PARCH, 4, 4);
    rect(x, y, w, h, (float)((int)x & 15), (float)((int)y & 15), 1, 1);
}

static void flat(float x, float y, float w, float h, int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    combine_flat();
    env(r, g, b, a);
    gDPLoadTextureTile(gfx_dl_head++, art, G_IM_FMT_RGBA, G_IM_SIZ_16b, ART_W, ART_H, 0, 0, 3, 3, 0,
                       G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 2, 2, G_TX_NOLOD, G_TX_NOLOD);
    rect(x, y, w, h, 0, 0, 1, 1);
    combine_tex();
    env(255, 255, 255, 255);
}
static void outline(float x, float y, float w, float h, int r, int g, int b, int a) {
    flat(x, y, w, 1, r, g, b, a);
    flat(x, y + h - 1, w, 1, r, g, b, a);
    flat(x, y + 1, 1, h - 2, r, g, b, a);
    flat(x + w - 1, y + 1, 1, h - 2, r, g, b, a);
}
static void own(float x, float y, int w, int h, int s) {
    gDPLoadTextureTile(gfx_dl_head++, art, G_IM_FMT_RGBA, G_IM_SIZ_16b, ART_W, ART_H, s, 0, s + w - 1, h - 1, 0,
                       G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    rect(x, y, (float)w, (float)h, (float)s, 0, 1, 1);
}

/* a panel as the menu's pages draw theirs (Menu_UpdateStatsPage): parchment, a black line round it, brown and tan
 * inner edges, and a 3-pixel shadow right and below */
static void panel(float X, float Y, float W, float H) {
    parchment(X, Y, W, H);
    swatch(SPR_BLACK, X + 3, Y + H + 1, W, 3);
    swatch(SPR_BLACK, X + W + 1, Y + 6, 2, H - 2);
    swatch(SPR_BLACK, X - 1, Y - 1, W + 2, 1);
    swatch(SPR_BLACK, X - 1, Y + H, W + 2, 1);
    swatch(SPR_BLACK, X - 1, Y, 1, H);
    swatch(SPR_BLACK, X + W, Y, 1, H);
    swatch(SPR_BROWN, X + 1, Y + 1, W - 3, 1);
    swatch(SPR_BROWN, X + 1, Y + 2, 1, H - 3);
    swatch(SPR_TAN, X + 1, Y + H - 2, W - 2, 1);
    swatch(SPR_TAN, X + W - 2, Y + 1, 1, H - 2);
}

static void el(int id) { nd_2d_element(id, EL_RD2D_ANCHOR_LEFT); }

/* a button: a raised parchment plate (spec 2.1 "Plate"), a gold line round it when pointed at. Element `id` the plate,
 * id + 1 the gold line, id + 2 (left set) what goes on it: a piece that comes and goes has ids of its own, so the
 * pictures between ticks never pair it with another */
static void plate(Rect r, int hover, int id) {
    el(id);
    parchment(r.x, r.y, r.w, r.h);
    flat(r.x, r.y, r.w, 1, 0xF4, 0xEC, 0xD6, 255);
    flat(r.x, r.y + 1, 1, r.h - 1, 0xF4, 0xEC, 0xD6, 255);
    flat(r.x + 1, r.y + r.h - 1, r.w - 1, 1, 0x3A, 0x2E, 0x1C, 255);
    flat(r.x + r.w - 1, r.y + 1, 1, r.h - 2, 0x3A, 0x2E, 0x1C, 255);
    flat(r.x + 1, r.y + r.h, r.w, 1, 0, 0, 0, 255);
    flat(r.x + r.w, r.y + 1, 1, r.h, 0, 0, 0, 255);
    el(id + 1);
    if (hover) outline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 0xF2, 0xC4, 0x2A, 255);
    el(id + 2);
}

/* the menu's digits (8x10, 7 apart: the pages' numbers) */
static int num_w(int v) {
    int n = 1;
    if (v < 0) v = 0;
    while (v >= 10) v /= 10, n++;
    return n * 7 + 1;
}
static void num(int v, float x, float y) {
    char buf[12];
    if (v < 0) v = 0;
    snprintf(buf, sizeof buf, "%d", v);
    for (int i = 0; buf[i]; i++) spr(SPR_DIGIT + buf[i] - '0', x + 7 * i, y);
}
static void num_right(int v, float right, float y) { num(v, right - num_w(v), y); }

/* the HUD's spell-name font (its Latin letters: index 1 the letter, 3 a rim that fills the whole cell) with palettes
 * of ours that keep only the letter: white (the region's name, outlined by drawing it in black around it first), ink
 * (#3E3A32: the plates' labels and "Next spirit" on the parchment) */
static const u16 white_text[16] = { 0x0000, 0xFFFF };
static const u16 black_text[16] = { 0x0000, 0x0001 };
static const u16 ink_text[16] = { 0x0000, 0x39CD };

static const u8 *encode(const char *t, u8 *buf, int cap) { return t ? Port_HudEncode(t, buf, cap) : NULL; }
static int text_w(const u8 *s) {
    int w = 0, adv = 8;
    if (!s) return 0;
    for (; *s != 0xFF; s++) {
        if (*s == 0x83) adv = 6;
        else if (*s == 0x82) w += 4;
        else if (*s & 0x80) w += 1;
        else w += adv;
    }
    return w;
}
static void text(float x, float y, const u8 *s, const u16 *pal) {
    int cur = 8, adv = 8;
    if (!s) return;
    gDPPipeSync(gfx_dl_head++);
    gDPLoadTLUT_pal16(gfx_dl_head++, 0, pal);
    for (; *s != 0xFF; s++) {
        if (*s == 0x82) x += 4;
        else if (*s == 0x83) adv = 6;
        else if (*s & 0x80) x += 1;
        else {
            const int row = (*s & 0x70) >> 4;
            if (row != cur) Hud_LoadTextureTile(&D_803A8748, 0, row * 9, 0xA0, 9), cur = row;
            rect(x, y, 8, 9, (float)((*s & 0xF) * 8), (float)(row * 9), 1, 1);
            x += adv;
        }
    }
}
static void text_centre(float cx, float y, const char *t, const u16 *pal) {
    u8 buf[64];
    const u8 *s = encode(t, buf, sizeof buf);
    if (!s) return;
    const float x = floorf(cx - text_w(s) / 2.0f);
    if (pal == white_text) { /* a 1-pixel black outline (spec 2.1 "HUD numbers") */
        static const s8 off[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
        for (int k = 0; k < 8; k++) text(x + off[k][0], y + off[k][1], s, black_text);
    }
    text(x, y, s, pal);
}

/* a bar as the Status page draws its (the 60x7 frame, the 52x3 fill), stretched to w */
static void bar(float x, float y, float w, float frac) {
    spr_scaled(SPR_BAR, x, y, w, (float)spr_h(SPR_BAR));
    if (frac > 1.0f) frac = 1.0f;
    const float fw = floorf((w - 2.0f) * frac + 0.5f);
    if (fw <= 0) return;
    spr_load(SPR_BAR_FILL, 0, 0);
    rect(x + 1, y + 1, fw, 3, 0, 0, spr_w(SPR_BAR_FILL) / (w - 2.0f), 1);
}

/* the Elements page's exp gauge (Menu_DrawExpGauge's four layers, each two halves), at ORB x ORB */
static void gauge(float x, float y, int v) {
    MenuExpGaugePalettes pals = menu_exp_gauge_tluts;
    MenuExpGaugeTextures tex = menu_exp_gauge_tex;
    const float k = ORB / 56.0f; /* the frame is 56 x 56 (two 28 x 56 halves); the orb 50 x 50 inside it, 3 in */
    const float ix = x + 2, iy = y + 2, isz = ORB - 4;
    filter(1);
    for (int layer = 0; layer < 4; layer++) {
        tlut(pals.v[layer]);
        if (layer == 1) env(255, 255, 255, 0x80);
        for (int j = 0; j < 2; j++) {
            if (layer == 3) {
                load_ci8(tex.v[3][j], 28, 0, 0, 27, 55, 0, 0, 0);
                rect(x + j * 28 * k, y, 28 * k, 56 * k, 0, 0, 1.0f / k, 1.0f / k);
            } else {
                const int top = layer == 2 ? 50 - v / 2 : 0;
                const float sc = 50.0f / isz;
                load_ci8(tex.v[layer][j], 25, 0, 0, 24, 49, 0, 0, 0);
                rect(ix + j * isz * 0.5f, iy + top / sc, isz * 0.5f, (50 - top) / sc, 0, (float)top, sc, sc);
            }
        }
        if (layer == 1) env(255, 255, 255, 255);
    }
    filter(0);
}

static int wobble(int shift) { return menu_wobble_offsets[(menu_frame_counter >> shift) & 0xF]; }

/* the map of a region in the viewport (the picture's 8-row strips, as the map page draws them; scaled by the zoom) */
static void draw_map(const Layout *L, int r, int alpha) {
    if (!map_load(r)) return;
    const Rect in = vp_inner(L);
    const int keep = alpha_mul;
    alpha_mul = alpha_mul * alpha / 255;
    env(255, 255, 255, 255);
    clip(in);
    /* (each strip, the region and the zoom in the ids: a change is a cut, never a slide) */
    el(0x1000 + r * 0x80 + S.zoom * 0x20 + 0x1F);
    tlut(map_tlut[r]);
    if (region_has_map(r)) {
        float ox, oy;
        map_origin(L, &ox, &oy);
        const float z = zooms[S.zoom];
        for (int row = 0; row < MAP_H; row += 8) {
            const int rows = row + 8 <= MAP_H ? 8 : MAP_H - row;
            const float y0 = oy + row * z;
            if (y0 + rows * z <= in.y || y0 >= in.y + in.h) continue;
            el(0x1000 + r * 0x80 + S.zoom * 0x20 + row / 8);
            load_ci8(map_tex[r], MAP_W, 0, row, MAP_W - 1, row + rows - 1, 0, 0, 0);
            rect(ox, y0, MAP_W * z, rows * z, 0, (float)row, 1.0f / z, 1.0f / z);
        }
    } else {
        /* a region with no map: its name panel (the map page's), in the middle */
        const float x = floorf(in.x + (in.w - map_w[r]) * 0.5f), y = floorf(in.y + (in.h - map_h[r]) * 0.5f);
        load_ci8(map_tex[r], map_w[r], 0, 0, map_w[r] - 1, map_h[r] - 1, 0, 0, 0);
        rect(x, y, (float)map_w[r], (float)map_h[r], 0, 0, 1, 1);
    }
    unclip();
    alpha_mul = keep;
    env(255, 255, 255, 255);
}

static void draw_main(const Layout *L) {
    const int fx = L->fx, fy = L->fy;
    u8 buf[64];
    /* the backdrop: the menu's own background (Menu_DrawBackground's strips) over the whole picture, dimmed */
    nd_2d_element(0xF0, EL_RD2D_ANCHOR_LEFT);
    flat(0, 0, (float)L->W, 240, 0x0A, 0x08, 0x04, 255);
    {
        MenuFourWords th = menu_bg_strip_tex_heights, sh = menu_bg_strip_heights;
        MenuFourPointers bt = menu_bg_strip_tex;
        env(0x70, 0x62, 0x4C, 255);
        tlut(menu_bg_tlut);
        const float sx = 0x4A8 / 1024.0f; /* (Menu_DrawBackground's scale; the 64 texels mirrored) */
        float y = 0;
        for (int half = 0; half < 2; half++)
            for (int k = 0; k < 4; k++) {
                const int i = half ? 3 - k : k;
                const float h = sh.v[i] * 240.0f / 196.0f;
                load_ci8(bt.v[i], 64, 0, 0, 63, th.v[i] - 1, 6, 0, 1);
                if (half) rect(0, y, (float)L->W, h, 0, (float)(th.v[i] - 1), sx, -(float)th.v[i] / h);
                else rect(0, y, (float)L->W, h, 0, 0, sx, (float)th.v[i] / h);
                y += h;
            }
        env(255, 255, 255, 255);
    }
    /* the frame */
    nd_2d_element(0xF1, EL_RD2D_ANCHOR_LEFT);
    panel(fx, fy, L->fw, L->fh);
    outline(fx + 3, fy + 3, L->fw - 6, L->fh - 6, 0x6B, 0x56, 0x34, 160); /* the inner hairline */
    /* the map's title on the frame's top edge, over the map */
    spr(SPR_TITLE_MAP, fx + VP_X + L->map_w / 2 - spr_w(SPR_TITLE_MAP) / 2, fy - 9);
    /* the region: arrows and name */
    const int wob = wobble(2);
    const Rect ra = arrow_rect(L, 0), rb = arrow_rect(L, 1);
    const int more = region_step(S.region, 1) != S.region;
    if (more) {
        el(0xE2);
        if (S.hover == T_PREV) outline(ra.x, ra.y, ra.w, ra.h, 0xF2, 0xC4, 0x2A, 255);
        el(0xE3);
        if (S.hover == T_NEXT) outline(rb.x, rb.y, rb.w, rb.h, 0xF2, 0xC4, 0x2A, 255);
        el(0xE1);
        spr(SPR_ARROW_L, ra.x + 2 + wob, ra.y + 2 + (S.pressed_t == T_PREV));
        spr(SPR_ARROW_R, rb.x + 2 - wob, rb.y + 2 + (S.pressed_t == T_NEXT));
    }
    nd_2d_element(0xF2 + (S.region << 8), EL_RD2D_ANCHOR_LEFT);
    text_centre(fx + VP_X + L->map_w / 2.0f, fy + 9, Port_TextRegion(S.region), white_text);
    /* the viewport */
    nd_2d_element(0xF3, EL_RD2D_ANCHOR_LEFT);
    const Rect v = vp_rect(L), in = vp_inner(L);
    outline(v.x, v.y, v.w, v.h, 0x8E, 0x7A, 0x58, 255);
    parchment(in.x, in.y, in.w, in.h);
    flat(in.x, in.y, in.w, 1, 0x9A, 0x8A, 0x68, 255);
    flat(in.x, in.y + 1, 1, in.h - 1, 0x9A, 0x8A, 0x68, 255);
    draw_map(L, S.region, 255);
    if (S.fade_t > 0 && S.fade_from >= 0) draw_map(L, S.fade_from, 255 * S.fade_t / 4);
    /* the player's pin (blinking, as on the map page) and the marker (the map page's quill and its shadow) */
    nd_2d_element(0xF4, EL_RD2D_ANCHOR_LEFT);
    clip(in);
    if (region_has_map(S.region)) {
        float ox, oy, x, y;
        const float z = zooms[S.zoom];
        map_origin(L, &ox, &oy);
        if (S.mark_set && S.mark_r == S.region) {
            static const int drop[3] = { -3, -1, 0 };
            const float mx = floorf(ox + S.mark_x * z), my = floorf(oy + S.mark_y * z);
            swatch(SPR_SHADOW, mx - 2, my - 1, 5, 1);
            spr(SPR_QUILL, mx - 2, my - 12 + drop[S.drop_t < 2 ? S.drop_t : 2]);
        }
        el(0xE8);
        if (player_on_map(S.region, &x, &y) && (menu_frame_counter & 0x20))
            spr(SPR_PIN, floorf(ox + x * z) - 4, floorf(oy + y * z) - 16);
    }
    /* the reticle: where A puts the marker */
    el(0xE9);
    flat(in.x + in.w / 2 - 3, in.y + in.h / 2, 7, 1, 0x3E, 0x3A, 0x32, 128);
    flat(in.x + in.w / 2, in.y + in.h / 2 - 3, 1, 7, 0x3E, 0x3A, 0x32, 128);
    unclip();
    if (region_has_map(S.region)) {
        for (int zi = 1; zi >= 0; zi--) {
            const Rect zr = zoom_rect(L, zi);
            const int off = zi ? S.zoom == 2 : S.zoom == 0;
            plate(zr, S.hover == (zi ? T_ZIN : T_ZOUT) && !off, zi ? 0xD0 : 0xD4);
            const int keep = alpha_mul;
            if (off) alpha_mul = alpha_mul * 40 / 100;
            env(255, 255, 255, 255);
            spr(zi ? SPR_PLUS : SPR_MINUS, zr.x + 2, zr.y + 1 + (S.pressed_t == (zi ? T_ZIN : T_ZOUT)));
            alpha_mul = keep;
            env(255, 255, 255, 255);
        }
    }
    /* the divider */
    nd_2d_element(0xF5, EL_RD2D_ANCHOR_LEFT);
    flat(fx + L->div_x, fy + 20, 1, 176, 0x6B, 0x56, 0x34, 255);
    flat(fx + L->div_x + 1, fy + 20, 1, 176, 0xE8, 0xDC, 0xC0, 255);
    /* Options and Close */
    const Rect ro = opt_rect(L), rc = close_rect(L);
    plate(rc, S.hover == T_CLOSE, 0xDC);
    own(rc.x + 3, rc.y + 3 + (S.pressed_t == T_CLOSE), 7, 7, ART_CROSS_S);
    plate(ro, S.hover == T_OPT, 0xD8);
    {
        const int dn = S.pressed_t == T_OPT;
        const u8 *s = encode("Options", buf, sizeof buf);
        int x = ro.x + 2;
        if (!s) x = ro.x + (ro.w - 7) / 2; /* (Japanese: the gear alone, in the middle) */
        if (L->wide || !s) {
            own(x, ro.y + 3 + dn, 7, 7, ART_GEAR_S);
            x += 9;
        }
        if (s) text(x, ro.y + 2 + dn, s, ink_text);
    }
    /* Status */
    const int cx = fx + L->col_x;
    nd_2d_element(0xF6, EL_RD2D_ANCHOR_LEFT);
    spr(SPR_TITLE_STATUS, cx, fy + 22);
    swatch(SPR_RULE, cx + spr_w(SPR_TITLE_STATUS) + 3, fy + 28, L->col_w - spr_w(SPR_TITLE_STATUS) - 3, 2);
    const ActorStats *st = &player_stats;
    for (int i = 0; i < 4; i++) {
        const int y = fy + 36 + 20 * i, bx = cx + L->stat_bar_x, right = bx + L->stat_bar_w;
        spr(SPR_LABEL_HP + i, cx, y + 2);
        int cur = 0, max = 1, xp = 0, cost = 1;
        switch (i) {
        case 0: cur = st->hp, max = st->max_hp, xp = st->hp_xp, cost = player_stat_xp_costs[st->hp_growth_rank]; break;
        case 1:
            cur = st->mp, max = st->max_mp, xp = st->mp_xp;
            cost = player_stat_xp_costs[st->mp_growth_rank] * ((player_state.flags & 0x4000) ? 3 : 4);
            break;
        case 2:
            cur = st->defense, max = 255, xp = st->defense_xp, cost = player_stat_xp_costs[st->defense_growth_rank];
            if (player_state.flags & 0x2000) cost = (s32)((f32)cost * 0.8);
            break;
        default:
            cur = st->agility, max = 255, xp = st->agility_xp, cost = player_stat_xp_costs[st->agility_growth_rank];
            if (player_state.flags & 0x8000) cost = (s32)((f32)cost * 0.8);
            break;
        }
        if (i < 2) {
            num_right(max, right, y);
            spr(SPR_SLASH, right - num_w(max) - 7, y);
            num_right(cur, right - num_w(max) - 7, y);
        } else {
            num_right(cur, right, y);
        }
        bar(bx, y + 12, L->stat_bar_w, max > 0 ? (float)cur / max : 0); /* (Def and Agi grow to 255: the game's cap) */
        spr(SPR_EXP, cx + L->exp_x, y + 6);
        const int pct = cost > 0 ? xp * 100 / cost : 0;
        spr(SPR_PERCENT, cx + L->col_w - 8, y + 4);
        num_right(pct, cx + L->col_w - 8, y + 4);
    }
    /* Elements */
    nd_2d_element(0xF7, EL_RD2D_ANCHOR_LEFT);
    spr(SPR_TITLE_ELEMENTS, cx, fy + 118);
    swatch(SPR_RULE, cx + spr_w(SPR_TITLE_ELEMENTS) + 3, fy + 124, L->col_w - spr_w(SPR_TITLE_ELEMENTS) - 3, 2);
    for (int e = 0; e < 4; e++) {
        const int y = fy + 132 + 16 * e, lv = st->element_levels[e];
        spr(gem_sprite[e], cx, y + 1);
        num_right(lv, cx + 29, y + 2);
        spr(SPR_SLASH, cx + 28, y + 2);
        num(50, cx + 35, y + 2);
        bar(cx + L->elem_bar_x, y + 5, L->elem_bar_w, lv / 50.0f);
    }
    /* the next spirit: the Elements page's exp gauge */
    {
        const int ox = cx + L->orb_x;
        int v = player_level_exp_table[st->level] ? (int)(st->exp * 100 / player_level_exp_table[st->level]) : 0;
        if (v > 100) v = 100;
        text_centre(ox + L->orb_w / 2.0f, fy + 129, "Next", ink_text);
        text_centre(ox + L->orb_w / 2.0f, fy + 138, "spirit", ink_text);
        gauge(ox + (L->orb_w - ORB) / 2, fy + 148, v);
        const int w = num_w(v) + 8;
        num(v, ox + (L->orb_w - w) / 2.0f, fy + 183);
        spr(SPR_PERCENT, ox + (L->orb_w - w) / 2.0f + num_w(v) - 1, fy + 183);
    }
}

static void draw_modal(const Layout *L) {
    const int mx = modal_x(L), rise = S.modal_t < 4 ? (4 - S.modal_t) / 2 : 0, my = MODAL_Y + rise;
    u8 buf[32];
    nd_2d_element(0xF8, EL_RD2D_ANCHOR_LEFT);
    const int keep = alpha_mul;
    alpha_mul = alpha_mul * (S.modal_t < 4 ? S.modal_t + 1 : 4) / 4;
    flat(0, 0, (float)L->W, 240, 0x0A, 0x08, 0x04, 153);
    nd_2d_element(0xF9, EL_RD2D_ANCHOR_LEFT);
    panel(mx, my, MODAL_W, MODAL_H);
    env(255, 255, 255, 0x80);
    swatch(SPR_BAND, mx + 18, my, 21, MODAL_H);
    env(255, 255, 255, 255);
    spr(SPR_TITLE_OPTIONS, mx + MODAL_W / 2 - spr_w(SPR_TITLE_OPTIONS) / 2, my - 7);
    /* the two rows, as the Options page lays them out */
    spr(SPR_LABEL_TEXT, mx + 9, my + 29);
    spr(SPR_LABEL_SOUND, mx + 13, my + 74);
    swatch(SPR_RULE, mx + 53, my + 56, 167, 2);
    for (int row = 0; row < 2; row++) {
        const int ty = row ? 78 : 32;
        el(0xC0 + row);
        if (S.hover == T_ROW0 + row) {
            Rect r = slider_rect(L, row);
            outline(r.x, r.y + rise, r.w, r.h, 0xF2, 0xC4, 0x2A, 255);
        }
        el(0xC2 + row);
        spr(row ? SPR_SOFT : SPR_SLOW, mx + 68, my + ty - 2 + row);
        spr(row ? SPR_LOUD : SPR_FAST, mx + 193, my + ty - 2 + row);
        spr(SPR_TRACK, mx + 101, my + ty);
        spr(SPR_TRACK_LINE, mx + 102, my + ty + 1);
        spr(0x27, mx + 139, my + ty - 5); /* the middle mark */
        const int p = option_get(row);
        const int id = knob_sprite[row ? menu_options_bgm_row_pos[p][1] : menu_options_top_row_pos[p][1]];
        spr(id, mx + knob_x(row, p), my + ty - 5);
    }
    /* Back */
    Rect rb = back_rect(L);
    rb.y += rise;
    plate(rb, S.hover == T_BACK, 0xCC);
    {
        const int dn = S.pressed_t == T_BACK;
        const u8 *s = encode("Back", buf, sizeof buf);
        if (s) text(rb.x + (rb.w - text_w(s)) / 2, rb.y + 2 + dn, s, ink_text);
        else spr(SPR_ARROW_L, rb.x + 12, rb.y + 1 + dn);
    }
    /* the game's hand cursor on the row */
    el(0xC8);
    const int wob = wobble(1);
    if (S.modal_row < ROW_BACK) spr(SPR_HAND, mx - 10 - wob, my + row_y[S.modal_row]);
    else spr(SPR_HAND, rb.x - 18 - wob, rb.y);
    alpha_mul = keep;
}

int Port_PauseMenuFrame(void) {
    if (!Port_PauseMenuEnabled()) {
        S.shown = 0;
        return 0;
    }
    const Layout L = layout();
    if (!S.shown) on_open(&L);
    if (S.modal_release) {
        S.modal_release = 0;
        menu_flags &= ~0x20;
    }
    frame_input(&L);
    if (S.open_t < 4) S.open_t++;
    if (S.fade_t > 0) S.fade_t--;
    if (S.drop_t < 3) S.drop_t++;
    if (!ND2D_TRACK()) return 1;
    make_art();
    draw_begin();
    alpha_mul = 255 * S.open_t / 4;
    draw_main(&L);
    if (S.modal) draw_modal(&L);
    alpha_mul = 255;
    draw_end();
    return 1;
}

/* ---- the compass dot (Hud_DrawCompass) ------------------------------------------------------------------------- */

int Port_PauseMenuCompassMute(void) { return Port_PauseMenuEnabled(); }

void Port_PauseMenuCompass(void) {
    if (!Port_PauseMenuEnabled()) return;
    const int r = menu_map_region;
    int shown = S.mark_set && S.mark_r == r && region_has_map(r) && menu_map_spot < 0 && !(core_flags & 0x80);
    float wx = 0, wz = 0, dx = 0, dz = 0, ang = 0;
    Vec2f v = { 0.0f, 18.0f };
    if (shown) {
        map_to_world(r, S.mark_x, S.mark_y, &wx, &wz);
        dx = wx - player_state.actor.x;
        dz = wz - player_state.actor.z;
        if (sqrtf(dx * dx + dz * dz) <= 10.0f) shown = 0; /* (there: the game's own rule) */
    }
    if (!shown) {
        S.dot_alpha = 0;
        if (S.dot_logged && log_level()) printf("P1_PAUSE_COMPASS vi=%llu shown=0\n", vi());
        S.dot_logged = 0;
        return;
    }
    /* the game's own way (its dot for the map page's marker): the bearing less the camera's yaw, 18 out, and drawn
     * where it draws its 8x8 diamond */
    ang = Math_Atan2(dx, dz) - camera_state.yaw;
    if (ang < -3.141592653589793) ang += 6.283185307179586;
    else if (ang > 3.141592653589793) ang -= 6.283185307179586;
    Math_RotateVec2(ang, &v);
    if (S.dot_alpha < 255) S.dot_alpha = S.dot_alpha + 51 > 255 ? 255 : S.dot_alpha + 51; /* fades in over 5 ticks */
    const s32 x = (s32)(v.x + 14.0f), y = (s32)(v.y + 13.0f); /* (Hud_DrawTextureTile's s32 arguments) */
    if (log_level() && (!S.dot_logged || !(Port_OsViCount() % 60)))
        printf("P1_PAUSE_COMPASS vi=%llu shown=1 region=%d marker=%.0f,%.0f player=%.0f,%.0f bearing=%.3f yaw=%.3f angle=%.3f "
               "dot=%.1f,%.1f\n", vi(), r, wx, wz, player_state.actor.x, player_state.actor.z, Math_Atan2(dx, dz),
               camera_state.yaw, ang, v.x, v.y);
    S.dot_logged = 1;
    if (!ND2D_TRACK()) return;
    make_art();
    compass_art();
    draw_begin();
    nd_2d_element(0xFA, EL_RD2D_ANCHOR_RIGHT);
    alpha_mul = S.dot_alpha;
    env(255, 255, 255, 255);
    own(hud_draw_origin_x + x, hud_draw_origin_y + y, 8, 8, ART_DOT_S);
    alpha_mul = 255;
    draw_end();
}
