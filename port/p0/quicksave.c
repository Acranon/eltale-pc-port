/* Quick saves (F2, F4): the game's own save, taken where the player stands, kept in a file that any
 * build of the port loads.
 *
 * Saving takes what the save menu's Save takes (SaveMenu_CaptureGameState: the SaveHeader and the
 * chests/spirits/items/bosses block), in the bytes Pak_WriteFile puts on the Controller Pak (the
 * header big-endian), plus what an inn save does not need: the place and heading, the time of day
 * and the random seed. Loading restarts the game's session as the end of the ending does (core_flags
 * 0x8000: the main loop's per-session reset) and then does what the title's Continue does after the
 * save menu's Load (SaveMenu_RestoreGameState, Player_SetSpawnFromSaveTable, D_8007A172 = 2), with the
 * spawn point set to the saved place (entry -1, as the save table's own spawn points are). The
 * gameplay loop then loads the map as for a Continue.
 *
 * File (quicksaves/quick<N>.elq in the data folder), every number little-endian:
 *   0x00 "ELTQUICK"
 *   0x08 u16 format version (1), u16 PC block bytes, u32 record bytes
 *   0x10 u64 FNV-1a hash of everything after the header
 *   0x18 the PC block: u32 flags (0), u64 time saved (Unix seconds), s32 map, s32 sub-area,
 *        f32 x, y, z, heading (as their bits), s32 env_clock, u32 math_rand_seed
 *   then the record: 0x80 bytes of SaveHeader (0x68 used) and 0xE0 of SaveGameStateBlock (0xC8
 *        used), as the game writes them to the Pak.
 * A reader takes the PC block fields it knows and skips the rest, so the block can grow. */
#include "port/boot_probe.h"
#include "port/gfx.h"
#include "port/os.h"
#include "game/core/main.h"
#include "game/audio/audio.h"
#include "game/audio/sound.h"
#include "game/battle/battle.h"
#include "game/env/env.h"
#include "game/map/map.h"
#include "game/math/math.h"
#include "game/player/player.h"
#include "game/save/save_menu.h"
#include "game/text/text_window.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

void port_swap_save_header(u8 *buf); /* game/input/input.c: the SaveHeader to and from the Pak's byte order */

enum { FORMAT_VERSION = 1, HEADER_BYTES = 0x18, PC_BYTES = 44, SAVE_BYTES = 0x80, STATE_BYTES = 0xE0,
       RECORD_BYTES = SAVE_BYTES + STATE_BYTES };

typedef struct QuickSave {
    s32 map, sub;
    f32 x, y, z, rot;
    s32 clock;
    u32 seed;
    u64 when;
    u8 record[RECORD_BYTES]; /* as on the Pak */
} QuickSave;

static QuickSave pending;  /* a quick load, from the request to the session's reset */
static int load_pending;   /* 1: Port_QuickLoadApply puts `pending` in place */
static int check_pending;  /* 1: the first gameplay frame after a load reports where the player is */

/* ---- the file ---- */

static uint64_t fnv(const u8 *p, size_t n) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    while (n--) h = (h ^ *p++) * UINT64_C(0x100000001B3);
    return h;
}
static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void put64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static u32 get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 get64(const u8 *p) { return get32(p) | (u64)get32(p + 4) << 32; }
static u32 fbits(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }
static f32 bitsf(u32 u) { f32 f; memcpy(&f, &u, 4); return f; }

/* The data folder: the window's (ELTALE_DATA_DIR or %APPDATA%\eltale); headless, ELTALE_DATA_DIR or
 * the current folder. */
static void quick_path(char *out, size_t size, int slot, int make) {
    const char *dir = Port_Rt64DataDir();
    const char *env = getenv("ELTALE_DATA_DIR");
    if (!strcmp(dir, ".") && env && *env) dir = env;
    snprintf(out, size, "%s/quicksaves", dir);
    if (make) make_dir(out);
    snprintf(out, size, "%s/quicksaves/quick%d.elq", dir, slot);
}

