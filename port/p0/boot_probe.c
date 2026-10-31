#include "port/boot_probe.h"
#include "game/core/boot.h"
#include "game/core/main.h"
#include "game/event/cutscene.h"
#ifdef ELTALE_P1_OS
#include "port/gfx.h"
extern void (*Port_GfxTaskHook)(OSTask *task, unsigned frame);
extern void (*Port_AudioTaskHook)(OSTask *task);
extern void (*Port_PiDmaHook)(void *dst, u32 size);
#ifdef ELTALE_P1_AUDIO
#include "port/audio.h"
#endif
#endif
#include "port/p0/host.h"
#ifdef ELTALE_P1_OS
#include "port/controller.h"
#include "port/os.h"
#include "port/pak.h"
#include "port/text.h"
#endif

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(Gfx) == 8, "N64 Gfx commands must stay eight bytes");
_Static_assert(sizeof(Mtx) == 64, "N64 matrix packets must stay 64 bytes");

GfxFrame port_gfx_frames[2];
static jmp_buf boot_done;
static unsigned display_list_words;
static uint64_t display_list_hash;

void Port_P0DisplayListReady(struct GfxFrame *raw_frame) {
    GfxFrame *frame = (GfxFrame *)raw_frame;
    Gfx *dl = frame->data.dl;
    unsigned words = frame->task.list.t.data_size / sizeof(Gfx);
    if (words < 12 || words > ARRAY_COUNT(frame->data.dl))
        Port_HostFatal("invalid first display-list length");
    fprintf(stderr, "P0 DL words=%u last=%08x:%08x expected_end=%02x\n",
            words, dl[words - 1].words.w0, dl[words - 1].words.w1, (u8)G_ENDDL);
    if ((dl[words - 1].words.w0 >> 24) != (u8)G_ENDDL)
        Port_HostFatal("first display list has no end command");
    display_list_words = words;
    display_list_hash = UINT64_C(0xCBF29CE484222325);
    for (unsigned i = 0; i < words; i++) {
        uint32_t pair[2] = { dl[i].words.w0, dl[i].words.w1 };
        for (unsigned j = 0; j < 2; j++) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                display_list_hash ^= (pair[j] >> shift) & 0xFFu;
                display_list_hash *= UINT64_C(0x100000001B3);
            }
        }
    }
#ifdef ELTALE_P1_OS
    Port_OsStop();
#else
    longjmp(boot_done, 1);
#endif
}

#ifdef ELTALE_P1_AUDIO
extern void (*Port_AiBufferHook)(void *buffer, u32 bytes, u32 rate);
#ifdef ELTALE_P1_OS
#include "port/data.h"
#include "game/env/env.h"
#include "game/battle/battle.h"

#include "game/text/text_window.h"

/* ELTALE_MAP_TOUR=1 (no --input): start a New Game, then visit every sub-area of every map, 90
 * frames each, the way an exit does (exit_destination, then the area transition). Entry -1 keeps
 * the player's position. Ends the run after the last map. Its pad presses Start on the intro and
 * title, A while a text window is open (so it never starts a conversation), B in menus, and in
 * battles a C button and A in turn (casting one-element spells). */
static void tour_pad(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    uint64_t vi = Port_OsViCount();
    *stick_x = *stick_y = 0;
    *buttons = 0;
    if (vi % 30 >= 2) return;
    if (core_game_mode == 3 || core_game_mode == 4) *buttons = 0x1000;
    else if (core_game_mode == 2) *buttons = 0x4000;
    else if (battle_flags & 1) {
        /* a one-element spell (a C button, each element in turn) then A to cast it; once the MP
         * is gone A attacks */
        static const u16 c_buttons[4] = { 0x0008, 0x0004, 0x0002, 0x0001 };
        *buttons = (vi / 30) % 2 ? 0x8000 : c_buttons[(vi / 60) % 4];
    } else if (text_active_window_count > 0) *buttons = 0x8000;
}

/* ELTALE_LOAD_TEST=1 (with --pak holding a save): from the title, Continue (stick right, A), load
 * the first save, and print where gameplay starts (P1_LOAD), then stop. */