static int write_file(const char *path, const QuickSave *q) {
    u8 buf[HEADER_BYTES + PC_BYTES + RECORD_BYTES];
    u8 *pc = buf + HEADER_BYTES;
    memset(buf, 0, sizeof buf);
    memcpy(buf, "ELTQUICK", 8);
    buf[8] = FORMAT_VERSION;
    buf[10] = PC_BYTES;
    put32(buf + 12, RECORD_BYTES);
    put32(pc + 0, 0);
    put64(pc + 4, q->when);
    put32(pc + 12, (u32)q->map);
    put32(pc + 16, (u32)q->sub);
    put32(pc + 20, fbits(q->x));
    put32(pc + 24, fbits(q->y));
    put32(pc + 28, fbits(q->z));
    put32(pc + 32, fbits(q->rot));
    put32(pc + 36, (u32)q->clock);
    put32(pc + 40, q->seed);
    memcpy(pc + PC_BYTES, q->record, RECORD_BYTES);
    put64(buf + 16, fnv(buf + HEADER_BYTES, sizeof buf - HEADER_BYTES));
    /* written whole to a new file, then put in place: a failed write leaves the old one */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    ok &= fclose(f) == 0;
    if (!ok) { remove(tmp); return -1; }
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

/* Returns NULL or why the file cannot be loaded. */
static const char *read_file(const char *path, QuickSave *q) {
    static u8 buf[0x10000];
    FILE *f = fopen(path, "rb");
    if (!f) return "no quick save yet";
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < HEADER_BYTES || memcmp(buf, "ELTQUICK", 8)) return "not a quick save";
    if ((buf[8] | buf[9] << 8) != FORMAT_VERSION) return "a quick save of another format";
    u32 pc_bytes = buf[10] | buf[11] << 8, record_bytes = get32(buf + 12);
    if (pc_bytes < PC_BYTES || record_bytes < SAVE_BYTES + sizeof(SaveGameStateBlock) || record_bytes > RECORD_BYTES ||
        n != HEADER_BYTES + pc_bytes + record_bytes)
        return "the quick save is cut short";
    if (get64(buf + 16) != fnv(buf + HEADER_BYTES, n - HEADER_BYTES)) return "the quick save is damaged";
    const u8 *pc = buf + HEADER_BYTES;
    memset(q, 0, sizeof *q);
    q->when = get64(pc + 4);
    q->map = (s32)get32(pc + 12);
    q->sub = (s32)get32(pc + 16);
    q->x = bitsf(get32(pc + 20));
    q->y = bitsf(get32(pc + 24));
    q->z = bitsf(get32(pc + 28));
    q->rot = bitsf(get32(pc + 32));
    q->clock = (s32)get32(pc + 36);
    q->seed = get32(pc + 40);
    memcpy(q->record, pc + pc_bytes, record_bytes);
    if (q->map < 0 || q->map >= 36 || q->sub < 0 || q->sub >= 64) return "the quick save names no map";
    return NULL;
}

/* ---- the game ---- */

static void say(const char *what) {
    fprintf(stderr, "P1_QUICK %s\n", what);
    Port_Rt64Notice(what);
}

/* In the field and in control: the game's own test for opening the Start menu (Core_RunGameplayLoop),
 * and no message, spell input, level-up screen, inn question, area change or scene. Returns NULL or
 * why not. */
static const char *field_blocked(void) {
    if (core_game_mode != 1) return "not in play";
    if (battle_flags & 1) return "in a battle";
    if ((core_flags & 2) || text_active_window_count > 0) return "a message is open";
    if (core_flags & 1) return "the item menu is open";
    if (core_flags & 0xC0) return "the map is changing";
    if ((core_flags & 0x101C) || (core_flags & 0x10000) || D_8007A172 == 4) return "an event is under way";
    if (player_state.actor.animFlags & 1) return "the player is busy";
    if (player_stats.hp == 0) return "the player is down";
    if (map_id < 0) return "no map";
    return NULL;
}

static void quick_save(int slot) {
    const char *why = field_blocked();
    char msg[1200];
    if (why) {
        snprintf(msg, sizeof msg, "Quick save refused: %s", why);
        say(msg);
        return;
    }
    QuickSave q;
    memset(&q, 0, sizeof q);
    /* the save menu's own capture, into its slot 0 and block, which are put back after */
    SaveHeader keep_header = save_menu_save_headers[0];
    SaveGameStateBlock keep_state = save_menu_game_state_block;
    SaveMenu_CaptureGameState(0);
    memcpy(q.record, &save_menu_save_headers[0], sizeof(SaveHeader));
    port_swap_save_header(q.record);
    memcpy(q.record + SAVE_BYTES, &save_menu_game_state_block, sizeof(SaveGameStateBlock));
    save_menu_save_headers[0] = keep_header;
    save_menu_game_state_block = keep_state;
    q.map = map_id;
    q.sub = map_subarea;
    q.x = player_state.actor.x;
    q.y = player_state.actor.y;
    q.z = player_state.actor.z;
    q.rot = player_state.actor.rot;
    q.clock = env_clock;
    q.seed = math_rand_seed;
    q.when = (u64)time(NULL);
    char path[1024];
    quick_path(path, sizeof path, slot, 1);
    if (write_file(path, &q) != 0) {
        snprintf(msg, sizeof msg, "Quick save failed: cannot write %s (%s)", path, strerror(errno));
        say(msg);
        return;
    }
    fprintf(stderr, "P1_QUICKSAVE map=%d sub=%d x=%.2f y=%.2f z=%.2f rot=%.3f hp=%d/%d mp=%d/%d level=%d clock=%d vi=%llu\n",
            q.map, q.sub, q.x, q.y, q.z, q.rot, player_stats.hp, player_stats.max_hp, player_stats.mp,
            player_stats.max_mp, player_stats.level, q.clock, (unsigned long long)Port_OsViCount());
    snprintf(msg, sizeof msg, "Quick saved (slot %d)", slot);
    say(msg);
}