#include "overlays/title/title.h"
#include "game/map/map.h"
#include "game/player/player.h"
static void load_pad(u16 *buttons, s8 *stick_x, s8 *stick_y) {
    static int moved;
    uint64_t vi = Port_OsViCount();
    *stick_x = *stick_y = 0;
    *buttons = 0;
    if (vi % 30 >= 2) return;
    if (core_game_mode == 4) *buttons = 0x1000;
    else if (core_game_mode == 3) {
        if (title_menu_state == 2 && !moved) { *stick_x = 80; moved = 1; } /* the items run left to right */
        else *buttons = title_menu_state == 2 ? 0x8000 : 0x1000;
    } else if (core_game_mode == 2) *buttons = 0x8000;
}

void Port_MapTourStep(void) {
    static int load_test = -1;
    if (load_test < 0) load_test = getenv("ELTALE_LOAD_TEST") != NULL;
    if (load_test) {
        fprintf(stderr, "P1_LOAD map=%d sub=%d hp=%d/%d level=%d vi=%llu\n", map_id, map_subarea, player_stats.hp,
                player_stats.max_hp, player_stats.level, (unsigned long long)Port_OsViCount());
        load_test = 0;
        Port_OsRequestStop();
        return;
    }
    static int on = -1, map = -1, sub, wait;
    if (on < 0) on = getenv("ELTALE_MAP_TOUR") != NULL;
    static int blocked;
    if (!on) return;
    if ((core_flags & 0x87) || (battle_flags & 1)) {
        if (++blocked == 3000)
            fprintf(stderr, "P1_TOUR blocked at map=%d sub=%d: core_flags=%x battle_flags=%x\n", map, sub,
                    (unsigned)core_flags, battle_flags);
        return;
    }
    blocked = 0;
    if (++wait < 90) return;
    wait = 0;
    if (map < 0) map = 0, sub = 0;
    else if (++sub >= (int)Port_MapAreaSubareaCount()) map++, sub = 0;
    if (map >= 36) {
        fprintf(stderr, "P1_TOUR done\n");
        Port_OsRequestStop();
        return;
    }
    fprintf(stderr, "P1_TOUR map=%d sub=%d vi=%llu\n", map, sub, (unsigned long long)Port_OsViCount());
    exit_destination.map = map;
    exit_destination.subarea = sub;
    exit_destination.entry = -1;
    core_flags |= 0x80;
    core_transition_timer = 0x14;
}
#endif

void Port_OverrideStartMode(void) {
#ifdef ELTALE_P1_OS
    static int done;
    const char *mode = getenv("ELTALE_START_MODE");
    if (done++ || !mode) return;
    core_game_mode = (u16)atoi(mode);
    if (core_game_mode == 5 || core_game_mode == 7) D_8007A172 = 4; /* as after the last boss (boss.c): no HUD */
    if (core_game_mode == 7) core_flags |= 0x40; /* start the tour's first scene */
    fprintf(stderr, "P1_START_MODE %d\n", core_game_mode);
#endif
}

/* A hash of everything the game plays (headless audio check). */
static uint64_t ai_hash = UINT64_C(0xCBF29CE484222325);
static unsigned ai_buffers, ai_nonzero;
static FILE *ai_wav;
static u32 ai_wav_rate, ai_wav_bytes;
static void hash_ai_buffer(void *buffer, u32 bytes, u32 rate) {
    const u8 *p = buffer;
    if (ai_wav) { /* ELTALE_AUDIO_WAV=path: the AI output as a WAV file (16-bit stereo) */
        if (!ai_wav_rate) ai_wav_rate = rate;
        fwrite(buffer, 1, bytes, ai_wav);
        ai_wav_bytes += bytes;
    }
    for (u32 i = 0; i < bytes; i++) {
        ai_hash = (ai_hash ^ p[i]) * UINT64_C(0x100000001B3);
        ai_nonzero += p[i] != 0;
    }
    ai_buffers++;
}
static void put_le(FILE *f, u32 v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 0xFF, f); }
static void ai_wav_close(void) {
    if (!ai_wav) return;
    fseek(ai_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, ai_wav); put_le(ai_wav, 36 + ai_wav_bytes, 4); fwrite("WAVEfmt ", 1, 8, ai_wav);
    put_le(ai_wav, 16, 4); put_le(ai_wav, 1, 2); put_le(ai_wav, 2, 2); put_le(ai_wav, ai_wav_rate, 4);
    put_le(ai_wav, ai_wav_rate * 4, 4); put_le(ai_wav, 4, 2); put_le(ai_wav, 16, 2);
    fwrite("data", 1, 4, ai_wav); put_le(ai_wav, ai_wav_bytes, 4);
    fclose(ai_wav);
    ai_wav = NULL;
}

/* Each screen the Controller Pak manager draws, as a hash of its pixels (the headless check). */
void Port_NoteCpuFrame(u32 framebuffer, s32 state) {
    static unsigned count;
    const u16 *px = (const u16 *)(uintptr_t)framebuffer;
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (u32 i = 0; i < 320 * 240; i++) hash = (hash ^ px[i]) * UINT64_C(0x100000001B3);
    printf("P1_CPU_FRAME n=%u state=%d hash=%016llx\n", ++count, (int)state, (unsigned long long)hash);
    /* ELTALE_DUMP_CPU_FRAMES=1: each as out/cpu_frame_<n>.ppm */
    if (getenv("ELTALE_DUMP_CPU_FRAMES")) {
        char name[64];
        snprintf(name, sizeof name, "out/cpu_frame_%u.ppm", count);
        FILE *f = fopen(name, "wb");
        if (!f) return;
        fprintf(f, "P6 320 240 255\n");
        for (u32 i = 0; i < 320 * 240; i++) {
            u16 p = px[i];
            fputc((p >> 11 & 31) * 255 / 31, f); fputc((p >> 6 & 31) * 255 / 31, f); fputc((p >> 1 & 31) * 255 / 31, f);
        }
        fclose(f);
    }
}
#endif

int main(int argc, char **argv) {
    const char *rom = NULL;
#ifdef ELTALE_P1_OS
    const char *input = NULL, *pak = NULL;
#endif
    unsigned requested_frames = 1;
    int frames_given = 0;
    int fast = 0;
    int window = 0;
    int pak_manager = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
#ifdef ELTALE_P1_OS
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--pak") && i + 1 < argc) pak = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            requested_frames = (unsigned)strtoul(argv[++i], NULL, 10);
            frames_given = 1;
            if (!requested_frames || requested_frames > 1000000)
                Port_HostFatal("--frames must be between 1 and 1000000");
        }
#endif
        else if (!strcmp(argv[i], "--fast")) fast = 1;
        else if (!strcmp(argv[i], "--headless")) { }
#ifdef ELTALE_P1_OS
        else if (!strcmp(argv[i], "--window")) window = 1;
        else if (!strcmp(argv[i], "--pak-manager")) pak_manager = 1;
#endif
        else {
#ifdef ELTALE_P1_OS
            fprintf(stderr, "usage: eltale-p1 --rom path [--window] [--headless --fast --frames N --input script --pak image.mpk]\n"
                            "       [--pak-manager: start in the game's Controller Pak manager]\n");
#else
            fprintf(stderr, "usage: eltale-p0 --rom path [--headless --fast]\n");
#endif
            return 2;
        }
    }
    if (!rom) rom = getenv("ELTALE_ROM");
    if (!rom) Port_HostFatal("supply --rom or ELTALE_ROM");
    if (Port_HostInit(rom) != 0) return 1;
#ifdef ELTALE_P1_OS
    if (window && Port_Rt64Start() != 0) return 1;
    Port_TextInit();
    /* a window keeps its Controller Pak in the data folder unless --pak names one */
    static char default_pak[1024];
    if (window && !pak) {
        snprintf(default_pak, sizeof default_pak, "%s/controller-pak.mpk", Port_Rt64DataDir());
        pak = default_pak;
    }
    if (Port_ControllerLoadScript(input) || Port_PakLoad(pak)) return 1;
    Port_OsInit();
    /* a window runs until it is closed, unless --frames was given */
    Port_OsSetFrameLimit(window && !frames_given ? 0 : requested_frames);
    Port_OsSetFast(fast);
    if (getenv("ELTALE_FRAME_IMAGE")) Port_GfxTaskHook = Port_FrameImageTrace;
    if ((getenv("ELTALE_MAP_TOUR") || getenv("ELTALE_LOAD_TEST")) && !input) {
        extern void (*Port_LivePad)(u16 *, s8 *, s8 *);
        Port_LivePad = getenv("ELTALE_LOAD_TEST") ? load_pad : tour_pad;
    }