/* Anywhere in play but an area change, or on the title screen: the session's reset puts the rest
 * (battles, messages, the item menu) away as the end of the ending does. */
static int load_allowed(void) { return core_game_mode == 3 || (core_game_mode == 1 && !(core_flags & 0xC0)); }

static void quick_load(int slot) {
    char msg[1200], path[1024];
    if (!load_allowed()) {
        say("Quick load refused: only in play or on the title screen");
        return;
    }
    quick_path(path, sizeof path, slot, 0);
    const char *why = read_file(path, &pending);
    if (why) {
        snprintf(msg, sizeof msg, "Quick load refused: %s", why);
        say(msg);
        return;
    }
    /* leave this loop and reset the session (main.c's outer loop), stopping the music on the way
     * as the ending's end does (0x8240); Port_QuickLoadApply takes it from there */
    load_pending = 1;
    Sfx_StopAllChannels();
    core_game_mode = 0;
    core_flags &= ~0x400; /* no save menu after the loop (an inn's rest) */
    core_flags |= 0x8200;
}

void Port_QuickLoadApply(void) {
    if (!load_pending) return;
    load_pending = 0;
    /* what the save menu's Load does, from its slot 0 and block (put back after) */
    SaveHeader keep_header = save_menu_save_headers[0];
    SaveGameStateBlock keep_state = save_menu_game_state_block;
    u8 header[SAVE_BYTES];
    memcpy(header, pending.record, SAVE_BYTES);
    port_swap_save_header(header);
    memcpy(&save_menu_save_headers[0], header, sizeof(SaveHeader));
    memcpy(&save_menu_game_state_block, pending.record + SAVE_BYTES, sizeof(SaveGameStateBlock));
    core_flags &= ~0x20000; /* the save says (the session's reset keeps it) */
    SaveMenu_RestoreGameState(0);
    save_menu_save_headers[0] = keep_header;
    save_menu_game_state_block = keep_state;
    /* then what Core_RunMenuLoop does after a load, and the title's Continue into play */
    Player_SetSpawnFromSaveTable();
    D_8007A172 = 2;
    Bgm_ApplyVolumeOption();
    core_game_mode = 1;
    /* the quick save's own: the place (entry -1, as the save table's spawn points), the time of
     * day and the random numbers */
    exit_destination.entry = -1;
    player_spawn_x = pending.x;
    player_spawn_z = pending.z;
    player_spawn_rot = pending.rot;
    env_clock = env_light_time = pending.clock;
    math_rand_seed = pending.seed;
    check_pending = 1;
}

/* ELTALE_QUICK_SAVE_AT=VI / ELTALE_QUICK_LOAD_AT=VI (headless tests): a quick save or load in slot 0
 * at the first frame at or after that VI where it is allowed (P1_QUICK says once why it waits). */
static int test_due(const char *name, int *done) {
    if (*done) return 0;
    const char *v = getenv(name);
    if (!v) { *done = 1; return 0; }
    return Port_OsViCount() >= strtoull(v, NULL, 10);
}

void Port_QuickFrame(void) {
    static int save_done, load_done, said_wait;
    if (check_pending && core_game_mode == 1) {
        /* the first frame after a load: where the player is, against the file */
        check_pending = 0;
        f32 dy = player_state.actor.y - pending.y;
        int same = map_id == pending.map && map_subarea == pending.sub && player_state.actor.x == pending.x &&
                   player_state.actor.z == pending.z && player_state.actor.rot == pending.rot && dy < 1.0f && dy > -1.0f;
        fprintf(stderr, "P1_QUICKLOAD map=%d sub=%d x=%.2f y=%.2f z=%.2f rot=%.3f hp=%d/%d mp=%d/%d level=%d clock=%d "
                        "vi=%llu (%s)\n",
                map_id, map_subarea, player_state.actor.x, player_state.actor.y, player_state.actor.z,
                player_state.actor.rot, player_stats.hp, player_stats.max_hp, player_stats.mp, player_stats.max_mp,
                player_stats.level, env_clock, (unsigned long long)Port_OsViCount(), same ? "as saved" : "NOT as saved");
        Port_Rt64Notice(same ? "Quick loaded" : "Quick loaded, but not where it was saved (see the log)");
    }
    int key = Port_Rt64TakeQuickKey();
    if (test_due("ELTALE_QUICK_SAVE_AT", &save_done)) {
        const char *why = field_blocked();
        if (!why) save_done = 1, key = 1;
        else if (!said_wait++) fprintf(stderr, "P1_QUICK save waits: %s\n", why);
    }
    if (test_due("ELTALE_QUICK_LOAD_AT", &load_done) && load_allowed()) load_done = 1, key = 2;
    if (key == 1) quick_save(0);
    else if (key == 2) quick_load(0);
}