#ifdef ELTALE_P1_AUDIO
    Port_AudioTaskHook = (void (*)(OSTask *))Port_RunAudioTask;
    /* the reference microcode: play with it, or check the HLE against it on every task */
    if (getenv("ELTALE_AUDIO_LLE")) Port_AudioTaskHook = (void (*)(OSTask *))Port_RunAudioTaskLle;
    if (getenv("ELTALE_AUDIO_CHECK")) Port_AudioTaskHook = (void (*)(OSTask *))Port_CheckAudioTask;
    Port_PiDmaHook = Port_LleNotePiDma;
    if (!Port_AiBufferHook) Port_AiBufferHook = hash_ai_buffer; /* a window plays it instead */
    if (getenv("ELTALE_AUDIO_WAV") && (ai_wav = fopen(getenv("ELTALE_AUDIO_WAV"), "wb")))
        fseek(ai_wav, 44, SEEK_SET);
#endif
    /* the menu's Reset puts everything back as it is here and boots again */
    if (window) Port_HostSnapshot();
    if (pak_manager) Port_ControllerHoldStartAtBoot();
    for (;;) {
        Boot_Main(0);
        Port_OsRun();
        int reset = window ? Port_Rt64TakeReset() : 0;
        if (!reset) break;
        Port_HostRestore();
        if (Port_PakLoad(pak)) return 1; /* the saves as they are now, not as they were */
        if (reset == 3) Port_ControllerHoldStartAtBoot();
        fprintf(stderr, "P1_RESET%s\n", reset == 3 ? " pak-manager" : "");
    }
    /* the tour and the load test stop themselves */
    int tour = getenv("ELTALE_MAP_TOUR") != NULL || getenv("ELTALE_LOAD_TEST") != NULL;
    if (!window && ((!tour && Port_OsFrameCount() != requested_frames) ||
        (!tour && Port_OsGfxTaskCount() < requested_frames) || (intro_pak_check_done != 1 && !getenv("ELTALE_START_MODE"))))
        Port_HostFatal("P1 OS did not complete the requested intro frames");
#ifdef ELTALE_P1_AUDIO
    ai_wav_close();
    Port_CheckAudioReport();
    printf("P1_AUDIO buffers=%u nonzero_bytes=%u hash=%016llx\n", ai_buffers, ai_nonzero, (unsigned long long)ai_hash);
#endif
    printf("P1_OS_OK frames=%u gfx_tasks=%u threads_run=%u dma_chunks=%u mode=%u last_hash=%016llx\n",
           Port_OsFrameCount(), Port_OsGfxTaskCount(), Port_OsRunCount(),
           Port_HostDmaCount(), core_game_mode, (unsigned long long)Port_OsLastGfxHash());
    return 0;
#else
    if (setjmp(boot_done) == 0) {
        Boot_Main(0);
        Port_HostFatal("Boot_Main returned before a display list");
    }
#endif
    if (Port_HostDmaCount() < 3 || Port_HostThreadCount() < 5 ||
        Port_HostRunThreadCount() !=
#ifdef ELTALE_P1_OS
            5
#else
            2
#endif
        )
        Port_HostFatal("boot skipped DMA or thread creation");
    if (core_game_mode != 4 || intro_pak_check_done != 1 || Port_HostIntroUpdateCount() != 1)
        Port_HostFatal("the first intro update did not complete");
    if (display_list_hash != UINT64_C(0x0CF266FD815BB6A0))
        Port_HostFatal("first intro display-list command hash changed");
    if (!Port_HostRomSpanMatches(0xDA4570, 0x80100000, 0x7080) ||
        !Port_HostRomSpanMatches(0xD3A8B0, 0x80332D30, 0x80) ||
        !Port_HostRomSpanMatches(0xD3A930, 0x803245A0, 0xE790) ||
        !Port_HostRomSpanMatches(0x71920, 0x80095AF0, 0x3AD0) ||
        !Port_HostRomSpanMatches(0x869620, 0x80206000, 0x60D0) ||
        !Port_HostRomSpanMatches(0xDC62B0, 0x8020C0D0, 0x5C3E0))
        Port_HostFatal("boot DMA bytes differ from ROM");
    printf("P0_BOOT_OK intro_updates=%u threads_created=%u threads_started=%u threads_run=%u dma_chunks=%u dma_bytes=%zu dl_words=%u dl_hash=%016llx\n",
           Port_HostIntroUpdateCount(),
           Port_HostThreadCount(), Port_HostStartedThreadCount(), Port_HostRunThreadCount(),
           Port_HostDmaCount(), Port_HostDmaBytes(), display_list_words,
           (unsigned long long)display_list_hash);
    return 0;
}
